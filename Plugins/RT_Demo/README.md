# RT_Demo

Drop-in Unreal Engine 5.8 plugin with the 1-SPP ray tracing demo tools:

- **GM panel** (`gm` console command): rendering profiles, denoiser selection,
  console-variable controls and command buttons, all driven by a data asset.
- **Live comparison**: a before/after wipe between raw 1-SPP path tracing and the
  selected method (custom filters, SVGF, Lumen + DLSS Ray Reconstruction), plus
  high-SPP reference captures.
- **Flythrough benchmark**: record or author a camera path, capture it per method,
  and compute MSE / PSNR / SSIM / CNR and timing charts.
- **Denoisers**: Gaussian, bilateral, SVGF and NNE neural passes (Ray Repair,
  NB Direct), with their shaders and bundled models.

The plugin is self-contained. It needs no code, Blueprint, or config changes in the
host project: `gm` attaches the tools to whichever local player controller is in play.

## Install

1. Copy this `RT_Demo` folder into `<YourProject>/Plugins/`.
2. Regenerate project files and build the editor target. The plugin has C++ source,
   so a Blueprint-only project needs a C++ toolchain (Visual Studio 2022 with the
   game-development workload); the editor offers to compile it on first open.
3. Open the project. Project plugins are enabled by default; otherwise enable
   **RT Demo** under *Edit > Plugins > Rendering*.
4. Press Play, open the console with `~`, and enter `gm`.

### Requirements

| Requirement | Notes |
| --- | --- |
| Unreal Engine 5.8, Win64, DX12 / SM6 | Uses renderer-private headers, so it is tied to the engine version it was built against. |
| Hardware ray tracing | Needed for real path tracing. Without it the tools fall back to synthetic noise for pipeline testing. |
| `NNERuntimeORT` engine plugin | Enabled automatically as a dependency. Provides `NNERuntimeORTDml` for the neural denoisers. |
| NVIDIA `DLSS` plugin | **Optional.** If installed, the *Lumen + DLSS RR* profile is compiled in and DLSS is enabled automatically. If not, that profile reports itself unavailable and everything else works. |

### Project settings

The plugin does not change the host project's renderer settings. Add these to
`Config/DefaultEngine.ini` (or set them in Project Settings) and restart the editor:

```ini
[/Script/Engine.RendererSettings]
r.RayTracing=True
r.SkinCache.CompileShaders=True

[/Script/WindowsTargetPlatform.WindowsTargetSettings]
DefaultGraphicsRHI=DefaultGraphicsRHI_DX12
+D3D12TargetedShaderFormats=PCD3D_SM6

[SystemSettings]
; SVGF guide pass needs base-pass velocity at startup.
r.VelocityOutputPass=1
; Path tracer as a real-time 1-spp renderer with no built-in denoising.
r.PathTracing.SamplesPerPixel=1
r.PathTracing.MaxBounces=4
r.PathTracing.Denoiser=0
r.PathTracing.SpatialDenoiser=0
r.PathTracing.TemporalDenoiser=0
```

For packaged builds, also cook the plugin's content, which is referenced by path
rather than by other assets:

```ini
; Config/DefaultGame.ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysCook=(Path="/RT_Demo")
```

## Using it

- `gm` opens the panel; **Minimize** collapses it to a button and returns input to
  the game. Entering `gm` again restores it.
- A game world switches its viewport to the 1-SPP path tracer when play begins.
  Set `SL.PathTracer.AutoEnable 0` to leave the viewport alone.
- Recorded camera paths work in any level. Level Sequence paths need one
  `SLFlythroughScenario` actor in the level.
- Output goes to the host project: `renders/` (flythrough runs),
  `Saved/References/` (single-view references), `Saved/BenchmarkPaths/` (recordings).

Console variables and commands keep the `SL.` prefix (`SL.Denoiser.Method`,
`SL.Flythrough.RunAll`, ...). Log output uses the `LogRTDemo` category.

### Customising the panel

The panel reads `/RT_Demo/GMTools/DA_RenderGMTools` by default. To customise it
without editing plugin content, duplicate that asset into your project and select the
copy under *Project Settings > Game > GM Tools > Configuration*.

To drive the panel yourself, call `USLGMToolsComponent::GetOrCreate(PlayerController)`
(Blueprint: **Get Or Create**) and then `OpenPanel` / `TogglePanel`.

### Neural models

`Content/ML` ships `candy-9-clean` (method 4) and `nb_direct` (method 6). Method 5
loads the asset named by `SL.Denoiser.RayRepair.Model`
(default `/RT_Demo/ML/RayRepair_v1_1920x1080`, not bundled). `Scripts/` holds the
ONNX rewrap and import helpers:

```bash
python Plugins/RT_Demo/Scripts/rewrap_rayrepair_onnx.py nb_direct.onnx nb_direct_noisebase.onnx --encoding noisebase
```

```text
UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin -script="<project>/Plugins/RT_Demo/Scripts/import_nb_direct.py C:/path/to/nb_direct_noisebase.onnx" -unattended -nosplash
```

## Layout

| Location | Contents |
| --- | --- |
| `RT_Demo.uplugin` | Descriptor. Module loads at `PostConfigInit` so its shaders register before global shader compilation. |
| `Source/RT_Demo/Public/` | `USLGMToolsComponent`, `USLGMToolConfig` / `USLGMToolSettings`, `USLLiveComparisonUserWidget`, log category. |
| `Source/RT_Demo/Private/Debug/GMTools/` | Panel, command service, comparison widget, `gm` command. |
| `Source/RT_Demo/Private/Benchmark/` | Comparison and flythrough subsystems, recorded paths, metrics, SVGF. |
| `Source/RT_Demo/Private/SLDenoiser*`, `SLPathTracerSubsystem*`, `SLNoiseFallback*` | Denoiser passes, startup path tracing, synthetic fallback. |
| `Shaders/Private/` | Shader sources, mapped to `/Plugin/RT_Demo/Private/`. |
| `Content/GMTools/`, `Content/ML/` | Panel configuration, comparison widget, NNE models. |
| `Scripts/` | Model rewrap and import helpers. |

## Sharing the plugin

Share the folder as source (without `Binaries/` and `Intermediate/`) so each project
builds it against its own engine and its own DLSS availability. A binary build made
with DLSS installed links against the DLSS plugin and will not load without it.

To produce a prebuilt package for the current engine:

```text
<Engine>\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin="<path>\RT_Demo\RT_Demo.uplugin" -Package="<output folder>" -TargetPlatforms=Win64
```
