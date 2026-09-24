// Copyright Epic Games, Inc. All Rights Reserved.
//
// SLDenoiser.h
// Declares the denoiser pipeline interface (ISLDenoiserPass) and all concrete
// denoiser implementations that can be swapped at runtime via the
// SL.Denoiser.Method console variable.
//
// Adding a new denoiser
// ---------------------
//  1. Declare a new class that inherits ISLDenoiserPass and override Apply().
//  2. Register it in SLDenoiser.cpp's GetDenoiserForMethod().
//  3. Add the matching shader / USF file if required.
//  4. Document the new method value in SLDenoiser.cpp's CVar help text.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ScreenPass.h"
#include "RenderGraphBuilder.h"
#include "SceneTexturesConfig.h"   // FSceneTextureShaderParameters

// ---------------------------------------------------------------------------
// Denoiser method enum — mirrors the SL.Denoiser.Method CVar integer values.
// ---------------------------------------------------------------------------
enum class ESLDenoiserMethod : int32
{
	/** No denoising — pass the noisy frame through unchanged. */
	None         = 0,

	/** Separable Gaussian blur (sigma driven by SL.Denoiser.GaussianBlur.Sigma). */
	GaussianBlur = 1,

	/** Edge-preserving Bilateral filter (spatial/color sigmas driven by CVars). */
	BilateralFilter = 2,

	/** Neural Denoiser placeholder — reads all G-Buffer auxiliary inputs. */
	NeuralPlaceholder = 3,
};

// ---------------------------------------------------------------------------
// FSLDenoiserContext — inputs passed to every denoiser pass.
// ---------------------------------------------------------------------------

/**
 * Bundles the noisy scene colour together with scene-texture bindings
 * so that advanced denoisers (e.g. SVGF, Neural) can access the G-Buffer
 * (albedo, normals, depth, motion vectors) without changing each simple
 * denoiser's call-site.
 *
 * Simple denoisers (None, Gaussian, Bilateral) only read SceneColor.
 */
struct FSLDenoiserContext
{
	/** The noisy 1-spp scene colour in linear HDR space. */
	FScreenPassTexture SceneColor;

	/**
	 * Scene texture uniform buffer containing all G-Buffer textures.
	 * Available entries include:
	 *   - GBufferATexture  (World Normals)
	 *   - GBufferCTexture  (Base Color / Albedo)
	 *   - SceneDepthTexture
	 *   - GBufferVelocityTexture (Motion Vectors)
	 * May be empty if scene textures were not available at the call-site.
	 */
	FSceneTextureShaderParameters SceneTextures;
};

// ---------------------------------------------------------------------------
// ISLDenoiserPass
// ---------------------------------------------------------------------------
class ISLDenoiserPass
{
public:
	virtual ~ISLDenoiserPass() = default;

	/**
	 * Apply the denoising algorithm.
	 *
	 * @param GraphBuilder  Active RDG builder for this frame.
	 * @param View          The scene view being rendered.
	 * @param Context       Noisy scene colour + optional G-Buffer textures.
	 * @return              The denoised texture (may be the same as Input for
	 *                      passthrough implementations).
	 */
	virtual FScreenPassTexture Apply(
		FRDGBuilder&          GraphBuilder,
		const FSceneView&     View,
		const FSLDenoiserContext& Context) = 0;
};

// ---------------------------------------------------------------------------
// FSLDenoiserNone  —  transparent passthrough (method 0)
// ---------------------------------------------------------------------------
class FSLDenoiserNone final : public ISLDenoiserPass
{
public:
	virtual FScreenPassTexture Apply(
		FRDGBuilder&          GraphBuilder,
		const FSceneView&     View,
		const FSLDenoiserContext& Context) override
	{
		return Context.SceneColor;
	}
};

// ---------------------------------------------------------------------------
// Gaussian Blur shaders  —  separable horizontal + vertical passes.
// ---------------------------------------------------------------------------

/** Horizontal (X) Gaussian blur pass. */
class FSLGaussianBlurPS_H : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSLGaussianBlurPS_H);
	SHADER_USE_PARAMETER_STRUCT(FSLGaussianBlurPS_H, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, ViewportOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(FVector2f, InvTextureSize)
		SHADER_PARAMETER(float, Sigma)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
	{
		return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
	}
};

/** Vertical (Y) Gaussian blur pass. */
class FSLGaussianBlurPS_V : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSLGaussianBlurPS_V);
	SHADER_USE_PARAMETER_STRUCT(FSLGaussianBlurPS_V, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, ViewportOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(FVector2f, InvTextureSize)
		SHADER_PARAMETER(float, Sigma)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
	{
		return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
	}
};

// ---------------------------------------------------------------------------
// FSLDenoiserGaussianBlur  —  separable Gaussian blur (method 1)
// ---------------------------------------------------------------------------
class FSLDenoiserGaussianBlur final : public ISLDenoiserPass
{
public:
	virtual FScreenPassTexture Apply(
		FRDGBuilder&          GraphBuilder,
		const FSceneView&     View,
		const FSLDenoiserContext& Context) override;
};

// ---------------------------------------------------------------------------
// Bilateral Filter shader & pass (method 2)
// ---------------------------------------------------------------------------

/** Edge-preserving Bilateral Filter pass. */
class FSLBilateralFilterPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSLBilateralFilterPS);
	SHADER_USE_PARAMETER_STRUCT(FSLBilateralFilterPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, ViewportOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(FVector2f, InvTextureSize)
		SHADER_PARAMETER(float, SigmaSpatial)
		SHADER_PARAMETER(float, SigmaColor)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
	{
		return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
	}
};

class FSLDenoiserBilateralFilter final : public ISLDenoiserPass
{
public:
	virtual FScreenPassTexture Apply(
		FRDGBuilder&          GraphBuilder,
		const FSceneView&     View,
		const FSLDenoiserContext& Context) override;
};

// ---------------------------------------------------------------------------
// Neural Denoiser Placeholder shader & pass (method 3)
//
// Reads all G-Buffer auxiliary inputs that a real neural denoiser would need:
//   - Noisy Color (1 spp)
//   - Albedo (GBufferC)
//   - World Normals (GBufferA)
//   - Linear Depth (SceneDepth)
//   - Motion Vectors (Velocity)
//
// Outputs a diagnostic visualisation proving all inputs are wired correctly.
// Replace the shader logic with your trained model's inference for production.
// ---------------------------------------------------------------------------

/** Neural Denoiser Placeholder pixel shader. */
class FSLNeuralPlaceholderPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSLNeuralPlaceholderPS);
	SHADER_USE_PARAMETER_STRUCT(FSLNeuralPlaceholderPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FUintVector2, ViewportOffset)
		SHADER_PARAMETER(FVector2f, InvTextureSize)
		// --- Primary input: the noisy 1-spp scene colour ---
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NoisyColor)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		// --- G-Buffer auxiliary inputs ---
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AlbedoTexture)    // GBufferC (Base Color)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalsTexture)   // GBufferA (World Normals)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)     // SceneDepthTexture
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)  // GBufferVelocityTexture (Motion Vectors)
		// --- Diagnostic control ---
		SHADER_PARAMETER(int32, DebugMode)  // 0 = composite diagnostic, 1..5 = isolate single buffer
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
	{
		return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
	}
};

class FSLDenoiserNeuralPlaceholder final : public ISLDenoiserPass
{
public:
	virtual FScreenPassTexture Apply(
		FRDGBuilder&          GraphBuilder,
		const FSceneView&     View,
		const FSLDenoiserContext& Context) override;
};

// ---------------------------------------------------------------------------
// Factory helper
// ---------------------------------------------------------------------------

/**
 * Returns the singleton denoiser implementation for the given method.
 * The returned pointer is owned by static storage — do not delete it.
 * Safe to call every frame on the render thread.
 */
ISLDenoiserPass* GetDenoiserForMethod(ESLDenoiserMethod Method);

/**
 * Reads SL.Denoiser.Method on the render thread and returns the matching
 * denoiser singleton.  Convenience wrapper to avoid exposing the CVar
 * across translation units.
 */
ISLDenoiserPass* GetActiveDenoiser();
