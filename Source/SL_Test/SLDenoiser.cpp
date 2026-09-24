// Copyright Epic Games, Inc. All Rights Reserved.
//
// SLDenoiser.cpp
// Implements the modular denoiser pass pipeline.
//
// Console variables
// -----------------
//   SL.Denoiser.Method                int  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder
//   SL.Denoiser.GaussianBlur.Sigma    float  standard deviation in pixels (default 1.5)
//   SL.Denoiser.Bilateral.SigmaSpatial float  spatial sigma (default 2.0)
//   SL.Denoiser.Bilateral.SigmaColor  float  color sigma (default 0.1)
//   SL.Denoiser.Neural.DebugMode      int  0 = composite, 1-5 = isolate individual buffer

#include "SLDenoiser.h"
#include "PixelShaderUtils.h"     // FPixelShaderUtils::AddFullscreenPass
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"      // TStaticSamplerState
#include "HAL/IConsoleManager.h"
#include "SystemTextures.h"       // GSystemTextures — black/white fallback textures

// ---------------------------------------------------------------------------
// Console variables
// ---------------------------------------------------------------------------

/** Selects the active denoiser.  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder. */
TAutoConsoleVariable<int32> CVarSLDenoiserMethod(
	TEXT("SL.Denoiser.Method"),
	0,
	TEXT("Select the denoiser applied to each 1-spp frame after noise injection.\n")
	TEXT("  0: None               — passthrough, no filtering (default)\n")
	TEXT("  1: GaussianBlur       — separable Gaussian blur (see SL.Denoiser.GaussianBlur.Sigma)\n")
	TEXT("  2: BilateralFilter    — edge-preserving blur (see SL.Denoiser.Bilateral.*)\n")
	TEXT("  3: NeuralPlaceholder  — diagnostic pass reading all G-Buffer inputs (see SL.Denoiser.Neural.DebugMode)\n")
	TEXT("Changes take effect on the next rendered frame."),
	ECVF_RenderThreadSafe);

/** Gaussian kernel standard deviation in pixels. */
static TAutoConsoleVariable<float> CVarSLGaussianBlurSigma(
	TEXT("SL.Denoiser.GaussianBlur.Sigma"),
	1.5f,
	TEXT("Standard deviation (in pixels) of the Gaussian blur kernel used by SL.Denoiser.Method 1.\n")
	TEXT("Larger values produce a smoother (blurrier) result.  Default: 1.5."),
	ECVF_RenderThreadSafe);

/** Spatial sigma for the Bilateral filter. */
static TAutoConsoleVariable<float> CVarSLBilateralSigmaSpatial(
	TEXT("SL.Denoiser.Bilateral.SigmaSpatial"),
	2.0f,
	TEXT("Spatial standard deviation (in pixels) for the Bilateral filter (Method 2). Default: 2.0"),
	ECVF_RenderThreadSafe);

/** Color/range sigma for the Bilateral filter. */
static TAutoConsoleVariable<float> CVarSLBilateralSigmaColor(
	TEXT("SL.Denoiser.Bilateral.SigmaColor"),
	0.1f,
	TEXT("Color standard deviation for the Bilateral filter (Method 2). Default: 0.1"),
	ECVF_RenderThreadSafe);

/** Debug mode for the Neural Placeholder. 0 = composite diagnostic, 1-5 = isolate buffers. */
static TAutoConsoleVariable<int32> CVarSLNeuralDebugMode(
	TEXT("SL.Denoiser.Neural.DebugMode"),
	0,
	TEXT("Controls the visualisation output of the Neural Denoiser Placeholder (Method 3).\n")
	TEXT("  0: Composite diagnostic — tiled grid showing all G-Buffer inputs\n")
	TEXT("  1: Noisy Color only (with purple tint)\n")
	TEXT("  2: Albedo only\n")
	TEXT("  3: World Normals only\n")
	TEXT("  4: Linear Depth only\n")
	TEXT("  5: Motion Vectors only\n"),
	ECVF_RenderThreadSafe);

// ---------------------------------------------------------------------------
// Shader binding  —  map C++ classes to HLSL entry points.
// ---------------------------------------------------------------------------
IMPLEMENT_GLOBAL_SHADER(FSLGaussianBlurPS_H,     "/Project/SLGaussianBlur.usf",             "MainPS_H", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FSLGaussianBlurPS_V,     "/Project/SLGaussianBlur.usf",             "MainPS_V", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FSLBilateralFilterPS,    "/Project/SLBilateralFilter.usf",          "MainPS",   SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FSLNeuralPlaceholderPS,  "/Project/SLNeuralPlaceholder.usf",        "MainPS",   SF_Pixel);

// ---------------------------------------------------------------------------
// FSLDenoiserGaussianBlur::Apply
// Runs a separable (horizontal then vertical) Gaussian blur over the input.
// ---------------------------------------------------------------------------
FScreenPassTexture FSLDenoiserGaussianBlur::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid())
	{
		return Input;
	}

	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const FIntRect  ViewRect        = Input.ViewRect;
	const FIntPoint TextureExtent   = Input.Texture->Desc.Extent;
	const float     Sigma           = CVarSLGaussianBlurSigma.GetValueOnRenderThread();
	const FVector2f InvTextureSize(
		1.0f / static_cast<float>(TextureExtent.X),
		1.0f / static_cast<float>(TextureExtent.Y));

	auto* BilinearClamp = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// ---- Horizontal pass: Input → IntermediateTexture ----
	FRDGTextureDesc IntermDesc = Input.Texture->Desc;
	FRDGTextureRef  IntermTexture = GraphBuilder.CreateTexture(IntermDesc, TEXT("SLGaussianBlur.Intermediate"));

	{
		FSLGaussianBlurPS_H::FParameters* Params =
			GraphBuilder.AllocParameters<FSLGaussianBlurPS_H::FParameters>();
		Params->ViewportOffset  = FUintVector2(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
		Params->InputTexture    = Input.Texture;
		Params->InputSampler    = BilinearClamp;
		Params->InvTextureSize  = InvTextureSize;
		Params->Sigma           = Sigma;
		Params->RenderTargets[0] = FRenderTargetBinding(IntermTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FSLGaussianBlurPS_H> HorizShader(GetGlobalShaderMap(FeatureLevel));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(FeatureLevel),
			RDG_EVENT_NAME("SLGaussianBlur (H, sigma=%.2f)", Sigma),
			HorizShader,
			Params,
			ViewRect);
	}

	// ---- Vertical pass: IntermediateTexture → OutputTexture ----
	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	FRDGTextureRef  OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SLGaussianBlur.Output"));

	{
		FSLGaussianBlurPS_V::FParameters* Params =
			GraphBuilder.AllocParameters<FSLGaussianBlurPS_V::FParameters>();
		Params->ViewportOffset  = FUintVector2(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
		Params->InputTexture    = IntermTexture;
		Params->InputSampler    = BilinearClamp;
		Params->InvTextureSize  = InvTextureSize;
		Params->Sigma           = Sigma;
		Params->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FSLGaussianBlurPS_V> VertShader(GetGlobalShaderMap(FeatureLevel));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(FeatureLevel),
			RDG_EVENT_NAME("SLGaussianBlur (V, sigma=%.2f)", Sigma),
			VertShader,
			Params,
			ViewRect);
	}

	return FScreenPassTexture(OutputTexture, ViewRect);
}

// ---------------------------------------------------------------------------
// FSLDenoiserBilateralFilter::Apply
// Runs a single-pass edge-preserving Bilateral filter over the input.
// ---------------------------------------------------------------------------
FScreenPassTexture FSLDenoiserBilateralFilter::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid()) return Input;

	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const FIntRect  ViewRect        = Input.ViewRect;
	const FIntPoint TextureExtent   = Input.Texture->Desc.Extent;

	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	FRDGTextureRef  OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SLBilateralFilter.Output"));

	FSLBilateralFilterPS::FParameters* Params =
		GraphBuilder.AllocParameters<FSLBilateralFilterPS::FParameters>();
	Params->ViewportOffset  = FUintVector2(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
	Params->InputTexture    = Input.Texture;
	Params->InputSampler    = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params->InvTextureSize  = FVector2f(1.0f / TextureExtent.X, 1.0f / TextureExtent.Y);
	Params->SigmaSpatial    = CVarSLBilateralSigmaSpatial.GetValueOnRenderThread();
	Params->SigmaColor      = CVarSLBilateralSigmaColor.GetValueOnRenderThread();
	Params->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction);

	TShaderMapRef<FSLBilateralFilterPS> PixelShader(GetGlobalShaderMap(FeatureLevel));
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GetGlobalShaderMap(FeatureLevel),
		RDG_EVENT_NAME("SLBilateralFilter"),
		PixelShader,
		Params,
		ViewRect);

	return FScreenPassTexture(OutputTexture, ViewRect);
}

// ---------------------------------------------------------------------------
// FSLDenoiserNeuralPlaceholder::Apply
//
// Reads all five G-Buffer auxiliary inputs and dispatches the placeholder
// shader that produces a diagnostic visualisation.  When your team's trained
// model is ready, replace the HLSL in SLNeuralPlaceholder.usf (or swap it
// for an NNE model inference call) and this C++ binding stays identical.
// ---------------------------------------------------------------------------
FScreenPassTexture FSLDenoiserNeuralPlaceholder::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid()) return Input;

	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const FIntRect  ViewRect      = Input.ViewRect;
	const FIntPoint TextureExtent = Input.Texture->Desc.Extent;

	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	FRDGTextureRef  OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SLNeuralPlaceholder.Output"));

	// Create a fallback black texture in case any G-Buffer is unavailable.
	FRDGTextureRef BlackFallback = GSystemTextures.GetBlackDummy(GraphBuilder);

	// Extract the individual G-Buffer textures from the scene texture uniform buffer.
	// If scene textures are not bound (e.g., forward rendering or stripped pipeline),
	// we fall back to a solid black texture so the shader doesn't crash.
	FRDGTextureRef AlbedoTex   = BlackFallback;
	FRDGTextureRef NormalsTex  = BlackFallback;
	FRDGTextureRef DepthTex    = BlackFallback;
	FRDGTextureRef VelocityTex = BlackFallback;

	if (Context.SceneTextures.SceneTextures)
	{
		const auto* STParams = Context.SceneTextures.SceneTextures->GetContents();
		if (STParams)
		{
			if (STParams->GBufferCTexture)         AlbedoTex   = STParams->GBufferCTexture;
			if (STParams->GBufferATexture)         NormalsTex  = STParams->GBufferATexture;
			if (STParams->SceneDepthTexture)        DepthTex    = STParams->SceneDepthTexture;
			if (STParams->GBufferVelocityTexture)  VelocityTex = STParams->GBufferVelocityTexture;
		}
	}

	FSLNeuralPlaceholderPS::FParameters* Params =
		GraphBuilder.AllocParameters<FSLNeuralPlaceholderPS::FParameters>();
	Params->ViewportOffset   = FUintVector2(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
	Params->InvTextureSize   = FVector2f(1.0f / TextureExtent.X, 1.0f / TextureExtent.Y);
	Params->NoisyColor       = Input.Texture;
	Params->InputSampler     = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params->AlbedoTexture    = AlbedoTex;
	Params->NormalsTexture   = NormalsTex;
	Params->DepthTexture     = DepthTex;
	Params->VelocityTexture  = VelocityTex;
	Params->DebugMode        = CVarSLNeuralDebugMode.GetValueOnRenderThread();
	Params->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction);

	TShaderMapRef<FSLNeuralPlaceholderPS> PixelShader(GetGlobalShaderMap(FeatureLevel));
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GetGlobalShaderMap(FeatureLevel),
		RDG_EVENT_NAME("SLNeuralPlaceholder (DebugMode=%d)", Params->DebugMode),
		PixelShader,
		Params,
		ViewRect);

	return FScreenPassTexture(OutputTexture, ViewRect);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
ISLDenoiserPass* GetDenoiserForMethod(ESLDenoiserMethod Method)
{
	// Static singletons — each denoiser is stateless on the CPU side so a
	// single instance shared across all frames is safe.
	static FSLDenoiserNone               s_None;
	static FSLDenoiserGaussianBlur       s_GaussianBlur;
	static FSLDenoiserBilateralFilter    s_BilateralFilter;
	static FSLDenoiserNeuralPlaceholder  s_NeuralPlaceholder;

	switch (Method)
	{
	case ESLDenoiserMethod::GaussianBlur:
		return &s_GaussianBlur;

	case ESLDenoiserMethod::BilateralFilter:
		return &s_BilateralFilter;

	case ESLDenoiserMethod::NeuralPlaceholder:
		return &s_NeuralPlaceholder;

	case ESLDenoiserMethod::None:
	default:
		return &s_None;
	}
}

ISLDenoiserPass* GetActiveDenoiser()
{
	const ESLDenoiserMethod Method =
		static_cast<ESLDenoiserMethod>(CVarSLDenoiserMethod.GetValueOnRenderThread());
	return GetDenoiserForMethod(Method);
}
