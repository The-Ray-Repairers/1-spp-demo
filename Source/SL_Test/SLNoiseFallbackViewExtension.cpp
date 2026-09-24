// Copyright Epic Games, Inc. All Rights Reserved.

#include "SLNoiseFallbackViewExtension.h"
// PostProcessMaterialInputs.h defines FPostProcessMaterialInputs, EPostProcessMaterialInput,
// and EPathTracingPostProcessMaterialInput.  It lives in Renderer/Public so no private path
// hack is required — just the "Renderer" PrivateDependencyModule in Build.cs.
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PixelShaderUtils.h"     // FPixelShaderUtils::AddFullscreenPass  (RenderCore/Public)
#include "RenderGraphUtils.h"     // RDG utility passes                    (RenderCore/Public)
#include "HAL/IConsoleManager.h"

// -----------------------------------------------------------------------
// Console variable — lets developers tune or disable the noise at runtime.
// -----------------------------------------------------------------------
static TAutoConsoleVariable<float> CVarSLNoiseFallbackScale(
	TEXT("SL.NoiseFallback.Scale"),
	1.0f,
	TEXT("Intensity of the software 1-spp noise fallback (active only when hardware ray tracing is unavailable).\n")
	TEXT("Scales the standard deviation of the Poisson shot noise per pixel: 0 = off, 1 = default.\n")
	TEXT("Applies on the render thread — changes take effect the next frame."),
	ECVF_RenderThreadSafe);

extern TAutoConsoleVariable<int32> CVarSLPathTracerTargetSPP;

// -----------------------------------------------------------------------
// Bind the C++ shader class to the HLSL entry point.
// /Project is mapped to <ProjectDir>/Shaders/ by FSL_TestModule::StartupModule().
// -----------------------------------------------------------------------
IMPLEMENT_GLOBAL_SHADER(FSLNoiseFallbackPS, "/Project/SLNoiseFallback.usf", "MainPS", SF_Pixel);

// -----------------------------------------------------------------------
// FSLNoiseFallbackViewExtension
// -----------------------------------------------------------------------

FSLNoiseFallbackViewExtension::FSLNoiseFallbackViewExtension(
	const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FSLNoiseFallbackViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// Subscribe after the MotionBlur pass (the last pre-tonemap stage).
	// The callback fires regardless of whether motion blur itself is enabled,
	// giving us the scene colour in linear-HDR space before the tonemapper.
	if (Pass != EPostProcessingPass::MotionBlur)
	{
		return;
	}

	// Capture view data needed by the callback.
	// SubscribeToPostProcessingPass runs on the render thread, so these reads are safe.
	// NOTE: in UE5.8 the delegate provides the FSceneView directly to the callback, so
	// we don't need to capture FeatureLevel or FrameNumber here anymore.

	// Use CreateRaw — no captures required; view is provided by the delegate at call time.
	InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
		this, &FSLNoiseFallbackViewExtension::ApplyNoise_RenderThread));
}

FScreenPassTexture FSLNoiseFallbackViewExtension::ApplyNoise_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const float Scale = CVarSLNoiseFallbackScale.GetValueOnRenderThread();

	// Read view data directly — the delegate provides FSceneView in UE5.8.
	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const uint32 FrameNumber = static_cast<uint32>(View.Family->FrameNumber);

	// Retrieve the scene colour passed in from the previous post-process stage.
	// GetInput returns FScreenPassTextureSlice (an SRV-based handle) in UE5.3+.
	// FScreenPassTexture has an explicit constructor that extracts the parent texture.
	const FScreenPassTextureSlice SceneColorSlice =
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	FScreenPassTexture SceneColor(SceneColorSlice);

	if (!SceneColor.IsValid() || Scale <= 0.0f)
	{
		return SceneColor;  // pass through unchanged
	}

	const FIntRect ViewRect = SceneColor.ViewRect;

	// Allocate an output texture of the same format.
	FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
		OutputDesc, TEXT("SLNoiseFallback.Output"));

	// Fill shader parameters.
	FSLNoiseFallbackPS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSLNoiseFallbackPS::FParameters>();

	// ViewportOffset lets the shader translate SV_Position (0-based within the
	// viewport) into the correct absolute texel coordinate in the full texture.
	PassParameters->ViewportOffset  = FUintVector2(
		static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
	PassParameters->InputTexture    = SceneColor.Texture;
	PassParameters->FrameIndex      = FrameNumber;
	PassParameters->NoiseScale      = Scale;
	PassParameters->TargetSPP       = static_cast<float>(CVarSLPathTracerTargetSPP.GetValueOnRenderThread());
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		OutputTexture, ERenderTargetLoadAction::ENoAction);

	TShaderMapRef<FSLNoiseFallbackPS> PixelShader(GetGlobalShaderMap(FeatureLevel));

	// Dispatch a full-viewport triangle that covers exactly ViewRect.
	// FPixelShaderUtils::AddFullscreenPass is part of RenderCore (public),
	// so it does not require any Renderer-private headers.
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GetGlobalShaderMap(FeatureLevel),
		RDG_EVENT_NAME("SLNoiseFallback (software 1-spp approximation)"),
		PixelShader,
		PassParameters,
		ViewRect);

	// Return the noisy texture as the new scene colour.
	// The tonemapper (and all subsequent post-process passes) will receive this.
	FScreenPassTexture NoisyOutput = FScreenPassTexture(OutputTexture, ViewRect);

	// -----------------------------------------------------------------
	// Denoiser pass  —  selected at runtime by SL.Denoiser.Method.
	// The denoiser receives the noisy linear-HDR output and may replace
	// it with a filtered version before the tonemapper consumes it.
	//
	// We also forward the scene texture uniform buffer so advanced
	// denoisers (Neural, SVGF) can access Albedo, Normals, Depth, and
	// Motion Vectors from the G-Buffer.
	// -----------------------------------------------------------------
	FSLDenoiserContext DenoiserContext;
	DenoiserContext.SceneColor    = NoisyOutput;
	DenoiserContext.SceneTextures = Inputs.SceneTextures;

	return GetActiveDenoiser()->Apply(GraphBuilder, View, DenoiserContext);
}
