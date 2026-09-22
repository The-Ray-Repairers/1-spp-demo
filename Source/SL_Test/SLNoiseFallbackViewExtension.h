// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"  // FWorldSceneViewExtension, EPostProcessingPass, FAfterPassCallbackDelegateArray
#include "ScreenPass.h"          // FScreenPassTexture  (Renderer/Public)
// NOTE: FPostProcessMaterialInputs is forward-declared by SceneViewExtension.h;
//       the full definition (#include "PostProcess/PostProcessMaterial.h") is
//       only needed in the .cpp where members are actually accessed.
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHIStaticStates.h"

/**
 * Global pixel shader that adds Poisson shot noise to the scene colour.
 * Dispatched via FPixelShaderUtils::AddFullscreenPass each frame by
 * FSLNoiseFallbackViewExtension.
 *
 * Source: Shaders/SLNoiseFallback.usf
 * The /Project -> <ProjectDir>/Shaders mapping is registered in FSL_TestModule::StartupModule().
 */
class FSLNoiseFallbackPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSLNoiseFallbackPS);
	SHADER_USE_PARAMETER_STRUCT(FSLNoiseFallbackPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** Top-left corner of the viewport inside the full render target (pixels). */
		SHADER_PARAMETER(FUintVector2, ViewportOffset)
		/** Linear-HDR scene colour loaded by texel coordinate. */
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		/** View.Family->FrameNumber (uint32) — unique seed per frame. */
		SHADER_PARAMETER(uint32, FrameIndex)
		/** Overall noise intensity (SL.NoiseFallback.Scale). */
		SHADER_PARAMETER(float, NoiseScale)
		/** Target samples per pixel (SL.PathTracer.TargetSPP) to emulate variance reduction. */
		SHADER_PARAMETER(float, TargetSPP)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Software 1-spp noise fallback — active only when hardware ray tracing is
 * unavailable (e.g. GTX 1660 / DXR Tier 1.0 GPUs).
 *
 * Subscribes to EPostProcessingPass::MotionBlur (the last pre-tonemap pass),
 * then injects a Poisson shot noise pass so the tonemapper and all later
 * passes receive the noisy linear-HDR colour as their input.
 *
 * This is intentionally a DEVELOPMENT FALLBACK.  USLPathTracerSubsystem
 * displays a persistent red/orange on-screen warning whenever this extension
 * is active so developers cannot mistake it for the physically-based path.
 */
class FSLNoiseFallbackViewExtension : public FWorldSceneViewExtension
{
public:
	FSLNoiseFallbackViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	// FSceneViewExtensionBase overrides
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	/**
	 * Called once per frame (render thread) when UE is deciding which
	 * post-process callbacks to run after each pass.  We subscribe to
	 * EPostProcessingPass::MotionBlur so our noise fires in linear-HDR space,
	 * before the tonemapper.
	 */
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& View,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

private:
	/**
	 * The actual noise pass, executed as a post-process callback.
	 * Reads the linear-HDR scene colour from Inputs, adds Poisson shot noise,
	 * and returns the modified texture for the tonemapper to consume.
	 *
	 * In UE5.8, FPostProcessingPassDelegate passes the view directly, so we
	 * receive FeatureLevel and FrameNumber here without any captured state.
	 */
	FScreenPassTexture ApplyNoise_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
};
