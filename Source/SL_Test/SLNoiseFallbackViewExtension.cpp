// Copyright Epic Games, Inc. All Rights Reserved.

#include "SLNoiseFallbackViewExtension.h"
#include "SL_Test.h"

extern TAutoConsoleVariable<int32> CVarSLDenoiserMethod;
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
	const FAutoRegister& AutoRegister, UWorld* InWorld, bool bInInjectNoise)
	: FWorldSceneViewExtension(AutoRegister, InWorld), bInjectNoise(bInInjectNoise)
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
	if (Pass != EPostProcessingPass::MotionBlur || View.bIsOfflineRender ||
        (!bInjectNoise && !View.Family->EngineShowFlags.PathTracing))
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

	if (!SceneColor.IsValid())
	{
		return SceneColor;  // pass through unchanged
	}

	const FIntRect ViewRect = SceneColor.ViewRect;

	FScreenPassTexture NoisyOutput = SceneColor;
    if (bInjectNoise && Scale > 0.f)
    {
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
	NoisyOutput = FScreenPassTexture(OutputTexture, ViewRect);
    }

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

    const FScreenPassTexture Filtered = GetActiveDenoiser()->Apply(GraphBuilder, View, DenoiserContext);
    const int32 Method = CVarSLDenoiserMethod.GetValueOnRenderThread();
    if (Method != LastExecutedMethod)
    {
        UE_LOG(LogSL_Test, Display, TEXT("SL filter: method=%d source=%s filtered_output=%s"), Method,
            bInjectNoise ? TEXT("software") : TEXT("hardware path tracing"),
            Filtered.Texture != NoisyOutput.Texture ? TEXT("yes") : TEXT("no"));
        LastExecutedMethod = Method;
    }
    const float Split = ComparisonPosition.Load();
    if (Split >= 0.f && Filtered.Texture != NoisyOutput.Texture)
    {
        // Preserve filter output/history; compose into a separate display texture.
        auto Composite = GraphBuilder.CreateTexture(Filtered.Texture->Desc, TEXT("SLFilter.Comparison"));
        AddCopyTexturePass(GraphBuilder, Filtered.Texture, Composite);
        FRHICopyTextureInfo Copy;
        Copy.SourcePosition = Copy.DestPosition = FIntVector(ViewRect.Min.X, ViewRect.Min.Y, 0);
        Copy.Size = FIntVector(FMath::Clamp(FMath::RoundToInt(ViewRect.Width() * Split), 0, ViewRect.Width()), ViewRect.Height(), 1);
        if (Copy.Size.X > 0) AddCopyTexturePass(GraphBuilder, NoisyOutput.Texture, Composite, Copy);
        return FScreenPassTexture(Composite, Filtered.ViewRect);
    }
	return Filtered;
}
