// Copyright Epic Games, Inc. All Rights Reserved.
//
// SLDenoiser.cpp
// Implements the modular denoiser pass pipeline.
//
// Console variables
// -----------------
//   SL.Denoiser.Method                int  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder,
//                                          4 = NNE Candy, 5 = RayRepair
//   SL.Denoiser.RayRepair.Model       string  NNE model asset for method 5
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
#include "NNE.h"
#include "NNERuntimeRDG.h"
#include "NNEModelData.h"

// ---------------------------------------------------------------------------
// Console variables
// ---------------------------------------------------------------------------

/** Selects the active denoiser.  0 = None, 1 = GaussianBlur, 2 = BilateralFilter, 3 = NeuralPlaceholder, 4 = NNE Candy, 5 = RayRepair. */
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
	TEXT("Changes take effect on the next rendered frame."),
	ECVF_RenderThreadSafe);

/** NNE model asset loaded by the Ray Repair denoiser (method 5). */
static TAutoConsoleVariable<FString> CVarSLRayRepairModel(
	TEXT("SL.Denoiser.RayRepair.Model"),
	TEXT("/Game/ML/RayRepair_v1_1920x1080"),
	TEXT("NNE model data asset for SL.Denoiser.Method 5, exported by neural-denoiser's denoiser/export_ue.py.\n")
	TEXT("The tensor size is read from the model and must equal the view rect (use r.ScreenPercentage 100).\n")
	TEXT("Changing it reloads the model on the next frame."),
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
IMPLEMENT_GLOBAL_SHADER(FSLNNEPreprocessCS,      "/Project/SLNNEPreprocess.usf",            "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLNNEPostprocessCS,     "/Project/SLNNEPostprocess.usf",           "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLNNEUpsamplePS,        "/Project/SLNNEUpsample.usf",              "MainPS",   SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FSLRayRepairPreprocessCS,  "/Project/SLRayRepairPreprocess.usf",    "MainCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSLRayRepairPostprocessCS, "/Project/SLRayRepairPostprocess.usf",   "MainCS",   SF_Compute);

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
// FSLDenoiserRayRepair::Apply
// Runs a Ray Repair format v1 ONNX model (raw UE buffers in, linear HDR out)
// at full view-rect resolution. See neural-denoiser docs/UE_INTEGRATION.md.
// ---------------------------------------------------------------------------
FScreenPassTexture FSLDenoiserRayRepair::Apply(
	FRDGBuilder&          GraphBuilder,
	const FSceneView&     View,
	const FSLDenoiserContext& Context)
{
	const FScreenPassTexture& Input = Context.SceneColor;
	if (!Input.IsValid()) return Input;

	static constexpr uint32 RawChannels = 10;
	static constexpr uint32 OutChannels = 3;

	// 0. Load the model and create the instance once (again if the CVar changes)
	static FString LoadedPath;
	static bool bLoadFailed = false;
	static TSharedPtr<UE::NNE::IModelInstanceRDG> ModelInstance;
	static FIntPoint TensorSize = FIntPoint::ZeroValue;

	const FString ModelPath = CVarSLRayRepairModel.GetValueOnRenderThread();
	if (ModelPath != LoadedPath)
	{
		LoadedPath  = ModelPath;
		bLoadFailed = true;
		ModelInstance.Reset();

		UNNEModelData* ModelData = LoadObject<UNNEModelData>(nullptr, *ModelPath);
		if (!ModelData)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: Failed to load %s"), *ModelPath);
			return Input;
		}

		TWeakInterfacePtr<INNERuntimeRDG> Runtime = UE::NNE::GetRuntime<INNERuntimeRDG>(TEXT("NNERuntimeORTDml"));
		if (!Runtime.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: NNERuntimeORTDml is not available"));
			return Input;
		}

		TSharedPtr<UE::NNE::IModelRDG> Model = Runtime->CreateModelRDG(ModelData);
		TSharedPtr<UE::NNE::IModelInstanceRDG> Instance = Model.IsValid() ? Model->CreateModelInstanceRDG() : nullptr;
		if (!Instance.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: Failed to create model instance for %s"), *ModelPath);
			return Input;
		}

		// Format v1 still models have exactly one [1, 10, H, W] input with static H, W.
		// History models (motion_px, history_rgb) need frame-to-frame state and are not wired up yet.
		const TConstArrayView<UE::NNE::FTensorDesc> InputDescs = Instance->GetInputTensorDescs();
		if (InputDescs.Num() != 1)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: %s has %d inputs; only single-input (non-history) format v1 models are supported"),
				*ModelPath, InputDescs.Num());
			return Input;
		}
		const UE::NNE::FSymbolicTensorShape& InShape = InputDescs[0].GetShape();
		const TConstArrayView<int32> Dims = InShape.GetData();
		if (!InShape.IsConcrete() || Dims.Num() != 4 || Dims[0] != 1 || Dims[1] != RawChannels)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: %s input is not a static [1, %u, H, W] tensor"), *ModelPath, RawChannels);
			return Input;
		}

		TArray<UE::NNE::FTensorShape> InputShapes = { UE::NNE::FTensorShape::MakeFromSymbolic(InShape) };
		if (Instance->SetInputTensorShapes(InputShapes) != UE::NNE::EResultStatus::Ok)
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: SetInputTensorShapes failed for %s"), *ModelPath);
			return Input;
		}

		TensorSize    = FIntPoint(Dims[3], Dims[2]);
		ModelInstance = Instance;
		bLoadFailed   = false;
		UE_LOG(LogTemp, Display, TEXT("SLDenoiserRayRepair: Loaded %s (%dx%d)"), *ModelPath, TensorSize.X, TensorSize.Y);
	}

	if (bLoadFailed || !ModelInstance.IsValid()) return Input;

	// v1 shapes are static: the view rect must match the export resolution exactly.
	const FIntRect ViewRect = Input.ViewRect;
	if (ViewRect.Size() != TensorSize)
	{
		static FIntPoint LastWarnedSize = FIntPoint::ZeroValue;
		if (LastWarnedSize != ViewRect.Size())
		{
			UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: view rect %dx%d does not match model %dx%d; passing through. ")
				TEXT("Re-export at this resolution or set r.ScreenPercentage 100 and a matching window size."),
				ViewRect.Width(), ViewRect.Height(), TensorSize.X, TensorSize.Y);
			LastWarnedSize = ViewRect.Size();
		}
		return Input;
	}

	if (!Context.SceneTextures.SceneTextures)
	{
		UE_LOG(LogTemp, Warning, TEXT("SLDenoiserRayRepair: scene textures unavailable; passing through"));
		return Input;
	}

	const ERHIFeatureLevel::Type FeatureLevel = View.FeatureLevel;
	const uint32 Plane = static_cast<uint32>(TensorSize.X) * static_cast<uint32>(TensorSize.Y);
	const FUintVector2 ViewRectMin(static_cast<uint32>(ViewRect.Min.X), static_cast<uint32>(ViewRect.Min.Y));
	const FUintVector2 TensorSizeU(static_cast<uint32>(TensorSize.X), static_cast<uint32>(TensorSize.Y));
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(TensorSize, 8);

	// 1. Preprocess: scene color + decoded G-Buffer -> ue_buffers [1, 10, H, W]
	FRDGBufferRef InputTensorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), RawChannels * Plane), TEXT("RayRepair.UEBuffers"));
	{
		FSLRayRepairPreprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLRayRepairPreprocessCS::FParameters>();
		Params->View              = View.ViewUniformBuffer;
		Params->SceneTextures     = Context.SceneTextures;
		Params->SceneColorTexture = Input.Texture;
		Params->ViewRectMin       = ViewRectMin;
		Params->TensorSize        = TensorSizeU;
		Params->OutputTensor      = GraphBuilder.CreateUAV(InputTensorBuffer, PF_R32_FLOAT);

		TShaderMapRef<FSLRayRepairPreprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("RayRepairPreprocess"), ComputeShader, Params, GroupCount);
	}

	// 2. Inference
	FRDGBufferRef OutputTensorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), OutChannels * Plane), TEXT("RayRepair.DenoisedRGB"));

	TArray<UE::NNE::FTensorBindingRDG> InputBindings;
	UE::NNE::FTensorBindingRDG InBinding;
	InBinding.Buffer = InputTensorBuffer;
	InputBindings.Add(InBinding);

	TArray<UE::NNE::FTensorBindingRDG> OutputBindings;
	UE::NNE::FTensorBindingRDG OutBinding;
	OutBinding.Buffer = OutputTensorBuffer;
	OutputBindings.Add(OutBinding);

	if (ModelInstance->EnqueueRDG(GraphBuilder, InputBindings, OutputBindings) != UE::NNE::EResultStatus::Ok)
	{
		UE_LOG(LogTemp, Error, TEXT("SLDenoiserRayRepair: EnqueueRDG failed; passing through"));
		return Input;
	}

	// 3. Postprocess: denoised_rgb -> full-resolution linear HDR scene color (no upsample)
	FRDGTextureDesc OutputDesc = Input.Texture->Desc;
	OutputDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("RayRepair.Output"));
	{
		FSLRayRepairPostprocessCS::FParameters* Params = GraphBuilder.AllocParameters<FSLRayRepairPostprocessCS::FParameters>();
		Params->InputTensor   = GraphBuilder.CreateSRV(OutputTensorBuffer, PF_R32_FLOAT);
		Params->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		Params->ViewRectMin   = ViewRectMin;
		Params->TensorSize    = TensorSizeU;

		TShaderMapRef<FSLRayRepairPostprocessCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("RayRepairPostprocess"), ComputeShader, Params, GroupCount);
	}

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
	static FSLDenoiserNNE                s_NNE;
	static FSLDenoiserRayRepair          s_RayRepair;

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
