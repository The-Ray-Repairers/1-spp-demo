## 1-SPP Demo: SL Developer Console Commands

This project adds several custom CVars to control the path tracer and the software noise fallback. You can tweak these at runtime by pressing `~` (tilde) in the editor or packaged game to open the developer console.
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

## 1-SPP Denoiser Testing Commands
These commands are built into UE5 raytracer, and allows the user to test different types of denoisers. Before testing, make sure to run `SL.PathTracer.ResetEveryFrame 0`
---
## OIDN (spatial)
r.PathTracing.TemporalDenoiser 0
r.PathTracing.SpatialDenoiser 1
r.PathTracing.SpatialDenoiser.Type 0
r.PathTracing.Denoiser.Name OIDN
r.PathTracing.Denoiser 1

## NFOR (temporal)
r.PathTracing.TemporalDenoiser 1
r.PathTracing.SpatialDenoiser 0
r.PathTracing.SpatialDenoiser.Type 1
r.PathTracing.TemporalDenoiser.Name NFOR
r.PathTracing.Denoiser 1
