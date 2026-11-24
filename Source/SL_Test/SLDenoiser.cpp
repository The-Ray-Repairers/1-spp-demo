// Copyright Epic Games, Inc. All Rights Reserved.
//
// SLDenoiser.cpp
// Implements the modular denoiser pass pipeline.
//
// Console variables
// -----------------
//   SL.Denoiser.Method                int  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder,
//                                          4 = NNE Candy, 5 = RayRepair, 6 = NBDirect, 7 = RayRepairMotion
//   SL.Denoiser.RayRepair.Model       string  NNE model asset for method 5
//   SL.Denoiser.RayRepairMotion.Model string  NNE model asset for method 7 (recurrent, motion-aware)
//   SL.Denoiser.GaussianBlur.Sigma    float  standard deviation in pixels (default 1.5)
//   SL.Denoiser.Bilateral.SigmaSpatial float  spatial sigma (default 2.0)
//   SL.Denoiser.Bilateral.SigmaColor  float  color sigma (default 0.1)
//   SL.Denoiser.Neural.DebugMode      int  0 = composite, 1-5 = isolate individual buffer

#include "SLDenoiser.h"
#include "PixelShaderUtils.h"     // FPixelShaderUtils::AddFullscreenPass
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"      // TStaticSamplerState
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"      // ENQUEUE_RENDER_COMMAND
#include "SystemTextures.h"       // GSystemTextures — black/white fallback textures
#include "NNE.h"
#include "NNERuntimeRDG.h"
#include "NNEModelData.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DelayedAutoRegister.h"
#include "UObject/StrongObjectPtr.h"
#include "Async/Async.h"
#include "Engine/Engine.h"           // GEngine->AddOnScreenDebugMessage
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

// ---------------------------------------------------------------------------
// Console variables
// ---------------------------------------------------------------------------

/** Selects the active denoiser.  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder, 4 = NNE Candy, 5 = RayRepair, 6 = NBDirect, 7 = RayRepairMotion. */
TAutoConsoleVariable<int32> CVarSLDenoiserMethod(
	TEXT("SL.Denoiser.Method"),
	0,
	TEXT("Select the denoiser applied to each 1-spp frame after noise injection.\n")
	TEXT("  0: None               — passthrough, no filtering (default)\n")
	TEXT("  1: GaussianBlur       — separable Gaussian blur (see SL.Denoiser.GaussianBlur.Sigma)\n")
	TEXT("  2: BilateralFilter    — edge-preserving blur (see SL.Denoiser.Bilateral.*)\n")
	TEXT("  3: NeuralPlaceholder  — diagnostic pass reading all G-Buffer inputs (see SL.Denoiser.Neural.DebugMode)\n")
	TEXT("  4: NNE Candy Model    — direct C++ inference of ONNX model via NNERuntimeRDG\n")
	TEXT("  5: RayRepair          — Ray Repair format v1 neural denoiser (see SL.Denoiser.RayRepair.Model)\n")
	TEXT("  6: NBDirect           — NoiseBase-trained Ray Repair U-Net (/Game/ML/nb_direct, NoiseBase input encoding)\n")
	TEXT("  7: RayRepairMotion    — recurrent, motion-aware Ray Repair U-Net (see SL.Denoiser.RayRepairMotion.Model)\n")
	TEXT("Changes take effect on the next rendered frame."),
	ECVF_RenderThreadSafe);

/**
 * NNE model asset for method 6: nb_direct (neural-denoiser runs/nb_direct) rewrapped by
 * Scripts/rewrap_rayrepair_onnx.py --encoding noisebase, so it expects camera-basis normals
 * and camera distance, and accepts any resolution.
 */
static const TCHAR* GSLNBDirectModelPath = TEXT("/Game/ML/nb_direct.nb_direct");

/**
 * NNE model asset loaded by the Ray Repair denoiser (method 5).
 * String CVars cannot be ECVF_RenderThreadSafe, so the render thread reads
 * a copy that the change callback forwards via a render command.
 */
static FString GSLRayRepairModelPath_RenderThread = TEXT("/Game/ML/RayRepair_v1_1920x1080");
static TAutoConsoleVariable<FString> CVarSLRayRepairModel(
	TEXT("SL.Denoiser.RayRepair.Model"),
	GSLRayRepairModelPath_RenderThread,
	TEXT("NNE model data asset for SL.Denoiser.Method 5, exported by neural-denoiser's denoiser/export_ue.py.\n")
	TEXT("The tensor size is read from the model and must equal the view rect (use r.ScreenPercentage 100).\n")
	TEXT("Changing it reloads the model on the next frame."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		ENQUEUE_RENDER_COMMAND(SLRayRepairSetModel)([Path = Var->GetString()](FRHICommandListImmediate&)
		{
			GSLRayRepairModelPath_RenderThread = Path;
		});
	}),
	ECVF_Default);

/**
 * NNE model asset for method 7: a recurrent export from neural-denoiser's
 * denoiser/export_onnx_ue.py (docs/UE_INTEGRATION.md), passed through
 * Scripts/replace_gridsample_onnx.py so DirectML never runs GridSample. It takes ue_buffers in the
 * NoiseBase encoding, motion_px and hidden_in0..4, and returns denoised_rgb and hidden_out0..4.
 * The default is the colour-fixed export (rayrepair_energy=tile, ..._et_1920x1080). The original
 * export (/Game/ML/rayrepair_motion_s3area_1920x1080_dmlsafe) has the same signature but darkens
 * its output by 20-30%.
 */
static FString GSLRayRepairMotionModelPath_RenderThread = TEXT("/Game/ML/rayrepair_motion_s3area_et_1920x1080_dmlsafe");
static TAutoConsoleVariable<FString> CVarSLRayRepairMotionModel(
	TEXT("SL.Denoiser.RayRepairMotion.Model"),
	GSLRayRepairMotionModelPath_RenderThread,
	TEXT("NNE model data asset for SL.Denoiser.Method 7, a recurrent motion-aware export from neural-denoiser's\n")
	TEXT("denoiser/export_onnx_ue.py, converted with Scripts/replace_gridsample_onnx.py.\n")
	TEXT("Its size is fixed at export and must equal the view rect (use r.ScreenPercentage 100).\n")
	TEXT("Changing it reloads the model and clears the recurrent state on the next frame."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		ENQUEUE_RENDER_COMMAND(SLRayRepairMotionSetModel)([Path = Var->GetString()](FRHICommandListImmediate&)
		{
			GSLRayRepairMotionModelPath_RenderThread = Path;
		});
	}),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSLRayRepairDebug(
	TEXT("SL.Denoiser.RayRepair.Debug"),
	0,
	TEXT("Debug view for the Ray Repair neural denoisers (SL.Denoiser.Method 5, 6, 7). The model still runs.\n")
	TEXT(" 0: denoised_rgb (default)\n")
	TEXT(" 1: denoised_rgb, plus an on-screen line saying whether the model ran this frame\n")
	TEXT(" 2: ue_buffers ch 0-2, the noisy colour the model receives. Must look exactly like method 0; if it\n")
	TEXT("    does not, the problem is in the pre/postprocess passes, not the network\n")
	TEXT(" 3: ue_buffers ch 3-5, base colour. Should be clean; grain here reaches the network as noise\n")
	TEXT(" 4: ue_buffers ch 6-8, normals (* 0.5 + 0.5)\n")
	TEXT("Problems (wrong view size, model failed to load) are always shown on screen."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSLRayRepairAlbedo(
	TEXT("SL.Denoiser.RayRepair.Albedo"),
	1,
	TEXT("Base colour (ue_buffers ch 3-5) and normals (ch 6-8) for the Ray Repair neural denoisers on path traced frames.\n")
	TEXT("The path tracer skips the base pass, so 0 and 1 render it once more each frame (the SLGuides custom pass).\n")
	TEXT(" 0: first-hit diffuse colour, base colour * (1 - metallic), and shading normal. NoiseBase's definition, but\n")
	TEXT("    metals get albedo 0 and the model breaks down on them\n")
	TEXT(" 1: first-hit base colour and shading normal, as neural-denoiser docs/UE_INTEGRATION.md specifies (default)\n")
	TEXT(" 2: the path tracer's denoiser albedo and normal. No extra pass, but mirrors take the albedo and normal of\n")
	TEXT("    what they reflect"),
	ECVF_RenderThreadSafe);

bool SLRayRepairWantsGuidePass()
{
	const int32 Method = CVarSLDenoiserMethod.GetValueOnGameThread();
	return Method >= int32(ESLDenoiserMethod::RayRepair) && Method <= int32(ESLDenoiserMethod::RayRepairMotion)
		&& CVarSLRayRepairAlbedo.GetValueOnGameThread() != 2;
}

/** Frames to wait before the requested dump; -1 when none is requested. */
static std::atomic<int32> GSLRayRepairDumpDelay{ -1 };
static FAutoConsoleCommand GSLRayRepairDumpCommand(
	TEXT("SL.Denoiser.RayRepair.Dump"),
	TEXT("Write one frame of the active Ray Repair model's tensors (SL.Denoiser.Method 5, 6, 7) to\n")
	TEXT("Saved/RayRepairDump/<method>_<time>/: ue_buffers, motion_px, hidden_in<i> and denoised_rgb as\n")
	TEXT("float32 NCHW .npy files (numpy.load), plus meta.json with shapes, per-channel min/max/mean and the\n")
	TEXT("pre-exposure. Running the same ONNX on these inputs offline must reproduce denoised_rgb.\n")
	TEXT("Optional argument: frames to wait first (default 0)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 Delay = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 0;
		GSLRayRepairDumpDelay = Delay;
		UE_LOG(LogTemp, Display, TEXT("SLDenoiser: dumping the Ray Repair tensors in %d frames"), Delay);
	}));

DECLARE_GPU_STAT_NAMED(SLRayRepair, TEXT("SL Ray Repair (pre + NNE + post)"));

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
IMPLEMENT_GLOBAL_SHADER(FSLNNEPreprocessCS,      "/Project/SLNNEPreprocess.usf",            "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLNNEPostprocessCS,     "/Project/SLNNEPostprocess.usf",           "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLNNEUpsamplePS,        "/Project/SLNNEUpsample.usf",              "MainPS",   SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FSLRayRepairPreprocessCS,  "/Project/SLRayRepairPreprocess.usf",    "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLRayRepairPostprocessCS, "/Project/SLRayRepairPostprocess.usf",   "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLRayRepairMotionCS,      "/Project/SLRayRepairMotion.usf",        "MainCS",   SF_Compute);

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
	Params->DebugMode        = DebugModeOverride >= 0 ? DebugModeOverride : CVarSLNeuralDebugMode.GetValueOnRenderThread();
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
// FSLDenoiserNNE::Apply
// Runs candy-9-clean model using NNERuntimeRDG directly in C++
// ---------------------------------------------------------------------------
FScreenPassTexture FSLDenoiserNNE::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid()) return Input;

	// 0. Load the model and create the instance once
	static UNNEModelData* ModelData = nullptr;
	static TSharedPtr<UE::NNE::IModelInstanceRDG> ModelInstance;


	if (!ModelInstance.IsValid())
	{
		ModelData = LoadObject<UNNEModelData>(nullptr, TEXT("/Game/ML/candy-9-clean"));
		if (!ModelData)
		{
			ModelData = LoadObject<UNNEModelData>(nullptr, TEXT("/Game/ML/candy-9-clean.candy-9-clean"));
		}
		
		if (!ModelData)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserNNE: Failed to load /Game/ML/candy-9-clean"));
			return Input;
		}

		TWeakInterfacePtr<INNERuntimeRDG> Runtime = UE::NNE::GetRuntime<INNERuntimeRDG>(TEXT("NNERuntimeORTDml"));
		if (!Runtime.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserNNE: NNERuntimeORTDml is not available"));
			return Input;
		}

		TSharedPtr<UE::NNE::IModelRDG> Model = Runtime->CreateModelRDG(ModelData);
		if (Model.IsValid())
		{
			ModelInstance = Model->CreateModelInstanceRDG();
			if (ModelInstance.IsValid())
			{
				TArray<UE::NNE::FTensorShape> InputShapes = { UE::NNE::FTensorShape::Make({1, 3, 224, 224}) };
				ModelInstance->SetInputTensorShapes(InputShapes);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("SLDenoiserNNE: Failed to create IModelInstanceRDG"));
				return Input;
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserNNE: Failed to create IModelRDG"));
			return Input;
		}
	}

	if (!ModelInstance.IsValid()) return Input;

	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;

	// 1. Preprocess: copy texture into 224x224 buffer
	FRDGBufferDesc InputTensorDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1 * 3 * 224 * 224);
	FRDGBufferRef InputTensorBuffer = GraphBuilder.CreateBuffer(InputTensorDesc, TEXT("NNEInputTensor"));

	{
		FSLNNEPreprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLNNEPreprocessCS::FParameters>();
		Params->InputTexture = Input.Texture;
		Params->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->OutputTensor = GraphBuilder.CreateUAV(InputTensorBuffer, PF_R32_FLOAT);
		Params->InvOutputSize = FVector2f(1.0f / 224.0f, 1.0f / 224.0f);

		TShaderMapRef<FSLNNEPreprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NNEPreprocess"), ComputeShader, Params, FIntVector(224 / 8, 224 / 8, 1));
	}

	// 2. Inference
	FRDGBufferDesc OutputTensorDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1 * 3 * 224 * 224);
	FRDGBufferRef OutputTensorBuffer = GraphBuilder.CreateBuffer(OutputTensorDesc, TEXT("NNEOutputTensor"));

	TArray<UE::NNE::FTensorBindingRDG> InputBindings;
	UE::NNE::FTensorBindingRDG InBinding;
	InBinding.Buffer = InputTensorBuffer;
	InputBindings.Add(InBinding);

	TArray<UE::NNE::FTensorBindingRDG> OutputBindings;
	UE::NNE::FTensorBindingRDG OutBinding;
	OutBinding.Buffer = OutputTensorBuffer;
	OutputBindings.Add(OutBinding);

	ModelInstance->EnqueueRDG(GraphBuilder, InputBindings, OutputBindings);

	// 3. Postprocess: unpack buffer to 224x224 texture
	FRDGTextureDesc TensorTexDesc = FRDGTextureDesc::Create2D(FIntPoint(224, 224), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_UAV | TexCreate_ShaderResource);
	FRDGTextureRef TensorTexture = GraphBuilder.CreateTexture(TensorTexDesc, TEXT("NNETensorTexture"));

	{
		FSLNNEPostprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLNNEPostprocessCS::FParameters>();
		Params->InputTensor = GraphBuilder.CreateSRV(OutputTensorBuffer, PF_R32_FLOAT);
		Params->OutputTexture = GraphBuilder.CreateUAV(TensorTexture);

		TShaderMapRef<FSLNNEPostprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NNEPostprocess"), ComputeShader, Params, FIntVector(224 / 8, 224 / 8, 1));
	}

	// 4. Upsample: stretch 224x224 texture back to screen resolution
	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("NNEUpsampleOutput"));

	{
		FSLNNEUpsamplePS::FParameters* Params = GraphBuilder.AllocParameters<FSLNNEUpsamplePS::FParameters>();
		Params->InputTexture = TensorTexture;
		Params->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->InvTextureSize = FVector2f(1.0f / (float)Input.ViewRect.Width(), 1.0f / (float)Input.ViewRect.Height());
		Params->ViewportOffset = FUintVector2(Input.ViewRect.Min.X, Input.ViewRect.Min.Y);
		Params->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FSLNNEUpsamplePS> PixelShader(GetGlobalShaderMap(FeatureLevel));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(FeatureLevel),
			RDG_EVENT_NAME("NNEUpsample"),
			PixelShader,
			Params,
			Input.ViewRect);
	}

	return FScreenPassTexture(OutputTexture, Input.ViewRect);
}

// ---------------------------------------------------------------------------
// FSLDenoiserRayRepair
// Runs a Ray Repair ONNX model (raw UE buffers in, linear HDR out) at full
// view-rect resolution. See neural-denoiser docs/UE_INTEGRATION.md and
// Scripts/rewrap_rayrepair_onnx.py.
//
// Recurrent models also take hidden_in<i> and return hidden_out<i>. Last
// frame's hidden_out<i> is bound as this frame's hidden_in<i>: each frame
// writes fresh RDG buffers and extracts them into History, and the next
// frame registers them back. Motion-aware models also take motion_px, which
// SLRayRepairMotion.usf writes from velocity or camera reprojection.
// ---------------------------------------------------------------------------
// Model assets are loaded on the game thread and handed to the render thread by pointer. The
// render thread cannot load packages itself: in a -game or cooked build the loader ignores the
// request and the method silently passes the noisy frame through.
namespace SLRayRepairModels
{
	TMap<FString, TStrongObjectPtr<UNNEModelData>> Loaded;   // game thread; keeps loaded assets alive
	TSet<FString> Failed;                                    // game thread; each failure is logged once
	TMap<FString, UNNEModelData*> ForRenderThread;           // render thread

	FString ObjectPath(const FString& Path)
	{
		// Accept "/Game/ML/Name" as well as the full object path "/Game/ML/Name.Name".
		return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPaths::GetBaseFilename(Path);
	}

	/** Game thread: the model asset SL.Denoiser.Method needs, or an empty string. */
	FString PathForMethod(int32 Method)
	{
		switch (static_cast<ESLDenoiserMethod>(Method))
		{
		case ESLDenoiserMethod::RayRepair:       return CVarSLRayRepairModel.GetValueOnGameThread();
		case ESLDenoiserMethod::NBDirect:        return GSLNBDirectModelPath;
		case ESLDenoiserMethod::RayRepairMotion: return CVarSLRayRepairMotionModel.GetValueOnGameThread();
		default:                                 return FString();
		}
	}

	/** Game thread: load Path once and hand it to the render thread. */
	void Prepare(const FString& Path)
	{
		if (Path.IsEmpty() || Loaded.Contains(Path) || Failed.Contains(Path)) return;
		UNNEModelData* Data = LoadObject<UNNEModelData>(nullptr, *ObjectPath(Path));
		if (!Data)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiser: Failed to load NNE model asset %s"), *Path);
			Failed.Add(Path);
			return;
		}
		Loaded.Add(Path, TStrongObjectPtr<UNNEModelData>(Data));
		ENQUEUE_RENDER_COMMAND(SLDenoiserModelReady)([Path, Data](FRHICommandListImmediate&)
		{
			ForRenderThread.Add(Path, Data);
		});
	}
}

// Every frame, before rendering and also while the game is paused (flythrough capture pauses it),
// make sure the selected method's model is loaded.
static FDelayedAutoRegisterHelper GSLDenoiserModelLoader(EDelayedRegisterRunPhase::EndOfEngineInit, []
{
	FCoreDelegates::OnBeginFrame.AddLambda([]
	{
		const int32 Method = CVarSLDenoiserMethod.GetValueOnGameThread();
		SLRayRepairModels::Prepare(SLRayRepairModels::PathForMethod(Method));

		// Say on screen when the selected neural method is passing the noisy frame through.
		if (GEngine && Method >= int32(ESLDenoiserMethod::RayRepair) && Method <= int32(ESLDenoiserMethod::RayRepairMotion))
		{
			bool bProblem = false;
			const FString Status = static_cast<FSLDenoiserRayRepair*>(
				GetDenoiserForMethod(static_cast<ESLDenoiserMethod>(Method)))->GetStatus(bProblem);
			if (!Status.IsEmpty() && (bProblem || CVarSLRayRepairDebug.GetValueOnGameThread() > 0))
			{
				GEngine->AddOnScreenDebugMessage(0xA014, 0.f, bProblem ? FColor::Red : FColor::Green, Status);
			}
		}
	});
	FCoreDelegates::OnPreExit.AddLambda([]
	{
		SLRayRepairModels::Loaded.Empty();
	});
});

static constexpr uint32 GSLRayRepairRawChannels = 10;
static constexpr uint32 GSLRayRepairOutChannels = 3;
static constexpr uint32 GSLRayRepairMotionChannels = 2;

namespace SLRayRepair
{
	/** Index of the tensor called Name, or INDEX_NONE. */
	int32 FindTensor(TConstArrayView<UE::NNE::FTensorDesc> Descs, const FString& Name)
	{
		return Descs.IndexOfByPredicate([&](const UE::NNE::FTensorDesc& D) { return D.GetName() == Name; });
	}

	FString ShapeString(TConstArrayView<int32> Dims)
	{
		return TEXT("[") + FString::JoinBy(Dims, TEXT(", "), [](int32 D) { return D < 0 ? FString(TEXT("?")) : FString::FromInt(D); }) + TEXT("]");
	}
}

namespace SLRayRepairDump
{
	FCriticalSection LastFolderLock;
	FString LastFolder;

	/** numpy .npy, format 1.0: little-endian float32, C order. */
	bool WriteNpy(const FString& Path, TConstArrayView<int64> Shape, TConstArrayView<float> Values)
	{
		FString Dims = FString::JoinBy(Shape, TEXT(", "), [](int64 D) { return LexToString(D); });
		if (Shape.Num() == 1) Dims += TEXT(",");
		FString Header = FString::Printf(TEXT("{'descr': '<f4', 'fortran_order': False, 'shape': (%s), }"), *Dims);
		const int32 Unpadded = 10 + Header.Len() + 1;   // magic + version + length + header + newline
		Header += FString::ChrN((64 - Unpadded % 64) % 64, TEXT(' ')) + TEXT("\n");

		TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*Path));
		if (!File) return false;
		uint8 Prefix[10] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, uint8(Header.Len() & 0xFF), uint8(Header.Len() >> 8) };
		File->Serialize(Prefix, sizeof(Prefix));
		const FTCHARToUTF8 Utf8(*Header);
		File->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		File->Serialize(const_cast<float*>(Values.GetData()), Values.Num() * sizeof(float));
		return File->Close();
	}

	FString Number(double V)
	{
		return FMath::IsFinite(V) ? FString::SanitizeFloat(V) : FString(TEXT("null"));
	}

	/** {"min", "max", "mean", "nan", "inf"} over Values. Means use finite values only. */
	FString Stats(TConstArrayView<float> Values)
	{
		double Min = TNumericLimits<double>::Max(), Max = -TNumericLimits<double>::Max(), Sum = 0.0;
		int64 NaN = 0, Inf = 0, Finite = 0;
		for (float V : Values)
		{
			if (FMath::IsNaN(V)) { ++NaN; continue; }
			if (!FMath::IsFinite(V)) { ++Inf; continue; }
			Min = FMath::Min(Min, double(V)); Max = FMath::Max(Max, double(V)); Sum += V; ++Finite;
		}
		if (Finite == 0) Min = Max = Sum = 0.0;
		return FString::Printf(TEXT("{\"min\": %s, \"max\": %s, \"mean\": %s, \"nan\": %lld, \"inf\": %lld}"),
			*Number(Min), *Number(Max), *Number(Finite ? Sum / Finite : 0.0), NaN, Inf);
	}
}

FString FSLDenoiserRayRepair::GetLastDumpFolder()
{
	FScopeLock Lock(&SLRayRepairDump::LastFolderLock);
	return SLRayRepairDump::LastFolder;
}

void FSLDenoiserRayRepair::SetStatus(const FString& NewStatus, bool bNewProblem)
{
	if (NewStatus == LastStatus_RenderThread) return;
	LastStatus_RenderThread = NewStatus;
	FScopeLock Lock(&StatusLock);
	Status = NewStatus;
	bStatusProblem = bNewProblem;
}

void FSLDenoiserRayRepair::QueueDump(FRDGBuilder& GraphBuilder, TArray<FDumpTensor>&& Tensors,
	TConstArrayView<FRDGBufferRef> Buffers, FRDGBufferRef ViewInfoBuffer, const FString& Meta)
{
	check(Tensors.Num() == Buffers.Num());
	for (int32 i = 0; i < Tensors.Num(); ++i)
	{
		FDumpTensor& T = Tensors[i];
		T.NumBytes = static_cast<uint32>(Buffers[i]->Desc.GetSize());
		T.Readback = MakeUnique<FRHIGPUBufferReadback>(FName(*(TEXT("SLRayRepairDump.") + T.TensorName)));
		AddEnqueueCopyPass(GraphBuilder, T.Readback.Get(), Buffers[i], T.NumBytes);
	}
	PendingViewInfo = MakeUnique<FRHIGPUBufferReadback>(TEXT("SLRayRepairDump.ViewInfo"));
	AddEnqueueCopyPass(GraphBuilder, PendingViewInfo.Get(), ViewInfoBuffer, 2 * sizeof(float));
	PendingDump = MoveTemp(Tensors);
	PendingDumpMeta = Meta;
	PendingDumpFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RayRepairDump")
		/ FString::Printf(TEXT("%s_%s"), *Name, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
}

void FSLDenoiserRayRepair::PollDump()
{
	if (PendingDump.IsEmpty() || !PendingViewInfo->IsReady()) return;
	for (const FDumpTensor& T : PendingDump)
	{
		if (!T.Readback->IsReady()) return;
	}

	struct FData { FString TensorName; TArray<int64> Shape; TArray<float> Values; };
	TArray<FData> Data;
	for (FDumpTensor& T : PendingDump)
	{
		FData& D = Data.AddDefaulted_GetRef();
		D.TensorName = T.TensorName;
		D.Shape = T.Shape;
		D.Values.SetNumUninitialized(T.NumBytes / sizeof(float));
		FMemory::Memcpy(D.Values.GetData(), T.Readback->Lock(T.NumBytes), T.NumBytes);
		T.Readback->Unlock();
	}
	float ViewInfo[2];
	FMemory::Memcpy(ViewInfo, PendingViewInfo->Lock(sizeof(ViewInfo)), sizeof(ViewInfo));
	PendingViewInfo->Unlock();
	PendingDump.Reset();
	PendingViewInfo.Reset();

	const FString Meta = PendingDumpMeta + FString::Printf(TEXT(",\n  \"pre_exposure\": %s"), *SLRayRepairDump::Number(ViewInfo[0]));
	// Writing ~200 MB takes a moment; do it off the render thread.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Folder = PendingDumpFolder, Meta, Data = MoveTemp(Data), LogName = Name]()
	{
		IFileManager::Get().MakeDirectory(*Folder, true);
		FString Tensors;
		bool bOk = true;
		for (const FData& D : Data)
		{
			bOk &= SLRayRepairDump::WriteNpy(Folder / (D.TensorName + TEXT(".npy")), D.Shape, D.Values);

			// Per-channel statistics for the image tensors, whole-tensor for the hidden state.
			FString Channels;
			const int64 C = D.Shape.Num() == 4 ? D.Shape[1] : 1;
			const int64 Plane = D.Values.Num() / FMath::Max<int64>(C, 1);
			if (C <= 10)
			{
				for (int64 c = 0; c < C; ++c)
				{
					const FString Stat = SLRayRepairDump::Stats(TConstArrayView<float>(D.Values.GetData() + c * Plane, Plane));
					Channels += (c ? TEXT(", ") : TEXT("")) + Stat;
					UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s dump: %s ch %lld %s"), *LogName, *D.TensorName, c, *Stat);
				}
			}
			const FString All = SLRayRepairDump::Stats(D.Values);
			if (C > 10) UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s dump: %s %s"), *LogName, *D.TensorName, *All);
			Tensors += FString::Printf(TEXT("%s\n    \"%s\": {\"file\": \"%s.npy\", \"shape\": [%s], \"all\": %s%s}"),
				Tensors.IsEmpty() ? TEXT("") : TEXT(","), *D.TensorName, *D.TensorName,
				*FString::JoinBy(D.Shape, TEXT(", "), [](int64 V) { return LexToString(V); }), *All,
				Channels.IsEmpty() ? TEXT("") : *(TEXT(", \"channels\": [") + Channels + TEXT("]")));
		}
		const FString Json = TEXT("{\n") + Meta + TEXT(",\n  \"layout\": \"float32 little-endian NCHW; element (c, y, x) at c*H*W + y*W + x. ")
			TEXT("ue_buffers ch 0-2 = scene colour * pre_exposure (linear, before tonemapping), 3-5 base colour, ")
			TEXT("6-8 normal, 9 depth (see SLRayRepairPreprocess.usf); motion_px = previous minus current pixel position, ")
			TEXT("x right, y down; hidden_in<i> = the recurrent state this frame received (zeros after a reset)\",\n  \"tensors\": {")
			+ Tensors + TEXT("\n  }\n}\n");
		bOk &= FFileHelper::SaveStringToFile(Json, *(Folder / TEXT("meta.json")));
		UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s: %s the tensor dump to %s"), *LogName, bOk ? TEXT("wrote") : TEXT("FAILED to write"), *Folder);
		{
			FScopeLock Lock(&SLRayRepairDump::LastFolderLock);
			SLRayRepairDump::LastFolder = bOk ? Folder : FString();
		}
		AsyncTask(ENamedThreads::GameThread, [Folder, bOk]
		{
			if (GEngine) GEngine->AddOnScreenDebugMessage(0xA015, 10.f, bOk ? FColor::Green : FColor::Red,
				FString::Printf(TEXT("Ray Repair tensor dump %s %s"), bOk ? TEXT("written to") : TEXT("FAILED:"), *Folder));
		});
	});
}

bool FSLDenoiserRayRepair::LoadModel(const FString& ModelPath, UNNEModelData* ModelData)
{
	TWeakInterfacePtr<INNERuntimeRDG> Runtime = UE::NNE::GetRuntime<INNERuntimeRDG>(TEXT("NNERuntimeORTDml"));
	if (!Runtime.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: NNERuntimeORTDml is not available"), *Name);
		return false;
	}

	TSharedPtr<UE::NNE::IModelRDG> Model = Runtime->CreateModelRDG(ModelData);
	TSharedPtr<UE::NNE::IModelInstanceRDG> Instance = Model.IsValid() ? Model->CreateModelInstanceRDG() : nullptr;
	if (!Instance.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: Failed to create model instance for %s"), *Name, *ModelPath);
		return false;
	}

	// Match the signature by name. A model with a single input and output is a still
	// model whatever its tensor names are (format v1 and rewrapped exports).
	const TConstArrayView<UE::NNE::FTensorDesc> InputDescs = Instance->GetInputTensorDescs();
	const TConstArrayView<UE::NNE::FTensorDesc> OutputDescs = Instance->GetOutputTensorDescs();
	const bool bSingleIO = InputDescs.Num() == 1 && OutputDescs.Num() == 1;
	const int32 NewBuffersInput = bSingleIO ? 0 : SLRayRepair::FindTensor(InputDescs, TEXT("ue_buffers"));
	const int32 NewDenoisedOutput = bSingleIO ? 0 : SLRayRepair::FindTensor(OutputDescs, TEXT("denoised_rgb"));
	if (NewBuffersInput == INDEX_NONE || NewDenoisedOutput == INDEX_NONE)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s has %d inputs and %d outputs but no ue_buffers input or denoised_rgb output"),
			*Name, *ModelPath, InputDescs.Num(), OutputDescs.Num());
		return false;
	}

	const TConstArrayView<int32> Dims = InputDescs[NewBuffersInput].GetShape().GetData();
	if (Dims.Num() != 4 || (Dims[0] != 1 && Dims[0] != -1) || Dims[1] != (int32)GSLRayRepairRawChannels)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s ue_buffers is %s, not [1, %u, H, W]"),
			*Name, *ModelPath, *SLRayRepair::ShapeString(Dims), GSLRayRepairRawChannels);
		return false;
	}
	const bool bNewDynamicSize = Dims[2] < 0 || Dims[3] < 0;

	// Every other input must be motion_px or a hidden_in<i> with a matching hidden_out<i>.
	int32 NewMotionInput = INDEX_NONE;
	TArray<FHiddenTensor> NewHidden;
	TArray<UE::NNE::FTensorShape> NewStaticShapes;
	NewStaticShapes.SetNum(InputDescs.Num());
	for (int32 i = 0; i < InputDescs.Num(); ++i)
	{
		if (i == NewBuffersInput) continue;
		const FString& InName = InputDescs[i].GetName();
		const UE::NNE::FSymbolicTensorShape& InShape = InputDescs[i].GetShape();
		if (!InShape.IsConcrete() || bNewDynamicSize)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s input %s is %s; recurrent and motion inputs need a fixed-size export"),
				*Name, *ModelPath, *InName, *SLRayRepair::ShapeString(InShape.GetData()));
			return false;
		}
		NewStaticShapes[i] = UE::NNE::FTensorShape::MakeFromSymbolic(InShape);

		if (InName == TEXT("motion_px"))
		{
			const TConstArrayView<int32> M = InShape.GetData();
			if (M.Num() != 4 || M[1] != (int32)GSLRayRepairMotionChannels || M[2] != Dims[2] || M[3] != Dims[3])
			{
				UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s motion_px is %s, expected [1, 2, %d, %d]"),
					*Name, *ModelPath, *SLRayRepair::ShapeString(M), Dims[2], Dims[3]);
				return false;
			}
			NewMotionInput = i;
			continue;
		}

		if (!InName.StartsWith(TEXT("hidden_in")))
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s has an unsupported input %s (expected ue_buffers, motion_px, hidden_in<i>)"),
				*Name, *ModelPath, *InName);
			return false;
		}
		const FString OutName = TEXT("hidden_out") + InName.RightChop(9);
		const int32 Out = SLRayRepair::FindTensor(OutputDescs, OutName);
		if (Out == INDEX_NONE || OutputDescs[Out].GetShape() != InShape)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s input %s %s has no output %s of the same shape"),
				*Name, *ModelPath, *InName, *SLRayRepair::ShapeString(InShape.GetData()), *OutName);
			return false;
		}
		NewHidden.Add({ i, Out, NewStaticShapes[i].Volume() });
	}
	if (OutputDescs.Num() != 1 + NewHidden.Num())
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: %s has %d outputs; expected denoised_rgb plus one hidden_out per hidden_in (%d)"),
			*Name, *ModelPath, OutputDescs.Num(), NewHidden.Num());
		return false;
	}

	ModelInstance     = Instance;
	TensorSize        = FIntPoint::ZeroValue;
	bDynamicSize      = bNewDynamicSize;
	NumInputs         = InputDescs.Num();
	NumOutputs        = OutputDescs.Num();
	BuffersInput      = NewBuffersInput;
	MotionInput       = NewMotionInput;
	DenoisedOutput    = NewDenoisedOutput;
	StaticInputShapes = MoveTemp(NewStaticShapes);
	Hidden            = MoveTemp(NewHidden);
	ResetHistory();

	if (bDynamicSize)
	{
		UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s: Loaded %s (dynamic resolution)"), *Name, *ModelPath);
		return true;
	}

	// Static export: prepare its one resolution now.
	if (!ResizeTo(FIntPoint(Dims[3], Dims[2])))
	{
		ModelInstance.Reset();
		return false;
	}
	uint64 HiddenFloats = 0;
	for (const FHiddenTensor& H : Hidden) HiddenFloats += H.NumFloats;
	UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s: Loaded %s (%dx%d%s, %d recurrent tensors, %.1f MB state)"),
		*Name, *ModelPath, TensorSize.X, TensorSize.Y, MotionInput != INDEX_NONE ? TEXT(", motion_px") : TEXT(""),
		Hidden.Num(), HiddenFloats * sizeof(float) / (1024.0 * 1024.0));
	return true;
}

bool FSLDenoiserRayRepair::ResizeTo(FIntPoint Size)
{
	TArray<UE::NNE::FTensorShape> InputShapes = StaticInputShapes;
	InputShapes[BuffersInput] = UE::NNE::FTensorShape::Make(
		{ 1u, GSLRayRepairRawChannels, static_cast<uint32>(Size.Y), static_cast<uint32>(Size.X) });
	if (ModelInstance->SetInputTensorShapes(InputShapes) != UE::NNE::EResultStatus::Ok)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: SetInputTensorShapes failed for %dx%d"), *Name, Size.X, Size.Y);
		TensorSize = FIntPoint::ZeroValue;
		return false;
	}
	// Output shapes are resolved here when the runtime can infer them; check the volume we bind.
	const TConstArrayView<UE::NNE::FTensorShape> OutShapes = ModelInstance->GetOutputTensorShapes();
	const uint64 Expected = uint64(GSLRayRepairOutChannels) * Size.X * Size.Y;
	if (OutShapes.Num() > DenoisedOutput && OutShapes[DenoisedOutput].Volume() != Expected)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: output has %llu values, expected [1, 3, %d, %d]"),
			*Name, (unsigned long long)OutShapes[DenoisedOutput].Volume(), Size.Y, Size.X);
		TensorSize = FIntPoint::ZeroValue;
		return false;
	}
	TensorSize = Size;
	ResetHistory();
	return true;
}

void FSLDenoiserRayRepair::ResetHistory()
{
	History.Reset();
	HistoryFrame = 0;
	HistoryViewKey = 0;
}

FScreenPassTexture FSLDenoiserRayRepair::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid()) return Input;
	PollDump();

	// 0. Load the model and create the instance once (again if the asset path changes)
	const FString ModelPath = GetModelPath();
	if (ModelPath != LoadedPath)
	{
		// The game thread loads the asset (SLRayRepairModels::Prepare); wait for it.
		UNNEModelData** ModelData = SLRayRepairModels::ForRenderThread.Find(ModelPath);
		if (!ModelData)
		{
			if (WaitingPath != ModelPath)
			{
				UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s: waiting for %s to load; passing through"), *Name, *ModelPath);
				WaitingPath = ModelPath;
			}
			SetStatus(FString::Printf(TEXT("%s: loading %s; showing the noisy frame"), *Name, *ModelPath), false);
			return Input;
		}
		WaitingPath.Reset();
		LoadedPath = ModelPath;
		ModelInstance.Reset();
		ResetHistory();
		bLoadFailed = !LoadModel(ModelPath, *ModelData);
		LastWarnedSize = FIntPoint::ZeroValue;
	}
	if (bLoadFailed || !ModelInstance.IsValid())
	{
		SetStatus(FString::Printf(TEXT("%s: could not load %s (see the log); showing the noisy frame"), *Name, *ModelPath), true);
		return Input;
	}

	// Dynamic models follow the view rect; static exports must match it exactly.
	const FIntRect ViewRect = Input.ViewRect;
	if (ViewRect.Size() != TensorSize)
	{
		const bool bResized = bDynamicSize && ViewRect.Width() > 0 && ViewRect.Height() > 0 && ResizeTo(ViewRect.Size());
		if (!bResized)
		{
			if (LastWarnedSize != ViewRect.Size())
			{
				UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: view rect %dx%d does not match model %dx%d; passing through. ")
					TEXT("Use a dynamic-resolution model (Scripts/rewrap_rayrepair_onnx.py), re-export at this resolution, ")
					TEXT("or set r.ScreenPercentage 100 and a matching window size."),
					*Name, ViewRect.Width(), ViewRect.Height(), TensorSize.X, TensorSize.Y);
				LastWarnedSize = ViewRect.Size();
			}
			SetStatus(FString::Printf(TEXT("%s is NOT running: the view is %dx%d but the model only runs at %dx%d, so this is the noisy frame.\n")
				TEXT("Play in a New Editor Window or Standalone Game and enter r.SetRes %dx%dw (PIE in the viewport cannot change size)."),
				*Name, ViewRect.Width(), ViewRect.Height(), TensorSize.X, TensorSize.Y, TensorSize.X, TensorSize.Y), true);
			return Input;
		}
	}

	if (!Context.SceneTextures.SceneTextures)
	{
		UE_LOG(LogTemp, Warning, TEXT("SLDenoiser%s: scene textures unavailable; passing through"), *Name);
		SetStatus(FString::Printf(TEXT("%s: scene textures unavailable; showing the noisy frame"), *Name), true);
		return Input;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, SLRayRepair, "SLRayRepair %s", *Name);
	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const uint32 Plane = static_cast<uint32>(TensorSize.X) * static_cast<uint32>(TensorSize.Y);
	const FUintVector2 ViewRectMin(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
	const FUintVector2 TensorSizeU(static_cast<uint32>(TensorSize.X), static_cast<uint32>(TensorSize.Y));
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(TensorSize, 8);

	TArray<UE::NNE::FTensorBindingRDG> InputBindings;
	InputBindings.SetNum(NumInputs);
	TArray<UE::NNE::FTensorBindingRDG> OutputBindings;
	OutputBindings.SetNum(NumOutputs);

	// Guide textures are view-rect sized; use one only if it covers the tensor.
	auto Fits = [&](FRDGTextureRef T) { return T && T->Desc.Extent.X >= TensorSize.X && T->Desc.Extent.Y >= TensorSize.Y; };

	// 1. Preprocess: scene color + decoded G-Buffer -> ue_buffers [1, 10, H, W]
	FRDGBufferRef InputTensorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), GSLRayRepairRawChannels * Plane), TEXT("RayRepair.UEBuffers"));
	FRDGBufferRef ViewInfoBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), 2), TEXT("RayRepair.ViewInfo"));
	{
		FSLRayRepairPreprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLRayRepairPreprocessCS::FParameters>();
		Params->View              = View.ViewUniformBuffer;
		Params->SceneTextures     = Context.SceneTextures;
		Params->SceneColorTexture = Input.Texture;
		Params->ViewRectMin       = ViewRectMin;
		Params->TensorSize        = TensorSizeU;
		Params->InputEncoding     = static_cast<uint32>(Encoding);
		// Path traced frames have no G-Buffer. Take base colour and normals from the SLGuides base pass,
		// else from the path tracer's own guides (SL.Denoiser.RayRepair.Albedo).
		const int32 AlbedoMode = CVarSLRayRepairAlbedo.GetValueOnRenderThread();
		FRDGTextureRef GuideAlbedo = nullptr, GuideNormal = nullptr;
		if (AlbedoMode != 2 && Fits(Context.GuideNormalDepth) && Fits(Context.GuideDiffuse) && Fits(Context.GuideBaseColor))
		{
			GuideAlbedo = AlbedoMode == 1 ? Context.GuideBaseColor : Context.GuideDiffuse;
			GuideNormal = Context.GuideNormalDepth;
			GuideSource = AlbedoMode == 1 ? TEXT("base colour, base pass") : TEXT("diffuse colour, base pass");
		}
		else if (Fits(Context.PathTracerAlbedo) && Fits(Context.PathTracerNormal))
		{
			GuideAlbedo = Context.PathTracerAlbedo;
			GuideNormal = Context.PathTracerNormal;
			GuideSource = AlbedoMode == 2 ? TEXT("path tracer albedo") : TEXT("path tracer albedo (base pass guides missing)");
		}
		else
		{
			GuideSource = View.Family->EngineShowFlags.PathTracing ? TEXT("EMPTY G-Buffer (no guides)") : TEXT("G-Buffer");
		}
		FRDGTextureRef Black = GSystemTextures.GetBlackDummy(GraphBuilder);
		Params->GuideAlbedo = GuideAlbedo ? GuideAlbedo : Black;
		Params->GuideNormal = GuideNormal ? GuideNormal : Black;
		Params->UseGuides   = GuideAlbedo ? 1u : 0u;
		if (!GuideAlbedo && View.Family->EngineShowFlags.PathTracing && !bWarnedNoGuides)
		{
			UE_LOG(LogTemp, Warning, TEXT("SLDenoiser%s: path traced frame without base pass or path tracer guides; ")
				TEXT("base colour and normals come from the empty G-Buffer"), *Name);
			bWarnedNoGuides = true;
		}
		Params->OutputTensor      = GraphBuilder.CreateUAV(InputTensorBuffer, PF_R32_FLOAT);
		Params->ViewInfo          = GraphBuilder.CreateUAV(ViewInfoBuffer, PF_R32_FLOAT);

		TShaderMapRef<FSLRayRepairPreprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%sPreprocess", *Name), ComputeShader, Params, GroupCount);
	}
	InputBindings[BuffersInput].Buffer = InputTensorBuffer;

	// 1b. motion_px [1, 2, H, W]: previous minus current pixel position
	FRDGBufferRef MotionTensorBuffer = nullptr;
	if (MotionInput != INDEX_NONE)
	{
		FRDGBufferRef MotionBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), GSLRayRepairMotionChannels * Plane), TEXT("RayRepair.MotionPx"));
		FSLRayRepairMotionCS::FParameters* Params = GraphBuilder.AllocParameters<FSLRayRepairMotionCS::FParameters>();
		Params->View          = View.ViewUniformBuffer;
		Params->SceneTextures = Context.SceneTextures;
		Params->ViewRectMin   = ViewRectMin;
		Params->TensorSize    = TensorSizeU;
		// Clip space to last frame's clip space, from this pass's own record of the camera. Doubles, then one
		// cast of the (well-conditioned) product, so large world coordinates do not lose precision.
		const FViewMatrices& Matrices = View.ViewMatrices;
		const FMatrix ViewProjection = Matrices.GetWorldToView() * Matrices.GetViewToClipNoAA();
		const bool bHavePrevCamera = PrevCameraFrame + 1 == View.Family->FrameNumber && PrevCameraViewKey == View.GetViewKey();
		Params->ClipToPrevClip = FMatrix44f(bHavePrevCamera ? ViewProjection.Inverse() * PrevViewProjection : FMatrix::Identity);
		Params->UseVelocity    = View.Family->EngineShowFlags.PathTracing ? 0u : 1u;
		const bool bGuideMotion = Fits(Context.GuideMotion);
		Params->GuideMotion    = bGuideMotion ? Context.GuideMotion : GSystemTextures.GetBlackDummy(GraphBuilder);
		Params->UseGuideMotion = bGuideMotion ? 1u : 0u;
		PrevViewProjection = ViewProjection;
		PrevCameraFrame    = View.Family->FrameNumber;
		PrevCameraViewKey  = View.GetViewKey();
		Params->OutputTensor  = GraphBuilder.CreateUAV(MotionBuffer, PF_R32_FLOAT);

		TShaderMapRef<FSLRayRepairMotionCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%sMotion", *Name), ComputeShader, Params, GroupCount);
		InputBindings[MotionInput].Buffer = MotionBuffer;
		MotionTensorBuffer = MotionBuffer;
	}

	// 1c. Recurrent state: last frame's hidden_out, or zeros when there is no usable history.
	// History only continues from the frame directly before this one, for the same view.
	const uint64 FrameNumber = View.Family->FrameNumber;
	const uint32 ViewKey = View.GetViewKey();
	const bool bResetHistory = !Hidden.IsEmpty() && (History.Num() != Hidden.Num() || View.bCameraCut
		|| HistoryFrame + 1 != FrameNumber || HistoryViewKey != ViewKey);
	TArray<FRDGBufferRef, TInlineAllocator<8>> HiddenIn, HiddenOut;
	for (int32 h = 0; h < Hidden.Num(); ++h)
	{
		const FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), static_cast<uint32>(Hidden[h].NumFloats));
		FRDGBufferRef In;
		if (bResetHistory)
		{
			In = GraphBuilder.CreateBuffer(Desc, TEXT("RayRepair.HiddenZero"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(In, PF_R32_FLOAT), 0u);
		}
		else
		{
			In = GraphBuilder.RegisterExternalBuffer(History[h], TEXT("RayRepair.HiddenIn"));
		}
		InputBindings[Hidden[h].InputIndex].Buffer = In;
		HiddenIn.Add(In);

		FRDGBufferRef Out = GraphBuilder.CreateBuffer(Desc, TEXT("RayRepair.HiddenOut"));
		OutputBindings[Hidden[h].OutputIndex].Buffer = Out;
		HiddenOut.Add(Out);
	}

	// 2. Inference
	FRDGBufferRef OutputTensorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), GSLRayRepairOutChannels * Plane), TEXT("RayRepair.DenoisedRGB"));
	OutputBindings[DenoisedOutput].Buffer = OutputTensorBuffer;

	if (ModelInstance->EnqueueRDG(GraphBuilder, InputBindings, OutputBindings) != UE::NNE::EResultStatus::Ok)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiser%s: EnqueueRDG failed; passing through"), *Name);
		SetStatus(FString::Printf(TEXT("%s: NNE EnqueueRDG failed (see the log); showing the noisy frame"), *Name), true);
		ResetHistory();
		return Input;
	}

	const uint32 Run = ++FramesRun;
	const uint32 WithHistory = (!Hidden.IsEmpty() && !bResetHistory) ? ++FramesWithHistory : FramesWithHistory.load();
	const int32 DebugView = CVarSLRayRepairDebug.GetValueOnRenderThread();
	static const TCHAR* DebugViewNames[] = { TEXT("denoised_rgb"), TEXT("denoised_rgb"),
		TEXT("DEBUG: input colour (ue_buffers 0-2)"), TEXT("DEBUG: base colour (ue_buffers 3-5)"), TEXT("DEBUG: normals (ue_buffers 6-8)") };
	SetStatus(FString::Printf(TEXT("%s ran at %dx%d: %u frames%s. Albedo/normals: %s. Showing %s"), *Name, TensorSize.X, TensorSize.Y, Run,
		Hidden.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", recurrent state carried on %u"), WithHistory),
		*GuideSource, DebugViewNames[FMath::Clamp(DebugView, 0, 4)]), false);

	// SL.Denoiser.RayRepair.Dump: copy this frame's tensors back to the CPU (written once they arrive).
	if (PendingDump.IsEmpty() && GSLRayRepairDumpDelay.load() >= 0 && GSLRayRepairDumpDelay.fetch_sub(1) == 0)
	{
		const int64 W = TensorSize.X, H = TensorSize.Y;
		TArray<FDumpTensor> Tensors;
		TArray<FRDGBufferRef> Buffers;
		auto Add = [&](const FString& TensorName, TArray<int64> Shape, FRDGBufferRef Buffer)
		{
			FDumpTensor& T = Tensors.AddDefaulted_GetRef();
			T.TensorName = TensorName;
			T.Shape = MoveTemp(Shape);
			Buffers.Add(Buffer);
		};
		Add(TEXT("ue_buffers"), { 1, GSLRayRepairRawChannels, H, W }, InputTensorBuffer);
		if (MotionTensorBuffer) Add(TEXT("motion_px"), { 1, GSLRayRepairMotionChannels, H, W }, MotionTensorBuffer);
		for (int32 h = 0; h < Hidden.Num(); ++h)
		{
			TArray<int64> Shape;
			for (uint32 D : StaticInputShapes[Hidden[h].InputIndex].GetData()) Shape.Add(D);
			Add(FString::Printf(TEXT("hidden_in%d"), h), MoveTemp(Shape), HiddenIn[h]);
		}
		Add(TEXT("denoised_rgb"), { 1, GSLRayRepairOutChannels, H, W }, OutputTensorBuffer);

		auto CVarInt = [](const TCHAR* CVarName)
		{
			const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
			return Var ? Var->GetInt() : -1;
		};
		const FString Meta = FString::Printf(
			TEXT("  \"method\": \"%s\",\n  \"model\": \"%s\",\n  \"frame\": %llu,\n  \"view_rect\": [%d, %d, %d, %d],\n")
			TEXT("  \"state_reset_this_frame\": %s,\n  \"frames_run\": %u,\n  \"frames_with_history\": %u,\n")
			TEXT("  \"r.DefaultFeature.AutoExposure\": %d,\n  \"r.EyeAdaptationQuality\": %d,\n  \"r.PathTracing.SamplesPerPixel\": %d,\n")
			TEXT("  \"SL.Denoiser.RayRepair.Debug\": %d,\n  \"albedo_normal_source\": \"%s\""),
			*Name, *ModelPath, (unsigned long long)FrameNumber, ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X, ViewRect.Max.Y,
			(Hidden.IsEmpty() || bResetHistory) ? TEXT("true") : TEXT("false"), Run, WithHistory,
			CVarInt(TEXT("r.DefaultFeature.AutoExposure")), CVarInt(TEXT("r.EyeAdaptationQuality")),
			CVarInt(TEXT("r.PathTracing.SamplesPerPixel")), DebugView, *GuideSource);
		QueueDump(GraphBuilder, MoveTemp(Tensors), Buffers, ViewInfoBuffer, Meta);
		UE_LOG(LogTemp, Display, TEXT("SLDenoiser%s: dumping frame %llu"), *Name, (unsigned long long)FrameNumber);
	}

	// Keep this frame's hidden_out for the next frame.
	if (!Hidden.IsEmpty())
	{
		History.SetNum(Hidden.Num());
		for (int32 h = 0; h < Hidden.Num(); ++h)
		{
			GraphBuilder.QueueBufferExtraction(HiddenOut[h], &History[h]);
		}
		HistoryFrame = FrameNumber;
		HistoryViewKey = ViewKey;
	}

	// 3. Postprocess: denoised_rgb -> full-resolution linear HDR scene color (no upsample)
	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	OutputDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("RayRepair.Output"));
	{
		FSLRayRepairPostprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLRayRepairPostprocessCS::FParameters>();
		// Debug views 2-4 show ue_buffers through the same pass instead of the network's output.
		const bool bShowInput = DebugView >= 2 && DebugView <= 4;
		Params->InputTensor   = GraphBuilder.CreateSRV(bShowInput ? InputTensorBuffer : OutputTensorBuffer, PF_R32_FLOAT);
		Params->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		Params->ViewRectMin   = ViewRectMin;
		Params->TensorSize    = TensorSizeU;
		Params->FirstChannel  = bShowInput ? 3u * (DebugView - 2) : 0u;
		Params->Scale         = DebugView == 4 ? 0.5f : 1.f;
		Params->Bias          = DebugView == 4 ? 0.5f : 0.f;

		TShaderMapRef<FSLRayRepairPostprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%sPostprocess", *Name), ComputeShader, Params, GroupCount);
	}

	return FScreenPassTexture(OutputTexture, ViewRect);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
ISLDenoiserPass* GetDenoiserForMethod(ESLDenoiserMethod Method)
{
	// Static singletons, one per method. Only a recurrent Ray Repair model keeps
	// state between frames, and it resets that state when the view changes.
	static FSLDenoiserNone               s_None;
	static FSLDenoiserGaussianBlur       s_GaussianBlur;
	static FSLDenoiserBilateralFilter    s_BilateralFilter;
	static FSLDenoiserNeuralPlaceholder  s_NeuralPlaceholder;
	static FSLDenoiserNNE                s_NNE;
	// Methods 5 and 6 share one pass type; each instance keeps its own model and tensor size.
	static FSLDenoiserRayRepair          s_RayRepair(TEXT("RayRepair"),
		[] { return GSLRayRepairModelPath_RenderThread; }, ESLRayRepairInputEncoding::FormatV1);
	static FSLDenoiserRayRepair          s_NBDirect(TEXT("NBDirect"),
		[] { return FString(GSLNBDirectModelPath); }, ESLRayRepairInputEncoding::NoiseBase);
	// Method 7 is the same pass with a recurrent model; the instance keeps that model's hidden state.
	static FSLDenoiserRayRepair          s_RayRepairMotion(TEXT("RayRepairMotion"),
		[] { return GSLRayRepairMotionModelPath_RenderThread; }, ESLRayRepairInputEncoding::NoiseBase);

	switch (Method)
	{
	case ESLDenoiserMethod::GaussianBlur:
		return &s_GaussianBlur;

	case ESLDenoiserMethod::BilateralFilter:
		return &s_BilateralFilter;

	case ESLDenoiserMethod::NeuralPlaceholder:
		return &s_NeuralPlaceholder;

	case ESLDenoiserMethod::NNE:
		return &s_NNE;

	case ESLDenoiserMethod::RayRepair:
		return &s_RayRepair;

	case ESLDenoiserMethod::NBDirect:
		return &s_NBDirect;

	case ESLDenoiserMethod::RayRepairMotion:
		return &s_RayRepairMotion;

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
