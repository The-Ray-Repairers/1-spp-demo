# Rendering baselines

Only this project is changed. NVIDIA's official DLSS RR runs on the Lumen/deferred
path as an **end-to-end** comparison. It is not presented as denoising the same
1-SPP input as our path tracer. No frame-generation plugin is installed.

## Install and build

Copy `DLSS` and `StreamlineNGXCommon` from the official UE 5.8 bundle's
`Plugins` directory into this project's `Plugins` directory. Do not overwrite
an existing installation without checking its version. Build the project with
UE 5.8 and restart the editor. No engine files need to be changed.

The vendor plugins are not vendored in Git. Keep the original distribution and
licenses; `Plugins/nvidia-install.json` records the installed bundle descriptors.
The adapted SVGF shaders and their provenance are already included in the project.

## Run

### GM panel workflow

Rebuild and restart the editor to activate `r.VelocityOutputPass=1`. Launch
**Standalone Game** with DX12 and a supported RTX GPU, then enter `gm`.

| Method | Purpose |
| --- | --- |
| Raw 1 SPP | No-denoising control |
| 1 SPP + Our Denoiser | Method under evaluation; implementation to be selected |
| 1 SPP + SVGF | Denoising baseline using the same path tracer |
| High-SPP Reference | Reference image from the same path tracer at the selected SPP |
| Lumen + DLSS RR (End-to-End) | Separate comparison of complete rendering pipelines |

Set **Target Samples Per Pixel** and click **Apply** before capture (try 32 or 64
for a quick preview). The reference button displays the selected SPP. Capture
locks this value and saves after accumulation reaches it; the profile no
longer forces 4096 SPP. Low-SPP references can remain noisy. Raw and SVGF remain
1 SPP during comparison.

Use **Generate High-SPP Reference** to capture a reference at the current camera.
**Cancel / Restore Settings** stops capture and restores settings. **Open Results
Folder** opens the output folder containing the reference image and metadata.
The batch Save Comparison Results action has been removed; method buttons only
switch the live preview. Reference generation remains available separately.

Raw and SVGF use the same 1-SPP path tracer configuration. Lumen + DLSS RR uses
its own supported rendering path and must not be interpreted as a denoiser-only
comparison. The current captures do not prove identical random noisy inputs.

Check that the reference reaches the selected Target Samples Per Pixel and that
SVGF reduces static noise. Inspect motion and disocclusion visually. Same-frame
SVGF input checks remain active at runtime and log any mismatch.

Capture freezes the current camera and pauses the world. It uses the current
viewport size. Profiles contain rendering settings.
Save the level and keep camera, resolution and settings consistent between runs.
Do not resize during capture. Materials or effects that ignore world pause may
continue animating. SVGF is validated for opaque nonmetal surfaces.

Labels, profiles and timeout are configurable under **Project
Settings > Game > Rendering Comparison** (`DefaultGame.ini`). Rendering parameters
are in `Config/Benchmarks`. Standalone editor-game operation has been verified;
packaged builds have not been verified.

## SVGF input contract

Source: local Falcor `SVGFPass`, commit
`759aad033ff610fb0d82c74f7e0a508d0096d5f2`. Its BSD-style notice must be retained.
The shader pipeline subtracts emission, divides by albedo, reprojects with motion
and geometric guides, estimates moments/variance, runs a-trous filtering, then
multiplies albedo and restores emission.

The current UE path-tracing denoiser interface supplies color, denoiser albedo,
normal, depth, variance and a flow texture. It does not supply a separate emission
texture or the Falcor PositionNormalFwidth input. UE's denoiser albedo/normal are
accumulated along paths; they must not automatically be treated as Falcor's
surface guides. UE FlowTex is an output of the selected plugin's motion-estimation
callback, not automatically Falcor-compatible geometry motion vectors.

The project-side implementation below now supplies raster surface guides and
geometric motion to an adaptation of Falcor SVGF. It does not substitute UE's
path-accumulated albedo or optical flow for Falcor's inputs. No engine patch is
used. Its verified scope is the controlled opaque room/cube tests, not arbitrary
UE materials or scenes.

## Merged auxiliary pass and Falcor SVGF

`Source/SL_Test/Benchmark/SLGuideProbe.*` builds one project-owned
`FCustomRenderPassBase` per main view. One depth/base-pass render exports normal
and depth, diffuse albedo, emission, and motion.
It supplies a persistent view state and disables transparent composition.
The project uses Renderer Private/Internal headers from the installed UE 5.8
distribution; this introduces a version dependency, but requires no engine edit.

**Motion fix:** UE's default depth-pass velocity path explicitly disables binding
the velocity target in custom passes. The project selects
`r.VelocityOutputPass=1` in DefaultEngine.ini. This moves velocity output to the
base pass and requires restarting the editor. The first run may compile material
shaders. No engine installation changes are needed.

`SLGuideDenoiser` checks the RDG builder, render frame and texture dimensions at
the actual PT denoiser entry point. A mismatch is logged and bypasses filtering. Full float texture dumps and the
old probe-only path have been removed.
The GM SVGF mode invokes the Falcor filter and retains the existing 1-SPP
path tracer and four-bounce limit.

**Sampling contract:** the new SVGF, raw control and reference profiles set
`r.PathTracing.FilterWidth=0`. UE then uses the pixel center for primary rays,
matching the raster guides with TAA/DOF off. This is a deliberate test condition,
not proof of alignment for the default stochastic pixel filter, DOF, transparency,
displacement or differing raster/ray-tracing geometry. Keep the same filter width and bounce count when editing the comparison profiles.

**Albedo contract:** Falcor `PathTracer.albedo` resolves primary-surface diffuse
reflection plus diffuse transmission. For the tested opaque standard material,
we use `GBuffer.DiffuseColor = BaseColor * (1 - Metallic)`. On a tested metal,
base RGB was `[0.9609375,1,0.9765625]`, metallic was 1, diffuse was exactly zero,
while UE's denoiser albedo was approximately `[0.90869,0.93945,0.91162]`.
Neither base color nor the UE denoiser AOV is an interchangeable albedo input.

**Material limitation:** the upstream Falcor demodulator clamps its divisor to
0.001, but final modulation uses the original albedo. A zero-diffuse metal can
therefore lose its reflected specular contribution. This port retains that
behavior rather than silently changing the algorithm. The metal probe verifies
input semantics; it is not an accepted metal-quality baseline. Transparent,
transmissive, layered Substrate, hair and material PT-switch cases remain outside
the verified baseline scope. The room baseline uses opaque nonmetal surfaces.

`SLSVGFFilter.*` implements reprojection, moments filtering, four a-trous passes
and final modulation using the adapted Falcor shaders committed to the project.
`Shaders/SVGF/provenance.json` retains the source hashes and Falcor commit, and
shader files retain NVIDIA's notices. The one-time import script has been removed.
UE adaptations are explicit:

- HLSL resource bindings, SV_Position input and output alpha=1.
- Octahedral normal packing and screen-space finite derivatives computed from
  auxiliary buffers; unlike Falcor's material raster derivatives these can cross
  triangle boundaries.
- UE depth units (centimeters) are retained; Falcor's absolute depth epsilon
  therefore has a different world-space meaning than a meter-based scene.
- Bounds checks validate the actual reprojection tap instead of only its nearest
  pixel; temporal history resets explicitly on camera cuts, frame gaps, resolution
  changes and parameter changes.
- Filter history is owned independently of UE's per-frame PT accumulation reset.
  The GM static capture was verified with zero motion and 32-frame history.

Parameters are exposed as `SL.SVGF.Iterations`, `FeedbackTap`, `Alpha`,
`MomentsAlpha`, `PhiColor`, and `PhiNormal`; the baseline JSON records their values.
Falcor's exported `VarianceEpsilon` setting is not used in its supplied filter
shader, so this port does not introduce a misleading control for it.

