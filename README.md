# 1-SPP Denoiser Demo

Unreal Engine 5.8 C++ project for comparing low-sample path tracing, image filters,
experimental neural passes, and renderer-level denoisers. The default scene is
`/Game/Levels/LVL_RT_Room`. Normal game startup enables the 1-SPP path tracer;
enter `gm` in the developer console during play to open the rendering tools.

## Project state

Developer status as of **2026-10-15**:

- The GM panel supports configurable rendering profiles, reference captures, and
  a live before/after slider. Profiles include raw path tracing, SVGF, and
  Lumen + DLSS Ray Reconstruction. The Lumen/RR option compares an entire rendering
  pipeline, so it should not be interpreted as a filter applied to the same input.
- The flythrough benchmark and full-reference metrics are implemented. It exports
  PNG sequences, frame timings, per-run metrics, and summary charts. End-to-end
  GPU capture and visual UI validation remain outstanding.
- Neural placeholder and NNE Candy are experimental demonstration methods. The
  placeholder displays diagnostics; Candy is a style-transfer model, not a
  validated denoising model. Their inclusion does not imply competitive quality.
- Without hardware ray tracing, normal play can use synthetic noise as a visual
  fallback. It is not a physical path-tracing reference, and flythrough benchmarks
  require hardware ray tracing by default. An explicit software-fallback switch
  supports local pipeline testing without a ray-tracing GPU (see below).

### Build and verification status

The current **SL_TestEditor / Win64 / DebugGame** build succeeds with the five
required NVIDIA plugins installed in the project's `Plugins/` directory.
The earlier missing `StreamlineDLSSGBlueprint` rules error came from incomplete
project-local plugin copies and an installed engine rules assembly that did not
include the engine-level copy. Fresh clones still need the external plugin bundle.

The earlier completion pass also passed UnrealHeaderTool, six independent C++
syntax checks without unity/PCH, and seven isolated numerical fixtures for MSE,
PSNR, SSIM, and CNR. Those numerical fixtures exercised the math with lightweight
stand-ins for Unreal containers; they were not engine integration tests. PNG/CSV
capture, renderer restoration, pause/camera behavior, and graph presentation still
need an end-to-end editor test. Local scratch verification files live under
`Saved/FlythroughVerification/` and are not committed.

## Developer setup

1. Install **Unreal Engine 5.8**, Visual Studio 2022 with C++ game-development
   tools, and a Windows SDK. The targets use `BuildSettingsVersion.V7` and
   `EngineIncludeOrderVersion.Unreal5_8`. Win64 with DX12/SM6 is the configured
   development path.
2. Install the UE 5.8 NVIDIA 8.8.0 bundle under project `Plugins/`: `DLSS`,
   `StreamlineNGXCommon`, `StreamlineCore`, `StreamlineDLSSG`, and
   `StreamlineReflex`. Copy each complete plugin, including its `.uplugin` and
   `Source/` directory. `SL_Test.uproject` enables DLSS, DLSSG, and Reflex;
   the other two are their dependencies. NVIDIA plugin files are not shipped in
   this repository. `DLSSBlueprint` is a required C++ module.
3. Keep the project-enabled engine plugins available, including StateTree,
   GameplayStateTree, NeuralRendering, and NNERuntimeRDG. Generate project files
   from `SL_Test.uproject`, then build **SL_TestEditor / Development / Win64** with
   the editor closed.
4. Open `SL_Test.uproject`, let shaders compile, and play the default map. Use a
   supported GPU for the rendering mode being tested. Restart the editor after
   changing startup renderer settings such as the SVGF velocity configuration.

Equivalent PowerShell build command from the repository root (set your engine path):

```powershell
$EngineRoot = 'A:\Program Files\EpicGames\UE_5.8'
$ProjectFile = (Resolve-Path .\SL_Test.uproject).Path
& "$EngineRoot\Engine\Build\BatchFiles\Build.bat" SL_TestEditor Win64 Development "-Project=$ProjectFile" -WaitMutex -NoHotReloadFromIDE -NoUnity
```

## Flythrough benchmark workflow

### Local pipeline testing without hardware ray tracing

During play, enter this developer console command, then open `gm`:

```text
SL.Flythrough.AllowSoftwareFallback 1
```

On a machine without hardware ray tracing, this allows **Render Flythrough** to
use the existing viewport noise simulation. Render `64SPP_GT`, then a test method
such as `1SPP_Noisy` or `1SPP_Gaussian`, and click **Compute Metrics** to exercise
PNG export, timing/metric CSVs, run detection, and charts. The synthetic GT uses
the fallback's `1/sqrt(64)` noise scaling; it does not wait for hardware sample
accumulation. `SL.NoiseFallback.Scale` still controls noise intensity.

The panel displays **SYNTHETIC TEST**. All outputs, including the summary CSV,
go under `renders/software_fallback/<scenario_id>_<map_hash>/`, and each run includes `capture_mode.txt`.
These captures and metrics test the pipeline only; use the normal hardware path
for actual quality/performance comparisons. Synthetic GT never unlocks metrics
for hardware runs. Enabling this switch on a machine already using hardware ray
tracing leaves the normal hardware path selected.

Use `SL.Flythrough.AllowSoftwareFallback 0` to restore the default requirement.
Changing it during a capture does not redirect that run's output. The completed
run remains selected for results; the new switch value applies to the next run.
This is a runtime opt-in; the project default remains `0`.

### Recording and selecting camera paths

The **Camera path** menu in Flythrough Benchmark selects either the level's
existing Sequence scenario or a saved recording for the current map.

1. Start play in any scene with a controllable perspective camera, open `gm`,
   enter a path name, and click **Record Camera Path (30 FPS)**.
2. Close GM to restore game input, then move and look around. The recorder samples
   the player camera's resulting world-space view, so it works with different
   pawn classes and camera offsets. Reopen GM and click **Stop and Save Recording**.
   **Discard Recording** abandons the take. Recordings are limited to 10 minutes.
3. The saved path is selected automatically. Use **Preview Recorded Path** to
   inspect it; preview restores the previous view and pause state when finished
   or stopped. Preview is available for recordings; use Sequencer for authored
   Sequence previews.
4. Render GT and the desired methods as usual. Path changes, recording, and preview
   are blocked during captures and batch runs. Each method uses the exact saved
   frame schedule. `SL.Flythrough.RunAll` uses the current selection in that game
   session; a new session initially selects the level's Sequence.

Recordings are versioned JSON files in `Saved/BenchmarkPaths/`, so they persist
across play sessions without recompiling C++ or creating editor assets. To import
one, copy its JSON into that directory and click **Refresh Camera Paths**. Only
valid files matching the current map's package name appear. Names are display
labels; a generated ID provides identity. The runtime code does not require a
FlythroughScenario actor for recorded paths. Existing Level Sequence behavior and
its results directory remain supported.

Version 1 stores a map package, ID, name, integer FPS, and an array of frames:
`[x, y, z, quaternion_x, quaternion_y, quaternion_z, quaternion_w, fov, aspect_ratio, constrain_aspect]`.
Position uses Unreal world coordinates in centimeters. The recorder interpolates
observed views onto a 30 FPS schedule using linear position/lens interpolation and
quaternion rotation interpolation. Replay reads those frozen frames, independent
of render speed. Low recording FPS or stalls can smooth out motion that was not
observed; preview the take before benchmarking.

Version 1 supports perspective cameras and records pose, FOV, aspect ratio, and
aspect constraint. It uses a standard playback camera with default camera
post-process settings; it does not record camera-specific post-process effects,
CineCamera focus/aperture, pawn animation, or dynamic scene state. World
post-process volumes and benchmark rendering overrides still apply. A paused game
or unsupported camera discards an active take. Scene changes still require a new
GT. Recorded paths are tied to the original map and world coordinate system.

Recorded playback explicitly refreshes the player camera cache at every frame,
including while gameplay is paused.

Recorded results go under
`renders/recorded_<map_hash>/<path_id>_<content_hash>/<method>/run_NNN/`
(or `renders/software_fallback/recorded_...` for synthetic tests). Switching paths
switches GT availability, summaries, and completion indicators. The path content
hash separates revised recordings, and the existing manifest comparison continues
to reject incompatible camera/settings identities.

### Capturing and comparing methods

1. In each benchmark level, place one `SLFlythroughScenario` actor. Assign a
   stable `Scenario Id`, a Level Sequence, and a placed CameraActor (a CineCameraActor
   also works). In Sequencer, bind that camera as a possessable and tag its actor
   binding `BenchmarkCamera`, or change `Camera Binding Tag` on the scenario actor
   to match. Animate only that camera and its camera componw sets the captured frames. Save the
   level and Sequence, start play, then enter `gm`.
   `LVL_RT_Room` already has this setup (`/Game/Benchmark/LS_RT_Room_Flythrough`,
   scenario `RT_Room`): the former interior loop, 300 frames at 30 FPS with an
   80-degree FOV. `Scripts/author_room_flythrough.py` regenerates it:
   `UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin -script=<project>/Scripts/author_room_flythrough.py -unattended -nullrhi`.
2. In **Flythrough Benchmark**, select **64SPP_GT** and click **Render Flythrough**.
   The benchmark evaluates the Sequence at each display frame and keeps that
   position fixed until the screenshot is complete. For a 10-second, 30 FPS run,
   use a 30 FPS display rate and a 300-frame playback range. The Sequence is
   not played in real time; GT can accumulate at each fixed camera position.
3. Render the desired test methods: `1SPP_Noisy`, `1SPP_Gaussian`,
   `1SPP_Bilateral`, `1SPP_NeuralPlaceholder`, and `1SPP_NNE`. SVGF and Lumen/RR
   remain separate GM profiles and are not registered flythrough methods.
4. Click **Compute Metrics** once a complete GT run exists. The operation evaluates
   each method's latest complete run against the latest complete `64SPP_GT` run.
   It currently runs synchronously and can block the UI while processing images.
5. Inspect the summary charts or click **Open Results Folder**. Blue marks the
   selected method, green marks other methods with completed runs, and gray marks
   methods without completed runs. Incomplete newer runs do not hide older complete
   runs. Run detection refreshes when the panel is constructed or a run completes.

To capture everything without the panel, run `SL.Flythrough.RunAll` in the console.
It renders the GT first, then every method, then computes metrics and logs one
`Flythrough result:` line per method (render time, PSNR, SSIM, MSE, CNR). Add
`quit` to exit afterwards, for example:
`UnrealEditor.exe SL_Test.uproject /Game/Levels/LVL_RT_Room -game -windowed -ResX=1280 -ResY=720 -ExecCmds="SL.Flythrough.RunAll quit"`.
The exit code is nonzero if any capture or the metrics step fails. Methods whose
capture identity differs from GT are listed in the metrics status.

The GT sample count defaults to 64. Set `SL.Flythrough.GTSPP <N>` before capturing
to change it; the GT method and its folder become `<N>SPP_GT`, so GTs with different
sample counts are kept apart, and metrics score against the currently configured one.
Existing test-method runs stay valid because the method's SPP is not part of the
capture identity. On the RTX 5070 Ti laptop, a 1280×720 room frame costs about
11 ms per sample, so 1024 SPP is roughly 11–12 s per frame, about an hour for 300 frames.
For example:
`-ExecCmds="SL.Flythrough.GTSPP 1024,SL.Flythrough.RunAll quit"`.

**Resuming.** Each frame's PNG and its timing (`timings_partial.csv`) are written as
it completes. If a capture stops (crash, cancel, closed window), starting the same
method again continues the newest run from its first missing frame, provided the
capture identity, method, SPP and denoiser are unchanged; otherwise a new run starts.
A re-rendered frame always measures its whole render time. Re-running
`SL.Flythrough.RunAll` also skips methods whose identical capture already finished,
so after an interruption the same command picks up where it stopped. A Render click
in the panel still starts a fresh run when the previous one is complete.

**Running in the background.** During capture `t.IdleWhenNotForeground` is forced to
0 (and, in the editor, *Use Less CPU when in Background* is turned off), then
restored afterwards. The window can lose focus or sit behind other windows at full
speed. Do not minimize it: a minimized game draws nothing, so 1 SPP methods wait and
redo the current frame on restore, and minimizing during GT accumulation crashed the
engine's renderer in testing. Resume after such a crash by running the same command.

During capture, gameplay is paused, the camera follows fixed Sequence positions,
path-tracer sampling is locked, and exposure/renderer settings are controlled for
comparison. GT accumulates `SL.Flythrough.GTSPP` samples (64 by default) at each position. Cancel restores captured
settings and leaves partial output on disk for resuming. Resizing the viewport or a frame
exceeding the timeout stops the run. Existing comparison ownership is released
before capture; conflicting GM controls are disabled while the benchmark runs.

Each run writes `run_manifest.json` with the map, Sequence asset, sampled camera
transform/FOV/projection/post-process settings, dimensions, frame
schedule, capture mode, and shared rendering settings. `Compute Metrics` skips a
method when this identity differs from GT; SPP and denoiser method are recorded
but intentionally allowed to differ. Changes to scene content within the same
map package are not detected, so recapture GT after editing the level.

**Runs created before `run_manifest.json` was introduced are ineligible for
scoring.** Render a new GT and all test methods. This also applies to runs under
`renders/software_fallback/`.

```text
renders/
  <scenario_id>_<map_hash>/
    <method_name>/
      run_001/
        frames/
          frame_0000.png
          ...
        timings.csv
        run_manifest.json   # capture identity required for scoring
        metrics.csv          # written by Compute Metrics
    summary_metrics.csv      # latest evaluated complete run per method
    summary_gt_run.txt       # GT run used for the current summary
```

Run IDs increase independently for each method. Completed runs require a valid
manifest, timings CSV, and the corresponding nonempty frame sequence; cancelled
runs are excluded. Capture identity, frame counts/names, and decoded image
dimensions must match GT for scoring.
`renders/` is ignored by Git. Share exported results separately from source changes.

### Reading the measurements

Metrics operate on **tone-mapped 8-bit sRGB PNGs**, not linear HDR radiance.

| Measurement | Definition / interpretation |
| --- | --- |
| MSE | Mean squared RGB error over channels and pixels using 0–255 inputs (MSE range 0–65025); lower is better. |
| PSNR | `10 * log10(255^2 / MSE)` in dB; exact matches use a finite 100 dB cap. |
| SSIM | Luminance SSIM with an 11×11 Gaussian window, sigma 1.5, sampled at stride 4; small images use a smaller odd window. |
| CNR | Linear `sigma_G / sigma_R`, where G is linear GT luminance and R is linear test-minus-GT luminance after sRGB decoding. Residual standard deviation is floored at `1e-8`; this is not a dB value. Values from older versions are not directly comparable. |
| Render time | Wall-clock milliseconds through frame completion/readback, excluding PNG compression and disk writes; includes capture/accumulation overhead and is not a GPU-only profiler measurement. |

Per-frame CSVs include an `avg` row. Summary metric values are arithmetic means of
frame metrics, rather than metrics recomputed over a pooled image sequence. The GT
self-comparison can have a very large CNR because of the residual floor. Charts
show average runtime, MSE, PSNR, SSIM, and CNR.

## Source map and next validation steps

| Location | Responsibility |
| --- | --- |
| `Source/SL_Test/Benchmark/SLFlythroughSubsystem.*` | Method registry, camera/state machine, capture, run detection, metrics orchestration. |
| `Source/SL_Test/Benchmark/SLFlythroughMetrics.*` | PNG decoding, metric calculations, CSV I/O, complete-run selection. |
| `Source/SL_Test/Benchmark/SLFlythroughPanel.*` | Method buttons, GT gating, progress, summary charts. |
| `Source/SL_Test/Benchmark/SLComparison*` | Interactive renderer ownership, same-camera references, live comparison. |
| `Source/SL_Test/Debug/GMTools/SLGMToolConfig.*` | Data-asset schema for GM profiles and presentation. |
| `Source/SL_Test/SLPathTracerSubsystem.*` | Startup path tracing, per-frame reset, software fallback. |
| `Source/SL_Test/SLDenoiser*`, `Shaders/` | Filter implementations and shader passes. |
| `Source/SL_Test/SL_Test.Build.cs` | Module dependencies, including Engine's `ImageUtils.h` API and ImageWrapper for decoding. |

Before treating benchmark numbers as validated, render GT plus a noisy and a
filtered run at the same resolution. Check all 300 PNGs and timing rows, compute
metrics, inspect the CSVs/charts, and compare representative frames visually.
Exercise cancellation, viewport resize, repeated runs, and restoration of the
camera/pause/render settings. Also test a partial newer GT run and a missing or
mismatched frame to verify gating and rejection. Changing shader types/layouts
requires a normal rebuild and editor restart; see the Live Coding notes below.

## Developer console reference

Normal game startup uses the existing 1-SPP path tracer. Enter `gm` during play for the rendering controls:
Raw 1 SPP, 1 SPP + SVGF, Lumen + DLSS RR (End-to-End), Generate High-SPP Reference.
Generate a reference directly from the panel and use Open Results Folder to view it.
Each capture saves `output.png` and `capture.txt` directly under
`Saved/References/YYYY-MM-DD_HH-MM-SS_<samples>SPP/` (local time).
Repeated names receive a numeric suffix to avoid overwriting earlier captures.
Restart the editor after rebuilding to activate the SVGF velocity startup setting.

### This project adds several custom CVars to control the path tracer and the software noise fallback. You can tweak these at runtime by pressing `~` (tilde) in the editor or packaged game to open the developer console.
---
### `SL.PathTracer.AutoEnable`
**Default:** `1`

Controls whether the path tracer is automatically forced on for the game viewport when a game world begins play.
* `0`: Leave the viewport alone (you must enable the path tracer yourself via `ShowFlag.PathTracing 1` or the Path Tracing view mode).
* `1`: Automatically enable path tracing (default).

---

### `SL.PathTracer.ResetEveryFrame`
**Default:** `1`

Controls whether path tracer accumulation restarts on every frame. When enabled, each frame becomes an independent, un-accumulated sample.
* `0`: Standard engine behaviour (the image accumulates while the camera and scene are static).
* `1`: Reset accumulation every frame (default).

---

### `SL.PathTracer.TargetSPP`
**Default:** `1`

Sets the Target Samples Per Pixel (SPP) for the real-time path tracer or its software fallback.
* When hardware ray tracing is available, this CVar drives the engine's built-in `r.PathTracing.SamplesPerPixel`.
* When hardware ray tracing is unavailable, this mathematically scales the variance of the software noise fallback to approximate the variance at the specified sample rate (standard deviation is scaled by `1 / sqrt(TargetSPP)`).

---

### `SL.NoiseFallback.Scale`
**Default:** `1.0`

Controls the overall intensity of the software noise fallback. **Note:** This is only active on machines without hardware ray tracing (DXR) support.
* Scales the standard deviation of the injected Poisson shot noise.
* `0`: Disables the noise injection completely.
* `1.0`: Default synthetic noise intensity; this does not validate physical 1-SPP rendering.
* Changes to this value apply on the render thread and take effect on the very next frame.

---

### `SL.Denoiser.Method`
**Default:** `0`

Selects the denoising algorithm to apply over the 1-spp frame (after noise injection). This provides a modular way to swap out denoising techniques.
* `0`: None (passthrough, no filtering).
* `1`: Separable Gaussian Blur.
* `2`: Edge-preserving Bilateral Filter.
* `3`: Neural Denoiser Placeholder — diagnostic pass that reads all G-Buffer auxiliary inputs (Albedo, Normals, Depth, Motion Vectors) and outputs a debug visualisation. See `SL.Denoiser.Neural.DebugMode`.
* `4`: NNE Candy Model — an ONNX-based neural denoiser integrated directly into the C++ Render Dependency Graph (RDG). Note: because this style transfer network expects display-referred images in [0,255], selecting this method automatically locks auto-exposure (`r.DefaultFeature.AutoExposure 0`) to prevent a blown-out feedback loop.

---

### `SL.Denoiser.GaussianBlur.Sigma`
**Default:** `1.5`

Controls the standard deviation (in pixels) of the Gaussian blur kernel when `SL.Denoiser.Method` is set to `1`.
* Larger values produce a blurrier, smoother image.
* The kernel radius is automatically scaled based on this sigma (`ceil(3 * sigma)` clamped to a maximum radius of 9).

---

### `SL.Denoiser.Bilateral.SigmaSpatial`
**Default:** `2.0`

Controls the spatial standard deviation (in pixels) of the Bilateral Filter kernel when `SL.Denoiser.Method` is set to `2`.
* Larger values increase the size of the blur neighborhood.

---

### `SL.Denoiser.Bilateral.SigmaColor`
**Default:** `0.1`

Controls the color standard deviation of the Bilateral Filter kernel when `SL.Denoiser.Method` is set to `2`.
* Larger values allow pixels with larger color differences to be blended together, reducing the edge-preserving effect.
* Smaller values preserve sharp edges better but may leave more noise.

---

### `SL.Denoiser.Neural.DebugMode`
**Default:** `0`

Controls the visualisation output of the Neural Denoiser Placeholder when `SL.Denoiser.Method` is set to `3`.

* `0`: **Composite diagnostic** — a 2×3 tiled grid showing all five G-Buffer inputs plus a placeholder "denoised" output:
  * Top-left: Noisy Color | Top-right: Albedo
  * Mid-left: World Normals | Mid-right: Linear Depth
  * Bottom-left: Motion Vectors | Bottom-right: Placeholder denoised (demodulate/remodulate)
* `1`: Noisy Color only (with a purple tint to mark it as "processed").
* `2`: Albedo only.
* `3`: World Normals only (remapped to 0–1 for display).
* `4`: Linear Depth only (perceptually normalised).
* `5`: Motion Vectors only (visualised as red/green colour).

---

## Developer Notes

### On-Screen Debug Messages
The active denoising method is continuously displayed in cyan text at the top-left of the viewport. This automatically updates when changing `SL.Denoiser.Method` via the developer console.

### Live Coding Limitations & Custom Denoisers
This project features a fully modular denoising pipeline (`ISLDenoiserPass`). If you decide to add your own denoisers (e.g., SVGF or neural nets), please be aware of Unreal Engine's **Live Coding restrictions** regarding C++ shader classes:

**You MUST close the Unreal Editor and perform a normal IDE build if you:**
1. Introduce a brand new `IMPLEMENT_GLOBAL_SHADER` macro.
2. Add, remove, or reorder parameters inside a `BEGIN_SHADER_PARAMETER_STRUCT`.

Live Coding applies C++ changes while the engine is running, but Unreal requires all shader types and their struct memory layouts to be strictly registered at startup (`PostConfigInit`). Modifying these via Live Coding will result in an assertion failure or a crash (e.g., `Assertion failed: !AreShaderTypesInitialized()` or `required shader parameter [...] was not set`).

*You can freely use Live Coding to hot-reload logic changes inside your C++ `Apply` methods, or use `Ctrl+Shift+.` in the editor to hot-reload `.usf` shader files!*

---

## 1-SPP Denoiser Testing Commands
### These commands are built into UE5 raytracer, and allows the user to test different types of denoisers. Before testing, make sure to run `SL.PathTracer.ResetEveryFrame 0`
---
## Enable 
SL.PathTracer.ResetEveryFrame 0

## Enable spatial denoiser
```
r.PathTracing.TemporalDenoiser 0
r.PathTracing.SpatialDenoiser 1
r.PathTracing.SpatialDenoiser.Type 0
r.PathTracing.Denoiser 1
```

## Enable temporal denoiser
```
r.PathTracing.TemporalDenoiser 1
r.PathTracing.SpatialDenoiser 0
r.PathTracing.SpatialDenoiser.Type 1
r.PathTracing.Denoiser 1
```

## Spatial Denoisers
(Default)
```
r.PathTracing.Denoiser.Name OIDN
```

## Temporal Denoisers
(Default)
```
r.PathTracing.TemporalDenoiser.Name NFOR
```
## GM Tools: one-click test groups

Run the game and enter `gm` in the developer console. Click **Minimize** to collapse the panel to a small **GM** button; click it or enter `gm` again to restore the panel. The **1-SPP Denoiser Testing** group contains **Enable spatial denoiser (OIDN)** and **Enable temporal denoiser (NFOR)** buttons. Each runs the corresponding sequence above, including `SL.PathTracer.ResetEveryFrame 0` and the denoiser name selection.

To configure another button, edit `/Game/Debug/GMTools/DA_RenderGMTools`: add an entry, choose **Command Batch** for `Control`, and add one full console command per `Commands` array element. Commands run in array order on click. The panel shows a short execution status; per-command results are written to the Output Log. Execution stops on failure without rolling back earlier commands. Descriptions and command text remain in the configuration asset and are hidden in the runtime UI. Existing numeric controls remain available.

### Edit the GM panel

Open `Content/Debug/GMTools/DA_RenderGMTools` in the Content Browser. Its `Entries`
array configures GM profile button labels, groups, order, radio options and
rendering settings. `RenderingAction` entries select an operation and contain
ordered `RenderSettings` name/value pairs. The reference label uses `{0}` for the
selected SPP. `RenderingStatus` adds the live status row wherever desired.
Panel title, minimize/restore labels, reference SPP variable and capture timeout
are also fields on this asset. No JSON profiles or separate comparison INI section
are used. C++ implements the reusable controls and rendering operations. The separate
flythrough section defines its methods in C++; camera routes come from Level Sequences or saved recordings.
