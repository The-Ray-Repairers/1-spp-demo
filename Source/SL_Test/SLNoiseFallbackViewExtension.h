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
#include "SLDenoiser.h"             // ISLDenoiserPass, GetDenoiserForMethod

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
 * Pre-tonemap filter selection for hardware path tracing and the software fallback.
 * Only the software path injects synthetic Poisson noise; hardware samples pass
 * directly to the selected filter. Offline reference views are excluded.
 * USLPathTracerSubsystem warns on screen only when using the software fallback.
 */
class FSLNoiseFallbackViewExtension : public FWorldSceneViewExtension
{
public:
	FSLNoiseFallbackViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld, bool bInInjectNoise = true);
    void SetComparisonPosition(float Position) { ComparisonPosition.Store(Position); }

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
	const bool bInjectNoise;
	int32 LastExecutedMethod = INDEX_NONE;
    TAtomic<float> ComparisonPosition{-1.f};
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
