"""Rewrap a Ray Repair format v1 ONNX export: new input encoding, dynamic resolution.

neural-denoiser's export_ue.py wraps a checkpoint as
    ue_format.preprocess -> U-Net -> ue_format.postprocess
at one fixed resolution. Two things can then be wrong for use in this project:

  * The input encoding. A checkpoint trained on NoiseBase (e.g. nb_direct,
    runs/nb_direct/best.pt) learned NoiseBase's encoding, not UE format v1:
      - colour clamped at its 99.5th percentile per frame before demodulation
        (format v1: demodulated irradiance clamped at 100)
      - normals in the camera basis [n.forward, n.right, n.up]
        (format v1: world-space normals)
      - depth = log(1 + 1/distance) min-max normalised per frame, with
        distance the Euclidean camera distance in metres
        (format v1: log1p(z-depth in m) / log1p(1000))
  * The resolution. export_ue.py bakes H x W into the padding and crop, so the
    model only runs at that size; NNE then passes every other viewport through.

This script keeps the trained U-Net body and the output transform, rebuilds the
padding and crop from the input shape (any H x W), and re-creates the input
encoding:

  --encoding noisebase  NoiseBase encoding (for NoiseBase-trained checkpoints).
                        UE must write ue_buffers channels 6-8 as camera-basis
                        normals and channel 9 as Euclidean distance in cm
                        (SLRayRepairPreprocess.usf InputEncoding = 1).
  --encoding ue_v1      The original format v1 encoding (for checkpoints trained
                        with --data ue); only the resolution becomes dynamic.

The percentile is estimated on a strided subsample (--percentile-stride, default
every 4th pixel in x and y) because an exact quantile over a full frame needs a
TopK over millions of values. Everything else is exact.

    pip install onnx onnxruntime numpy
    python Scripts/rewrap_rayrepair_onnx.py nb_direct.onnx nb_direct_noisebase.onnx --encoding noisebase
"""

from __future__ import annotations

import argparse
import math

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

UNET_INPUT = "/model/Pad_output_0"      # padded network input inside export_ue.py graphs
UNET_OUTPUT = "/model/head/Conv_output_0"  # head output before the crop
EPS = 1e-2               # ue_format.EPS == data.EPS (albedo floor)
FIREFLY_CLAMP = 100.0    # ue_format.FIREFLY_CLAMP
MAX_DEPTH_M = 1000.0     # ue_format.MAX_DEPTH_M
NB_PERCENTILE = 99.5     # nb_data.clamp_fireflies
PAD_MULTIPLE = 16        # U-Net downsamples 4x by 2


def unet_body(graph: onnx.GraphProto) -> list[onnx.NodeProto]:
    """Nodes computing UNET_OUTPUT from UNET_INPUT, in graph order."""
    producer = {o: n for n in graph.node for o in n.output}
    initializers = {i.name for i in graph.initializer}
    if UNET_INPUT not in producer or UNET_OUTPUT not in producer:
        raise SystemExit(f"not an export_ue.py graph: missing {UNET_INPUT} or {UNET_OUTPUT}")
    keep, stack = set(), [UNET_OUTPUT]
    while stack:
        name = stack.pop()
        if name == UNET_INPUT or name in initializers or name not in producer:
            continue
        node = producer[name]
        if id(node) not in keep:
            keep.add(id(node))
            stack.extend(node.input)
    return [n for n in graph.node if id(n) in keep]


class Builder:
    def __init__(self):
        self.nodes: list[onnx.NodeProto] = []
        self.inits: list[onnx.TensorProto] = []
        self.n = 0

    def const(self, value, dtype=np.float32) -> str:
        name = f"/rewrap/c{len(self.inits)}"
        self.inits.append(numpy_helper.from_array(np.asarray(value, dtype=dtype), name))
        return name

    def op(self, op_type: str, *inputs: str, out: str | None = None, **attrs) -> str:
        self.n += 1
        out = out or f"/rewrap/{op_type}_{self.n}"
        self.nodes.append(helper.make_node(op_type, list(inputs), [out], **attrs))
        return out

    def channels(self, x: str, start: int, end: int) -> str:
        return self.op("Slice", x, self.const([start], np.int64), self.const([end], np.int64), self.const([1], np.int64))


def encode_ue_v1(b: Builder, color, base, normal, depth):
    irr = b.op("Div", b.op("Clip", color, b.const(0.0)), b.op("Add", b.op("Clip", base, b.const(0.0)), b.const(EPS)))
    irr = b.op("Clip", irr, "", b.const(FIREFLY_CLAMP))
    ch_color = b.op("Log", b.op("Add", b.const(1.0), irr))
    depth_m = b.op("Div", b.op("Clip", depth, b.const(0.0)), b.const(100.0))
    ch_depth = b.op("Div", b.op("Log", b.op("Add", b.const(1.0), depth_m)), b.const(math.log1p(MAX_DEPTH_M)))
    ch_depth = b.op("Clip", ch_depth, b.const(0.0), b.const(1.0))
    return ch_color, normal, ch_depth


def encode_noisebase(b: Builder, color, base, normal, distance, stride: int):
    # clamp_fireflies: per-frame 99.5th percentile of max(colour, 0), estimated on a strided subsample.
    positive = b.op("Relu", color)
    sub = b.op("Slice", positive, b.const([0, 0], np.int64), b.const([2**62, 2**62], np.int64),
               b.const([2, 3], np.int64), b.const([stride, stride], np.int64))
    flat = b.op("Reshape", sub, b.const([-1], np.int64))
    count = b.op("Shape", flat)                                   # [N]
    above = round(1.0 / (1.0 - NB_PERCENTILE / 100.0))            # 1 in 200 values lies above the cap
    k = b.op("Add", b.op("Div", count, b.const([above], np.int64)), b.const([1], np.int64))
    top = b.op("TopK", flat, k, axis=0, largest=1, sorted=1, out="/rewrap/topk_values")
    b.nodes[-1].output.append("/rewrap/topk_indices")
    cap = b.op("ReduceMin", top, keepdims=1)                     # [1]: k-th largest value
    cap = b.op("Where", b.op("Greater", cap, b.const(0.0)), cap, b.const(np.finfo(np.float32).max))
    clamped = b.op("Min", positive, cap)
    # tonemap(noisy / (albedo + EPS)) = log1p(max(., 0))
    irr = b.op("Div", clamped, b.op("Add", b.op("Clip", base, b.const(0.0)), b.const(EPS)))
    ch_color = b.op("Log", b.op("Add", b.const(1.0), b.op("Relu", irr)))
    # NoiseBase log_depth: log(1 + 1/d), d = camera distance (metres); then per-frame min-max.
    dist_m = b.op("Max", b.op("Div", distance, b.const(100.0)), b.const(1e-4))
    disparity = b.op("Log", b.op("Add", b.const(1.0), b.op("Reciprocal", dist_m)))
    lo = b.op("ReduceMin", disparity, keepdims=1)
    hi = b.op("ReduceMax", disparity, keepdims=1)
    ch_depth = b.op("Div", b.op("Sub", disparity, lo), b.op("Max", b.op("Sub", hi, lo), b.const(1e-6)))
    return ch_color, normal, ch_depth


def rewrap(src: onnx.ModelProto, encoding: str, stride: int, expose_net_input: bool = False) -> onnx.ModelProto:
    g = src.graph
    body = unet_body(g)
    used = {i for n in body for i in n.input}
    b = Builder()

    raw = "ue_buffers"
    color, base = b.channels(raw, 0, 3), b.channels(raw, 3, 6)
    normal, depth = b.channels(raw, 6, 9), b.channels(raw, 9, 10)
    if encoding == "noisebase":
        ch_color, ch_normal, ch_depth = encode_noisebase(b, color, base, normal, depth, stride)
    else:
        ch_color, ch_normal, ch_depth = encode_ue_v1(b, color, base, normal, depth)
    # Base colour goes in unclamped, as in both ue_format.preprocess and nb_data.
    net_in = b.op("Concat", ch_color, base, ch_normal, ch_depth, axis=1, out="/rewrap/net_input")

    # Edge-pad bottom/right to a multiple of 16, from the actual input size.
    shape = b.op("Shape", raw)
    hw = b.op("Gather", shape, b.const([2, 3], np.int64), axis=0)            # [H, W]
    m = b.const([PAD_MULTIPLE, PAD_MULTIPLE], np.int64)
    pad_hw = b.op("Mod", b.op("Sub", m, b.op("Mod", hw, m)), m)
    pads = b.op("Concat", b.const([0, 0, 0, 0, 0, 0], np.int64), pad_hw, axis=0)
    b.op("Pad", net_in, pads, mode="edge", out=UNET_INPUT)

    b.nodes.extend(body)

    # Crop back to H x W, then remodulate (ue_format.postprocess).
    cropped = b.op("Slice", UNET_OUTPUT, b.const([0, 0], np.int64), hw, b.const([2, 3], np.int64))
    light = b.op("Clip", b.op("Sub", b.op("Exp", cropped), b.const(1.0)), b.const(0.0))
    b.op("Mul", light, b.op("Add", b.op("Clip", base, b.const(0.0)), b.const(EPS)), out="denoised_rgb")

    inits = [i for i in g.initializer if i.name in used] + b.inits
    inputs = [helper.make_tensor_value_info("ue_buffers", TensorProto.FLOAT, [1, 10, "height", "width"])]
    outputs = [helper.make_tensor_value_info("denoised_rgb", TensorProto.FLOAT, [1, 3, "height", "width"])]
    if expose_net_input:
        outputs.append(helper.make_tensor_value_info("/rewrap/net_input", TensorProto.FLOAT, [1, 10, "height", "width"]))
    graph = helper.make_graph(b.nodes, g.name or "rayrepair", inputs, outputs, inits)
    model = helper.make_model(graph, opset_imports=list(src.opset_import), producer_name="rewrap_rayrepair_onnx")
    model.ir_version = src.ir_version

    meta = {p.key: p.value for p in src.metadata_props}
    meta.update({
        "rayrepair_resolution": "dynamic",
        "rayrepair_input_encoding": encoding,
        "rayrepair_rewrapped_from": meta.get("rayrepair_checkpoint", "unknown"),
    })
    if encoding == "noisebase":
        meta["rayrepair_percentile_stride"] = str(stride)
    for k, v in meta.items():
        entry = model.metadata_props.add()
        entry.key, entry.value = k, v
    onnx.checker.check_model(model, full_check=True)
    return model


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--encoding", choices=["noisebase", "ue_v1"], required=True)
    p.add_argument("--percentile-stride", type=int, default=4)
    args = p.parse_args()
    model = rewrap(onnx.load(args.src), args.encoding, args.percentile_stride)
    onnx.save(model, args.dst)
    print(f"wrote {args.dst}: ue_buffers [1,10,H,W] -> denoised_rgb [1,3,H,W], encoding={args.encoding}")


if __name__ == "__main__":
    main()
