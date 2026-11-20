"""Replace every GridSample node in an ONNX model with an equivalent gather-based bilinear sampler.

DirectML's GridSample (ONNX Runtime 1.24 / DirectML 1.15, as shipped with UE 5.8's NNERuntimeORT)
reads past the end of its input tensor. Inside Unreal the hidden-state buffers are exact-size RDG
buffers, so the motion-aware recurrent denoiser (SL.Denoiser.Method 7) hit GPU page faults
(DXGI_ERROR_DEVICE_HUNG). This rewrite computes the same result with Gather on clamped indices, so
no read can leave the tensor.

Supported: mode=bilinear, padding_mode=zeros, align_corners=0 or 1, batch 1, static input shape.
For each sample (gx, gy) in [-1, 1]:
    ix = ((gx + 1) * W - 1) / 2            (align_corners=0)   or  (gx + 1) / 2 * (W - 1)  (=1)
    out = sum over the 4 neighbours of w * X[y, x], with neighbours outside the image contributing 0
Coordinates more than one texel outside the image give 0 either way, so they are clamped to
[-2, W + 1] first; NaN coordinates are treated as outside. The result is identical to GridSample.

    python Scripts/replace_gridsample_onnx.py in.onnx out.onnx

Requires: onnx (and onnxruntime for --verify, which compares both models on the CPU).
"""
import argparse

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper, shape_inference


def const(graph_nodes, name, value, dtype=np.float32):
    arr = np.asarray(value, dtype=dtype)
    graph_nodes.append(helper.make_node("Constant", [], [name], value=numpy_helper.from_array(arr, name + "_v")))
    return name


def replace_one(node, shape_of, idx):
    """Return the nodes that replace one GridSample node."""
    attrs = {a.name: helper.get_attribute_value(a) for a in node.attribute}
    mode = attrs.get("mode", b"bilinear")
    mode = mode.decode() if isinstance(mode, bytes) else mode
    pad = attrs.get("padding_mode", b"zeros")
    pad = pad.decode() if isinstance(pad, bytes) else pad
    align = int(attrs.get("align_corners", 0))
    if mode not in ("bilinear", "linear") or pad != "zeros":
        raise ValueError(f"{node.name}: only bilinear / zeros is supported (got {mode} / {pad})")

    x_name, g_name = node.input[0], node.input[1]
    n, c, h, w = shape_of[x_name]
    gn, ho, wo, two = shape_of[g_name]
    if n != 1 or gn != 1 or two != 2:
        raise ValueError(f"{node.name}: needs batch 1 and a [1, Ho, Wo, 2] grid, got X {shape_of[x_name]} grid {shape_of[g_name]}")

    p = f"/gs{idx}/"
    out = []
    k = lambda s: p + s  # noqa: E731

    # Split the grid into x and y, each [Ho, Wo].
    out.append(helper.make_node("Split", [g_name, const(out, k("split"), [1, 1], np.int64)], [k("gx4"), k("gy4")], axis=3))
    shp_hw = const(out, k("shp_hw"), [ho, wo], np.int64)
    out.append(helper.make_node("Reshape", [k("gx4"), shp_hw], [k("gx")]))
    out.append(helper.make_node("Reshape", [k("gy4"), shp_hw], [k("gy")]))

    def unnormalise(g, size, tag):
        if align:
            a, b = (size - 1) / 2.0, (size - 1) / 2.0             # (g + 1) / 2 * (size - 1)
        else:
            a, b = size / 2.0, (size - 1) / 2.0                   # ((g + 1) * size - 1) / 2
        out.append(helper.make_node("Mul", [g, const(out, k(tag + "_a"), a)], [k(tag + "_m")]))
        out.append(helper.make_node("Add", [k(tag + "_m"), const(out, k(tag + "_b"), b)], [k(tag + "_raw")]))
        # NaN -> outside; then clamp to [-2, size + 1], which never changes a nonzero result.
        out.append(helper.make_node("IsNaN", [k(tag + "_raw")], [k(tag + "_nan")]))
        out.append(helper.make_node("Where", [k(tag + "_nan"), const(out, k(tag + "_out"), -2.0), k(tag + "_raw")], [k(tag + "_fin")]))
        out.append(helper.make_node("Clip", [k(tag + "_fin"), const(out, k(tag + "_lo"), -2.0), const(out, k(tag + "_hi"), size + 1.0)], [k(tag)]))
        return k(tag)

    ix = unnormalise(k("gx"), w, "ix")
    iy = unnormalise(k("gy"), h, "iy")
    out.append(helper.make_node("Floor", [ix], [k("x0")]))
    out.append(helper.make_node("Floor", [iy], [k("y0")]))
    one = const(out, k("one"), 1.0)
    out.append(helper.make_node("Add", [k("x0"), one], [k("x1")]))
    out.append(helper.make_node("Add", [k("y0"), one], [k("y1")]))
    out.append(helper.make_node("Sub", [ix, k("x0")], [k("wx1")]))
    out.append(helper.make_node("Sub", [one, k("wx1")], [k("wx0")]))
    out.append(helper.make_node("Sub", [iy, k("y0")], [k("wy1")]))
    out.append(helper.make_node("Sub", [one, k("wy1")], [k("wy0")]))

    zero = const(out, k("zero"), 0.0)
    wmax = const(out, k("wmax"), w - 1.0)
    hmax = const(out, k("hmax"), h - 1.0)
    wf = const(out, k("wf"), float(w))
    flat_hw = const(out, k("flat_hw"), [ho * wo], np.int64)
    out.append(helper.make_node("Reshape", [x_name, const(out, k("shp_x"), [c, h * w], np.int64)], [k("xflat")]))

    def inside(v, vmax, tag):
        out.append(helper.make_node("GreaterOrEqual", [v, zero], [k(tag + "_ge")]))
        out.append(helper.make_node("LessOrEqual", [v, vmax], [k(tag + "_le")]))
        out.append(helper.make_node("And", [k(tag + "_ge"), k(tag + "_le")], [k(tag + "_in")]))
        out.append(helper.make_node("Clip", [v, zero, vmax], [k(tag + "_c")]))
        return k(tag + "_in"), k(tag + "_c")

    corners = []
    for yn, wyn in (("y0", "wy0"), ("y1", "wy1")):
        y_in, y_c = inside(k(yn), hmax, yn)
        for xn, wxn in (("x0", "wx0"), ("x1", "wx1")):
            x_in, x_c = inside(k(xn), wmax, f"{yn}{xn}_{xn}")
            t = f"{yn}{xn}"
            # Linear index y * W + x, as int64 for Gather.
            out.append(helper.make_node("Mul", [y_c, wf], [k(t + "_yw")]))
            out.append(helper.make_node("Add", [k(t + "_yw"), x_c], [k(t + "_lin")]))
            out.append(helper.make_node("Reshape", [k(t + "_lin"), flat_hw], [k(t + "_linf")]))
            out.append(helper.make_node("Cast", [k(t + "_linf")], [k(t + "_idx")], to=TensorProto.INT64))
            out.append(helper.make_node("Gather", [k("xflat"), k(t + "_idx")], [k(t + "_val")], axis=1))
            # Weight, zero for neighbours outside the image.
            out.append(helper.make_node("And", [y_in, x_in], [k(t + "_ok")]))
            out.append(helper.make_node("Mul", [k(wyn), k(wxn)], [k(t + "_w")]))
            out.append(helper.make_node("Where", [k(t + "_ok"), k(t + "_w"), zero], [k(t + "_wm")]))
            out.append(helper.make_node("Reshape", [k(t + "_wm"), const(out, k(t + "_shpw"), [1, ho * wo], np.int64)], [k(t + "_wf")]))
            out.append(helper.make_node("Mul", [k(t + "_val"), k(t + "_wf")], [k(t + "_contrib")]))
            corners.append(k(t + "_contrib"))
    out.append(helper.make_node("Sum", corners, [k("sum")]))
    out.append(helper.make_node("Reshape", [k("sum"), const(out, k("shp_out"), [1, c, ho, wo], np.int64)], [node.output[0]]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--verify", action="store_true", help="compare the two models with onnxruntime (CPU) on random and extreme grids")
    args = ap.parse_args()

    model = onnx.load(args.src)
    inferred = shape_inference.infer_shapes(model)
    shape_of = {}
    for v in list(inferred.graph.value_info) + list(inferred.graph.input) + list(inferred.graph.output):
        dims = [d.dim_value if d.HasField("dim_value") else None for d in v.type.tensor_type.shape.dim]
        if dims and all(d is not None for d in dims):
            shape_of[v.name] = dims

    # Shape inference cannot always resolve the grid (it may come from Resize / Pad with computed
    # sizes); run the model once on zeros to measure whatever is still unknown.
    missing = {i for n in model.graph.node if n.op_type == "GridSample" for i in n.input[:2] if i not in shape_of}
    if missing:
        shape_of.update(probe_shapes(model, missing))

    new_nodes, count = [], 0
    for node in model.graph.node:
        if node.op_type == "GridSample":
            new_nodes.extend(replace_one(node, shape_of, count))
            count += 1
        else:
            new_nodes.append(node)
    del model.graph.node[:]
    model.graph.node.extend(new_nodes)
    meta = {p.key: p for p in model.metadata_props}
    entry = meta.get("rayrepair_gridsample") or model.metadata_props.add()
    entry.key, entry.value = "rayrepair_gridsample", f"replaced {count} GridSample nodes with Gather (replace_gridsample_onnx.py)"
    onnx.checker.check_model(model)
    onnx.save(model, args.dst)
    print(f"replaced {count} GridSample node(s); wrote {args.dst}")

    if args.verify:
        verify(args.src, args.dst)


def probe_shapes(model, names):
    """Shapes of intermediate tensors, measured by running the model on zeros (CPU)."""
    import copy

    import onnxruntime as ort
    probe = copy.deepcopy(model)
    for name in names:
        probe.graph.output.append(helper.make_tensor_value_info(name, TensorProto.FLOAT, None))
    so = ort.SessionOptions()
    so.log_severity_level = 3
    sess = ort.InferenceSession(probe.SerializeToString(), so, providers=["CPUExecutionProvider"])
    feed = {i.name: np.zeros([d if isinstance(d, int) else 1 for d in i.shape], np.float32) for i in sess.get_inputs()}
    values = sess.run(list(names), feed)
    return {n: list(v.shape) for n, v in zip(names, values)}


def verify(src, dst):
    import onnxruntime as ort
    so = ort.SessionOptions()
    so.log_severity_level = 3
    a = ort.InferenceSession(src, so, providers=["CPUExecutionProvider"])
    b = ort.InferenceSession(dst, so, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(0)
    outs = [o.name for o in a.get_outputs()]
    feeds = {}
    for i in a.get_inputs():
        shape = [d if isinstance(d, int) else 1 for d in i.shape]
        feeds[i.name] = rng.random(shape, dtype=np.float32)
    names = [i.name for i in a.get_inputs()]
    cases = {"random inputs": {}}
    if "motion_px" in names:
        shape = feeds["motion_px"].shape
        cases["motion 0..40 px"] = {"motion_px": (rng.random(shape, dtype=np.float32) - 0.5) * 80}
        cases["motion +-5000 px (far off-screen)"] = {"motion_px": (rng.random(shape, dtype=np.float32) - 0.5) * 10000}
    for label, override in cases.items():
        feed = {**feeds, **override}
        ra, rb = a.run(outs, feed), b.run(outs, feed)
        worst = max(float(np.abs(x - y).max() / max(np.abs(x).max(), 1e-6)) for x, y in zip(ra, rb))
        print(f"  {label:36s} max relative difference over all outputs: {worst:.2e}")
        assert worst < 1e-4, label


if __name__ == "__main__":
    main()
