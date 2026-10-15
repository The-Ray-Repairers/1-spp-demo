"""NVIDIA FLIP for flythrough benchmark runs (perceptual error; lower is better).

Scores each method's latest complete run under renders/ against the reference
run, frame by frame, with NVIDIA's official implementation (flip-evaluator):

  HDR mode (default): HDR-FLIP on frames_hdr/*.exr, the pre-tonemap linear
                      frames written when SL.Flythrough.CaptureHDR is 1.
  LDR mode:           LDR-FLIP on frames/*.png, the tonemapped frames.

FLIP is not computed in the editor because a faithful C++ port of HDR-FLIP is
large; relMSE, SMAPE and temporal flicker are (Compute Metrics).

    pip install flip-evaluator OpenEXR numpy
    python Scripts/flythrough_flip.py                    # renders/ next to the .uproject, HDR mode
    python Scripts/flythrough_flip.py --mode LDR --stride 5
    python Scripts/flythrough_flip.py --reference 64SPP_GT --methods 1SPP_Noisy 1SPP_Bilateral

Writes <run>/flip_<mode>.csv (per frame) and renders/flip_summary_<mode>.csv.
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from pathlib import Path

import numpy as np

REFERENCE_PREFERENCE = ["512SPP_GT", "64SPP_GT"]


def read_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []


def frame_count(run: Path) -> int | None:
    """Frames in a complete run (timings.csv is written last), else None. Mirrors IsRunComplete."""
    lines = read_lines(run / "timings.csv")
    if len(lines) < 3 or lines[0] != "frame_idx,render_time_ms" or not lines[-1].startswith("avg,"):
        return None
    count = len(lines) - 2
    if any(not (run / "frames" / f"frame_{i:04d}.png").is_file() for i in range(count)):
        return None
    return count


def hdr_complete(run: Path, count: int) -> bool:
    """Mirrors IsHDRRunComplete: hdr_frames.csv lists every frame and every EXR exists."""
    lines = read_lines(run / "hdr_frames.csv")
    return (len(lines) == count + 1 and lines[0] == "frame_idx,pre_exposure"
            and all((run / "frames_hdr" / f"frame_{i:04d}.exr").is_file() for i in range(count)))


def latest_run(method_dir: Path, mode: str) -> tuple[Path, int] | None:
    runs = sorted((d for d in method_dir.glob("run_*") if d.is_dir()),
                  key=lambda d: int(d.name[4:]) if d.name[4:].isdigit() else -1, reverse=True)
    for run in runs:
        count = frame_count(run)
        if count is not None and (mode == "LDR" or hdr_complete(run, count)):
            return run, count
    return None


def load_exr_rgb(path: Path) -> np.ndarray:
    import OpenEXR  # noqa: PLC0415

    with OpenEXR.File(str(path)) as f:
        channels = f.channels()
        if "RGBA" in channels:
            rgb = channels["RGBA"].pixels[..., :3]
        elif "RGB" in channels:
            rgb = channels["RGB"].pixels
        else:
            rgb = np.stack([channels[c].pixels for c in ("R", "G", "B")], axis=-1)
    # HDR-FLIP expects nonnegative linear values.
    return np.ascontiguousarray(np.nan_to_num(rgb.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0).clip(min=0))


def frame_path(run: Path, mode: str, i: int) -> Path:
    return run / "frames_hdr" / f"frame_{i:04d}.exr" if mode == "HDR" else run / "frames" / f"frame_{i:04d}.png"


def flip_frame(flip, ref: Path, test: Path, mode: str) -> float:
    if mode == "HDR":
        return float(flip.evaluate(load_exr_rgb(ref), load_exr_rgb(test), "HDR", applyMagma=False)[1])
    return float(flip.evaluate(str(ref), str(test), "LDR", applyMagma=False)[1])


def main() -> int:
    project_dir = Path(__file__).resolve().parent.parent
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--renders", type=Path, default=project_dir / "renders")
    p.add_argument("--reference", help=f"reference method (default: first complete of {REFERENCE_PREFERENCE})")
    p.add_argument("--mode", choices=["HDR", "LDR"], default="HDR")
    p.add_argument("--stride", type=int, default=1, help="score every Nth frame (HDR-FLIP is slow)")
    p.add_argument("--methods", nargs="*", help="methods to score (default: every method folder)")
    args = p.parse_args()

    try:
        import flip_evaluator as flip  # noqa: PLC0415
    except ImportError:
        print("flip-evaluator is not installed: pip install flip-evaluator OpenEXR numpy", file=sys.stderr)
        return 1

    if not args.renders.is_dir():
        print(f"no renders folder at {args.renders}", file=sys.stderr)
        return 1

    candidates = [args.reference] if args.reference else REFERENCE_PREFERENCE
    reference = next(((name, found) for name in candidates
                      if (found := latest_run(args.renders / name, args.mode))), None)
    if reference is None:
        need = "frames_hdr/ (render with SL.Flythrough.CaptureHDR 1)" if args.mode == "HDR" else "frames/"
        print(f"no complete reference run among {candidates} with {need}", file=sys.stderr)
        return 1
    ref_name, (ref_run, ref_count) = reference
    ref_label = f"{ref_name}/{ref_run.name}"
    print(f"reference: {ref_label} ({ref_count} frames), {args.mode}-FLIP, stride {args.stride}")

    methods = args.methods or sorted(d.name for d in args.renders.iterdir() if d.is_dir())
    frames = list(range(0, ref_count, max(1, args.stride)))
    summary = []
    for method in methods:
        found = latest_run(args.renders / method, args.mode)
        if found is None:
            print(f"  {method}: skipped (no complete run with {args.mode} frames)")
            continue
        run, count = found
        if count != ref_count:
            print(f"  {method}: skipped ({count} frames, reference has {ref_count})")
            continue

        rows = []
        for n, i in enumerate(frames, 1):
            rows.append((i, flip_frame(flip, frame_path(ref_run, args.mode, i), frame_path(run, args.mode, i), args.mode)))
            print(f"\r  {method}: frame {n}/{len(frames)}", end="", flush=True)
        mean = float(np.mean([v for _, v in rows]))
        print(f"\r  {method} ({run.name}): mean FLIP {mean:.4f} over {len(rows)} frames")

        with open(run / f"flip_{args.mode.lower()}.csv", "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["frame_idx", "flip"])
            w.writerows((i, f"{v:.6f}") for i, v in rows)
            w.writerow(["avg", f"{mean:.6f}"])
        summary.append((method, run.name, args.mode, len(rows), f"{mean:.6f}", ref_label))

    if not summary:
        print("nothing scored", file=sys.stderr)
        return 1
    out = args.renders / f"flip_summary_{args.mode.lower()}.csv"
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["method", "run_id", "mode", "frames", "mean_flip", "reference"])
        w.writerows(summary)
    print(f"wrote {os.path.relpath(out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
