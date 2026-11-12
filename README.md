# 1-SPP Denoiser Demo

Unreal Engine 5.8 C++ project for comparing low-sample path tracing, image filters,
experimental neural passes, and renderer-level denoisers. The default scene is
`/Game/Levels/LVL_RT_Room`. Normal game startup enables the 1-SPP path tracer;
enter `gm` in the developer console during play to open the rendering tools.

The project provides two workflows:

- **Interactive comparison:** rendering profiles, single-view reference captures,
  and a live before/after slider. Profiles include raw path tracing, SVGF, and
  Lumen + DLSS Ray Reconstruction. Lumen/RR compares an entire rendering pipeline.
- **Flythrough benchmarking:** recorded camera paths or camera-only Level
  Sequences, deterministic frame capture, PNG export, timings, full-reference
  metrics, and summary charts.

The former Neural Placeholder is preserved as the `raw-inputs` diagnostic capture command. NNE Candy is a style-transfer model,
not a validated denoising model. These are experimental demonstration methods.
Hardware ray tracing is required for normal benchmarks. A synthetic noise
fallback supports pipeline testing without a ray-tracing GPU.

## Developer setup

1. Install **Unreal Engine 5.8**, **Visual Studio 2026 (18.0 or newer)** with C++
   game-development tools, and a Windows SDK. The engine's .NET 10 tooling requires
   Visual Studio 18.0 or newer when built inside the IDE. The configured development target is Win64 with
   DX12/SM6. Targets use `BuildSettingsVersion.V7` and
   `EngineIncludeOrderVersion.Unreal5_8`.
2. Install UE 5.8-compatible NVIDIA plugins and their dependencies: `DLSS`,
   `StreamlineNGXCommon`, `StreamlineCore`, `StreamlineDLSSG`, and
   `StreamlineReflex`. Install complete plugin directories, including source and
   `.uplugin` descriptors, in an Unreal plugin search location such as the
   project's `Plugins/` directory. The project enables DLSS, DLSSG, and Reflex;
   `DLSSBlueprint` is a required C++ module. These external plugins are not shipped
   in this repository.
3. Keep the project-enabled engine plugins available, including StateTree,
   GameplayStateTree, NeuralRendering, and NNERuntimeRDG. Generate project files
   from `SL_Test.uproject`, close the editor, and build
   **SL_TestEditor / Development / Win64**.
4. Open the project, let shaders compile, and play the default map. Use a GPU
   supported by the rendering profile being tested.

PowerShell build command from the repository root (adjust the engine path):

```powershell
$EngineRoot = 'A:\Program Files\EpicGames\UE_5.8'
$ProjectFile = (Resolve-Path .\SL_Test.uproject).Path
& "$EngineRoot\Engine\Build\BatchFiles\Build.bat" SL_TestEditor Win64 Development "-Project=$ProjectFile" -WaitMutex -NoHotReloadFromIDE
```

Restart the editor after rebuilding to load the new module. Renderer startup
settings, including `r.VelocityOutputPass=1` for SVGF guides, also require a restart.

If Visual Studio reports that it cannot target .NET 10, open the solution in
Visual Studio 2026 (18.0 or newer). Visual Studio 2022 cannot build those .NET
projects; installing the .NET 10 SDK alone does not change that IDE limitation.
Alternatively, use the PowerShell build command above: Unreal's `Build.bat`
uses its bundled .NET SDK and can use the installed VS 2022 C++ toolchain.
Do not retarget Unreal's C# projects to .NET 9 to suppress this error.

## GM tools and interactive comparison

Enter `gm` during play to open the panel. **Minimize** collapses it to a **GM**
button and returns movement/look input to the viewport. Click that button or enter
`gm` again to restore the panel. Closing the panel restores game-only input and
exits the live comparison overlay.

Rendering Comparisons, Active Method, and Flythrough Benchmark share this order:
**1spp, 512spp, SVGF, DLSS, Ours, KPN/Direct, Open Image Denoise**.
Active Method selects the same full rendering profiles as Rendering Comparisons.
DLSS uses Lumen + DLSS Ray Reconstruction. Ours and KPN/Direct are disabled until their implementations are integrated; tooltips explain
availability. Single-view references are separate from flythrough GT runs. Each
saved reference contains `output.png` and `capture.txt` under:

```text
Saved/References/YYYY-MM-DD_HH-MM-SS_<samples>SPP/
```

Timestamps use local time. A numeric suffix avoids overwriting an existing folder.
Use the panel's results-folder control to locate the capture.

## Diagnostic input grid

During Play or Standalone Game, open the console and enter:

```text
raw-inputs
```

The command preserves the former Neural Placeholder's six-tile grid: scene color,
albedo, world normals, depth, motion vectors, and the placeholder composite.
It is a diagnostic visualization, not a denoising method; the existing grid has
no roughness tile. It temporarily uses deferred rendering from a frozen copy of
the current camera, disables custom filters/noise injection and the comparison
wipe, and saves `raw-inputs.png` plus `capture.txt` under a unique folder in
`Saved/RawInputs/`. Missing renderer buffers use fallback textures; holding the
camera/gameplay can make motion vectors zero. The image is tone-mapped diagnostic
output, not a lossless dump of numerical G-buffer data.

On completion, cancellation (`raw-inputs cancel`), resize, or timeout, the camera,
pause state, renderer settings, and comparison wipe are restored. Concurrent
reference/flythrough captures, recording, and preview are rejected. The command
needs a rendered game viewport; `-nullrhi` cannot produce an image. For the intended
camera position, invoke it after moving the camera in Play, rather than at startup.

## Defining a benchmark camera path

The **Camera path** menu selects a saved recording for the current map or
**Level Sequence (scene default)**. A new game session initially selects the
level's Sequence option. Recorded paths require no scenario actor and can be
created in any scene with a controllable perspective camera.

### Record in-game movement

1. Start play, open `gm`, enter a path name, and click
   **Record Camera Path (30 FPS)**.
2. Minimize or close GM, then move and look around. Recording samples the player
   camera's resulting world-space view, including camera offsets from the pawn.
3. Reopen GM and click **Stop and Save Recording**. The saved path is selected
   automatically. **Discard Recording** abandons the take.

Recordings are limited to 10 minutes. A paused game or an unavailable or
unsupported camera discards an active take. Recording, path changes, and preview
are blocked during benchmark captures and batch runs.

### Preview and trim

1. Select a recording and click **Preview / Trim Recorded Path**. Preview opens
   paused and temporarily takes control of the camera.
2. Drag the timeline to scrub the full recording. Use **Previous Frame / Next
   Frame** for precise selection; the readout shows the frame index and time.
3. Click **Set Start Here** and **Set End Here** at the poses to retain. Both
   endpoints are included, and the range must contain at least two frames.
4. Use **Play Selected Range** and **Pause** to inspect the motion. Playback stays
   within the selected range and holds its final pose. Scrubbing or setting an
   endpoint pauses playback. **Reset Range** restores the full recording.
5. Click **Save Trimmed Copy** to save and select the shortened benchmark path.

A trimmed copy preserves the exact samples and FPS, receives a new path ID, and
has its own benchmark results. The original recording and its results remain
available. **Close Preview** restores the previous view and pause state; closing
without saving does not change the benchmark path. Draft markers persist when
reopening preview in the same panel and selection. Selecting another path or
refreshing/reconstructing the panel resets them. Use Sequencer to preview or edit
Level Sequence paths.

### Delete a recorded path

Select the recording or trimmed copy in **Camera path**, then click **Delete
Selected Path**. Close preview first; deletion is disabled during recording,
preview, captures, metric computation, and batch runs. This removes the recording
from the list and selects **Level Sequence (scene default)**. It does not delete
Level Sequence assets or exported benchmark results.

Deleted JSON files are kept in `Saved/BenchmarkPaths/Deleted/` with a unique suffix.
To restore one, move its JSON back into `Saved/BenchmarkPaths/` and click **Refresh
Camera Paths**. The recording retains its ID, so its results remain associated
with it. To permanently remove it, delete its archived JSON file manually.

### Recording storage and limitations

Recordings are JSON files in `Saved/BenchmarkPaths/<path_id>.json`. They persist
across play sessions without C++ recompilation or editor-asset creation. To import
a recording, copy its JSON into that directory and click **Refresh Camera Paths**.
Only valid files matching the current map's package name appear. The path name is
a display label; the generated ID provides identity.

The current JSON schema (`version: 1`) stores `id`, `name`, `map`, integer `fps`,
and `frames`. Each frame is:

```text
[x, y, z, quaternion_x, quaternion_y, quaternion_z, quaternion_w,
 fov, aspect_ratio, constrain_aspect]
```

Positions use Unreal world coordinates in centimeters; FOV uses degrees. The
recorder samples onto a fixed 30 FPS schedule using linear position/lens
interpolation and quaternion rotation interpolation. Benchmark replay uses the
saved frames independently of rendering speed. Low recording FPS or stalls can
smooth motion that was not observed; inspect the take before benchmarking.

Recorded playback supports perspective pose, FOV, aspect ratio, and aspect
constraint. It uses a standard camera with default camera post-process settings;
it does not record camera-specific post-process effects, CineCamera focus or
aperture, pawn animation, or dynamic scene state. World post-process volumes and
benchmark rendering overrides still apply. Recordings are tied to the map's
package name and world coordinate system.

### Author a Level Sequence

For the Sequence option, place exactly one `SLFlythroughScenario` actor in the
benchmark level and configure:

- A stable **Scenario Id**, using letters, numbers, underscores, or hyphens.
- A Level Sequence and a placed CameraActor or CineCameraActor.
- A possessable camera binding tagged `BenchmarkCamera`, or a matching custom
  **Camera Binding Tag** on the scenario actor.

Animate only the designated camera and its camera component. The Sequence needs
a nonempty, bounded playback range and a positive integer display rate. Spawnable
actors, unrelated actor bindings, event/audio tracks, and subsequences are not
supported. The playback range determines the captured frames.

The default room uses `/Game/Benchmark/LS_RT_Room_Flythrough`, scenario `RT_Room`,
with 300 frames at 30 FPS and an 80-degree FOV. The editor authoring script
`Scripts/author_room_flythrough.py` regenerates the room Sequence and its benchmark
actors; it replaces the assets/actors it creates. Run it with:

```text
UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin -script=<project>/Scripts/author_room_flythrough.py -unattended -nullrhi
```

## Capturing and comparing methods

1. Select a recorded path, trimmed copy, or configured Level Sequence in GM.
2. Select **512spp** and click **Render Flythrough** to capture the default
   ground-truth baseline.
3. Render the desired test methods from the registry below at the same viewport
   resolution and with the same scene and shared rendering settings.
4. Click **Compute Metrics** once a complete GT exists. This compares each method's
   latest complete run against the latest complete GT for the selected path and
   configured GT sample count. Processing is synchronous and can block the UI.
5. Inspect the charts or click **Open Results Folder**. Blue identifies the
   selected method, green identifies other methods with completed runs, and gray
   identifies methods without completed runs.

| Flythrough method | Behavior |
| --- | --- |
| `1spp` | Raw path tracing without denoising; results remain in `1SPP_Noisy`. |
| `512spp` | Undenoised reference; defaults to 512 samples and `512SPP_GT` results. |
| `SVGF` | 1-sample path tracing with same-frame guide buffers and SVGF filtering. |
| `DLSS` | Lumen + DLSS Ray Reconstruction; requires supported NVIDIA hardware and DX12. |
| `Ours` | Disabled: implementation not integrated. |
| `KPN/Direct` | Disabled: implementation not integrated. |
| `Open Image Denoise` | Engine OIDN spatial denoiser on 1-sample path tracing, using albedo and normal guides. |

Open Image Denoise uses the enabled UE `OpenImageDenoise` plugin (Win64 x64).
It denoises on the CPU, including GPU-to-CPU input readback and output upload;
these costs are included in flythrough timings. It uses one warm-up view, not the
32-view temporal settling used by SVGF and DLSS. The input is 1-sample path tracing
with albedo and normal auxiliary inputs, which OIDN also prefilters. Path tracing
hardware support is required; synthetic fallback is rejected. Interactive wipes
render a separate raw view with denoising disabled. `r.OIDN.*` tuning is recorded
in method metadata so changes invalidate resume. Engine plugin errors are reported
in the Output Log under `LogOpenImageDenoise`; successful GPU output still needs
validation on a path-tracing-capable machine.

SVGF and DLSS capture each held camera pose after a camera-cut history reset and
32 rendered warm-up views. Warm-up runs again after resuming or restoring a
minimized window. This is an offline pose capture benchmark, not continuous-motion
temporal playback: timings include warm-up, rendering, and screenshot readback.
Lumen scene caches can still affect convergence; the fixed warm-up is not a
claim of bit-identical renders across sessions. DLSS compares the complete Lumen
rendering pipeline against the path-traced reference, not just a denoising filter.

SVGF requires hardware ray tracing, `r.VelocityOutputPass=1`, and
`r.Substrate.ProjectGBufferFormat=0` (restart after changing startup settings).
Neither SVGF nor DLSS accepts synthetic software fallback. The selector explains
hardware/profile failures in its tooltips. A run aborts if SVGF guide inputs are
missing, its filter does not execute, or DLSS RR becomes disabled.

Capture protocol 2 records effective backend tuning and warm-up in
`run_manifest.json` under `method_settings`. Backend tuning may differ from GT,
but changing it invalidates resume of that method. Re-capture the GT after this
update; protocol 1 captures cannot be scored against protocol 2 captures.
Shared scene quality stays fixed; only backend settings are imported from the
matching GM profile. Completion, cancellation, and startup failure restore the
previous renderer state and release the guide/view extensions.

Batch capture skips unavailable methods and logs the reason. Display labels are separate from output
folder IDs, so the slash in KPN/Direct cannot create nested result directories.

Capture pauses gameplay, evaluates each saved or Sequence camera frame, and holds
that pose until its screenshot completes. Camera sampling, exposure, and shared
renderer settings are controlled for comparison. Capture does not play the path
in real time: high-SPP GT can accumulate at each fixed pose. A 300-frame path at
30 FPS describes 10 seconds of camera motion, regardless of capture wall time.

Set `SL.Flythrough.GTSPP <N>` before capture to choose a GT sample count (1 through
65536). Its display label becomes `<N>spp` and its folder name becomes `<N>SPP_GT`. Test-method SPP and denoiser
choice may differ from GT; matching map, path, frame schedule, dimensions, capture
mode, and shared settings are still required.

### Single-method console capture

Use `SL.Flythrough.Methods` to list actual labels, IDs, and availability. In a
ready game viewport, select a camera path in GM, then use a display label or ID:

```text
SL.Flythrough.Render 1spp
SL.Flythrough.Render 512spp
SL.Flythrough.Render SVGF
SL.Flythrough.Render DLSS
SL.Flythrough.Render OpenImageDenoise
```

Run these one at a time. `Open Image Denoise` is also accepted as a display label.
The GT display label follows `SL.Flythrough.GTSPP`; after overriding it, use the
label reported by `SL.Flythrough.Methods`. Unknown or unavailable names are
rejected without capturing another method. Ours and KPN/Direct remain unavailable;
Gaussian, bilateral, Neural Placeholder and Candy are not benchmark methods.
Single-method capture does not score results or exit. Use GM's Compute Metrics
after capturing a matching GT, or use the batch command below for unattended runs.

### Batch capture and resume

`SL.Flythrough.RunAll [quit]` captures GT first (512 SPP by default), then available
1spp, SVGF, DLSS, and Open Image Denoise runs, computes metrics,
and logs a `Flythrough result:` line per method. It uses the selected path in the
current game session. Add `quit` to exit afterward; failure returns a nonzero exit
code. For a level with a configured Sequence, a standalone example is:

```text
UnrealEditor.exe SL_Test.uproject /Game/Levels/LVL_RT_Room -game -windowed -ResX=1280 -ResY=720 -ExecCmds="SL.Flythrough.GTSPP 512,SL.Flythrough.RunAll quit"
```

Each completed frame writes its PNG and timing to `timings_partial.csv`. Starting
the same method resumes the newest compatible incomplete run at its first missing
frame. Capture identity, method, SPP, and denoiser must match. Re-rendered frames
measure their whole render time. Batch capture skips identical completed runs;
the panel's Render button starts a fresh run when the previous run is complete.
Incomplete newer runs do not hide older complete runs during result selection.

Cancel restores camera/render settings and leaves partial output for resuming.
Viewport resizing or a frame exceeding the 600-second timeout stops capture.
Conflicting GM controls are disabled while capture is active.

Capture disables foreground-idle throttling and restores it afterward. The game
can lose focus or sit behind other windows. Keep its window unminimized: minimized
windows do not render, interrupted frames must be retried, and minimizing during
GT accumulation can destabilize the renderer.

### Synthetic pipeline testing

On a machine without hardware ray tracing, enable:

```text
SL.Flythrough.AllowSoftwareFallback 1
```

The panel displays **SYNTHETIC TEST** and uses viewport noise simulation. Render
GT and a test method to exercise export, timings, metrics, and charts. Synthetic
noise scales by `1/sqrt(SPP)`; GT does not wait for hardware sample accumulation.
`SL.NoiseFallback.Scale` controls noise intensity. These results validate pipeline
behavior, not physical path-tracing quality or performance.

Synthetic results have a separate `renders/software_fallback/` root and never
provide a GT baseline for hardware captures. The switch leaves hardware rendering
selected when hardware ray tracing is available. Its default is `0`; use
`SL.Flythrough.AllowSoftwareFallback 0` to require hardware rendering. Changing
the switch during capture does not redirect that run's output.

## Output layout and capture identity

The **results root** depends on the selected path type and capture mode:

| Path type | Hardware capture results root | Synthetic capture results root |
| --- | --- | --- |
| Recorded path or trimmed copy | `renders/recorded_<map_hash>/<path_id>_<content_hash>/` | `renders/software_fallback/recorded_<map_hash>/<path_id>_<content_hash>/` |
| Level Sequence scenario | `renders/<scenario_id>_<map_hash>/` | `renders/software_fallback/<scenario_id>_<map_hash>/` |

Every results root contains the same method/run structure:

```text
<results_root>/
  <method_name>/
    run_001/
      frames/
        frame_0000.png
        ...
      run_manifest.json
      capture_mode.txt       # synthetic runs only
      timings_partial.csv
      timings.csv
      metrics.csv
  summary_metrics.csv
  summary_gt_run.txt
```

- `map_hash` hashes the map package name, not geometry, materials, or lighting.
- `path_id` is the recording's generated ID. Renaming its display label does not
  change the results location. A trimmed copy receives a new ID.
- `content_hash` fingerprints the recording's FPS, camera poses, FOV, aspect
  settings, and playback revision. Changes to those inputs use a different root.
- `scenario_id` comes from the level's scenario actor. Sequence changes are
  checked through run manifests within that scenario's results root.
- Run numbers increase independently for each method within its results root.
- `capture_mode.txt` marks synthetic runs as pipeline tests. Capture mode is also
  recorded in every run manifest.
- `timings_partial.csv` records frame progress for resuming; `timings.csv` is
  written on completion. `metrics.csv` and the summary files are written by
  **Compute Metrics**. The summary identifies the GT run used for scoring.

`run_manifest.json` records the map, camera source (Sequence asset or recorded
path ID), camera-state fingerprint, frame schedule, image dimensions, capture
mode, shared rendering settings, and method settings. Metrics reject incompatible
capture identities, mismatched frame sequences, and mismatched decoded image
sizes. A complete run requires a valid manifest, timings, and its nonempty PNG
sequence; unfinished runs are ineligible for scoring.

Changing the selected path switches the results root, GT availability, and
summaries. Changes to scene content within the same map are not detected by the
identity checks: recapture GT and test methods after editing the scene.
`renders/` is ignored by Git; share exported results separately from source.

## Reading the measurements

Metrics operate on **tone-mapped 8-bit sRGB PNGs**, not linear HDR radiance.

| Measurement | Definition / interpretation |
| --- | --- |
| MSE | Mean squared RGB error over channels and pixels with 0-255 inputs; range 0-65025. Lower is better. |
| PSNR | `10 * log10(255^2 / MSE)` in dB; exact matches use a finite 100 dB cap. Higher is better. |
| SSIM | Luminance SSIM with an 11-by-11 Gaussian window, sigma 1.5, sampled at stride 4. Small images use a smaller odd window. |
| CNR | Linear `sigma_G / sigma_R`, where G is linear GT luminance and R is linear test-minus-GT luminance after sRGB decoding. Residual standard deviation is floored at `1e-8`; this is not dB. |
| Render time | Wall-clock milliseconds through frame completion/readback, excluding PNG compression and disk writes. Includes capture/accumulation overhead; not a GPU-only profiler measurement. |

Per-frame CSVs include an `avg` row. Summary values are arithmetic means of frame
metrics rather than metrics over a pooled image sequence. GT self-comparison can
produce a very large CNR because of the residual floor. Charts show average
runtime, MSE, PSNR, SSIM, and CNR.

## Developer console reference

Open the console with `~` during play. Benchmark capture temporarily overrides
relevant rendering settings and restores them afterward.

| Command / CVar | Default | Purpose |
| --- | --- | --- |
| `gm` | ? | Open or restore the GM panel. |
| `SL.Flythrough.GTSPP` | `512` | Set GT samples per pixel before capturing. |
| `SL.Flythrough.AllowSoftwareFallback` | `0` | Opt into synthetic benchmarks without hardware ray tracing. |
| `SL.Flythrough.RunAll [quit]` | ? | Capture GT and available methods, compute metrics, optionally exit. |
| `SL.Flythrough.Methods` | ? | List current method labels, stable IDs, and availability. |
| `SL.Flythrough.Render <method>` | ? | Capture the selected path with one method; no scoring or automatic exit. |
| `raw-inputs [cancel]` | ? | Save the current camera diagnostic grid; `cancel` restores without saving. |
| `SL.PathTracer.AutoEnable` | `1` | Enable path tracing automatically when a game world begins play; `0` leaves viewport selection to the user. |
| `SL.PathTracer.ResetEveryFrame` | `1` | Reset accumulation every frame; `0` permits accumulation. |
| `SL.PathTracer.TargetSPP` | `1` | Set hardware path-tracing samples or the synthetic fallback's noise scaling. |
| `SL.NoiseFallback.Scale` | `1.0` | Synthetic noise intensity; `0` disables injected noise. |
| `SL.Denoiser.Method` | `0` | Legacy/debug custom pass: `0` none, `1` Gaussian, `2` bilateral, `3` diagnostic grid, `4` NNE Candy. These values do not select GM renderer backends. |
| `SL.Denoiser.GaussianBlur.Sigma` | `1.5` | Gaussian standard deviation in pixels; larger values blur more. |
| `SL.Denoiser.Bilateral.SigmaSpatial` | `2.0` | Bilateral spatial neighborhood scale. |
| `SL.Denoiser.Bilateral.SigmaColor` | `0.1` | Bilateral color similarity scale; smaller values preserve edges more strongly. |
| `SL.Denoiser.Neural.DebugMode` | `0` | Placeholder visualization: `0` composite, `1` noisy color, `2` albedo, `3` normals, `4` depth, `5` motion vectors. |

Selecting NNE Candy locks auto-exposure because its model expects display-referred
images in the 0-255 range. The active custom denoising method is displayed in the
viewport.

### Built-in path-tracer denoiser experiments

These engine controls are separate from the flythrough method registry and
require the corresponding denoiser plugins to be installed and enabled. The GM
**1-SPP Denoiser Testing** group provides configurable command-batch buttons for
these experiments.

Spatial denoising with OIDN:

```text
SL.PathTracer.ResetEveryFrame 0
r.PathTracing.TemporalDenoiser 0
r.PathTracing.SpatialDenoiser.Type 0
r.PathTracing.Denoiser 1
r.PathTracing.Denoiser.Name OIDN
```

Temporal denoising with NFOR:

```text
SL.PathTracer.ResetEveryFrame 0
r.PathTracing.TemporalDenoiser 1
r.PathTracing.SpatialDenoiser.Type 1
r.PathTracing.Denoiser 1
r.PathTracing.TemporalDenoiser.Name NFOR
```

## Extending and validating the project

### Configure GM tools

Edit `/Game/Debug/GMTools/DA_RenderGMTools` in the Content Browser. Its `Entries`
array configures labels, groups, ordering, radio options, and rendering settings.
`RenderingAction` entries select operations with ordered `RenderSettings` pairs;
`RenderingStatus` inserts the status display. The asset also configures the panel
labels, reference-sample variable, and capture timeout. The reference label can
use `{0}` for the selected SPP.

For a command button, choose **Command Batch** and put one full console command in
each `Commands` array element. Commands execute in order; execution stops on
failure without rolling back earlier commands. The panel displays a short status
and the Output Log records per-command results. The flythrough method registry
is defined in C++; camera paths come from recordings or Level Sequences.

### Source map

| Location | Responsibility |
| --- | --- |
| `Source/SL_Test/Benchmark/SLFlythroughSubsystem.*` | Recording, preview, trimming workflow, camera playback, capture, resume, and metric orchestration. |
| `Source/SL_Test/Benchmark/SLRecordedCameraPath.*` | Runtime recording data, JSON validation/storage, interpolation, and trimmed copies. |
| `Source/SL_Test/Benchmark/SLFlythroughScenario.*` | Level Sequence scenario configuration. |
| `Source/SL_Test/Benchmark/SLFlythroughPanel.*` | Path/method selectors, preview/trim controls, progress, and charts. |
| `Source/SL_Test/Benchmark/SLFlythroughMetrics.*` | PNG decoding, metrics, CSV I/O, and complete-run validation. |
| `Source/SL_Test/Benchmark/SLComparison*` | Interactive rendering profiles, references, and live comparison. |
| `Source/SL_Test/Debug/GMTools/` | GM configuration, commands, panel, and comparison widgets. |
| `Source/SL_Test/SLPathTracerSubsystem.*` | Startup path tracing, sampling controls, and fallback integration. |
| `Source/SL_Test/SLDenoiser*`, `Shaders/` | Filter implementations and shader passes. |
| `Source/SL_Test/SL_Test.Build.cs` | Module dependencies. |

### Shader development

Close the editor and perform a normal build when introducing a new
`IMPLEMENT_GLOBAL_SHADER` registration or changing a shader parameter struct's
layout. The runtime module loads at `PostConfigInit` for shader registration.
Recompile shader source edits in the editor with `Ctrl+Shift+.`. Live Coding is
appropriate only for compatible C++ implementation changes that do not alter
shader registration or parameter layouts.

### Validation

Run the recorded-path automation tests in the editor's Session Frontend, or use
this headless Development-editor command after building:

```text
UnrealEditor-Cmd.exe SL_Test.uproject -unattended -nullrhi -nosplash -nosound -ExecCmds="Automation RunTests SL.Flythrough.RecordedPath" -TestExit="Automation Test Queue Empty" -ReportExportPath="<project>/Saved/FlythroughVerification/RecordedPaths"
```

Use `Automation RunTests SL.Flythrough.Renderer` with the same command to run
backend routing/history-reset and capture-identity regression tests.

The recorded-path test prefix covers recording serialization/interpolation, camera-view updates,
result routing, trimming, and recoverable path deletion. Use the executable matching the built configuration;
DebugGame uses `UnrealEditor-Win64-DebugGame-Cmd.exe`. Inspect test results in the
report or log, not just the process exit code.

Headless tests do not validate GPU output or the visual UI. For end-to-end
validation, capture GT plus noisy and filtered runs at the same resolution, check
all expected PNGs and timing rows, inspect representative images, and compute
metrics. Exercise preview scrubbing, trimmed-copy selection, cancellation, resume,
viewport resize, camera/pause/settings restoration, and incomplete or mismatched
runs. Local verification notes and reports live under
`Saved/FlythroughVerification/` and are not committed. Packaged-game behavior
requires separate validation.
