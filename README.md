## 1-SPP Demo: SL Developer Console Commands
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
* When hardware ray tracing is unavailable, this mathematically scales the variance of the software noise fallback to correctly emulate the specified sample rate (standard deviation is scaled by `1 / sqrt(TargetSPP)`).

---

### `SL.NoiseFallback.Scale`
**Default:** `1.0`

Controls the overall intensity of the software noise fallback. **Note:** This is only active on machines without hardware ray tracing (DXR) support.
* Scales the standard deviation of the injected Poisson shot noise.
* `0`: Disables the noise injection completely.
* `1.0`: Default intensity (mathematically matched to 1-spp Monte Carlo variance).
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

## Enable special denoiser
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
