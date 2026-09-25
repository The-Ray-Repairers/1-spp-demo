# Architectural Proposal: Neural Denoising Pipeline

Based on our testing and debugging sessions, we strongly recommend adopting the **C++ Render Dependency Graph (RDG) Pipeline (Option B)** as the standard implementation for the team's neural denoiser, deprecating the Post Process Material approach currently on `main`.

Here is a summary of the technical arguments for why this approach is critical for the success of the project:

### 1. Pipeline Correctness (Pre-Tonemapping vs. Post-Tonemapping)
A high-quality denoiser must operate on raw, linear, high-dynamic-range (HDR) scene color. 
* **The Problem with Materials:** Post Process Materials operate late in the rendering pipeline, typically *after* tonemapping. By this stage, the image has been converted to display-referred sRGB space. Specular highlights are clamped, physically-based lighting values are destroyed, and mathematical variance is non-linear. Training a network on this data yields poor, unpredictable results.
* **The C++ Advantage:** By hooking directly into the `FSceneView` and RDG in C++, we intercept the raw `SceneColor` immediately after the path tracer finishes its pass, ensuring the neural network processes the exact mathematical variance it was trained on.

### 2. G-Buffer & Temporal Data Access
As the ML team scales the neural denoiser, it will require auxiliary inputs: Albedo, Linear Depth, World Normals, and Motion Vectors.
* **The Problem with Materials:** While a material can sample some G-Buffer textures, accessing historical data (like previous frame motion vectors for temporal reprojection) or custom buffers is clunky, performance-heavy, or outright impossible within the restrictive node graph.
* **The C++ Advantage:** Our C++ pipeline natively receives the `FSLDenoiserContext`, which contains all `FScreenPassTexture` inputs. We can easily bind these directly into the ONNX model as separate input tensors via `FTensorBindingRDG` with zero overhead.

### 3. Hardware Agnosticism & Robustness
During testing, we discovered that the Material approach on `main` fundamentally breaks on machines without Hardware Ray Tracing (DXR).
* **The Problem with Materials:** The project relies on a Software Noise Fallback for non-DXR hardware. Because the Material runs as a generic post-process effect, the engine injects the fallback noise *after* the neural network runs (or feeds the fake noise in incorrectly), resulting in severe visual artifacts (the "gray fog").
* **The C++ Advantage:** C++ allows explicit, absolute ordering within the Render Graph. We can guarantee the denoiser evaluates at the correct stage regardless of hardware capabilities, ensuring a consistent testing environment for all developers.

### 4. Performance and Memory Control
* **The Problem with Materials:** The Unreal `NeuralProfile` asset acts as a black box. It handles tensor allocation and execution opaquely, which can lead to unpredictable GPU memory spikes or sub-optimal tiling dispatches.
* **The C++ Advantage:** Using `NNERuntimeRDG` directly in C++ allows us to manage `FRDGBuffer` allocations manually. We can schedule the inference (`EnqueueRDG`) in the exact same command list as our custom compute shaders for preprocessing and upsampling. This avoids pipeline stalls, keeps memory footprint tight, and allows us to seamlessly drop in the final PyTorch model export when it's ready.
