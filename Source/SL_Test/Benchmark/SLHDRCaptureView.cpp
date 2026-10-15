#include "SLHDRCaptureView.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

void FSLHDRCaptureView::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// MotionBlur is the last pre-tonemap hook; the SL denoiser runs there at a higher priority.
	// Offline (reference) views are included: they are the 64/512 SPP ground truth.
	if (Pass != EPostProcessingPass::MotionBlur || View.bIsSceneCapture || RequestedFrame_RT == INDEX_NONE)
		return;

	InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSLHDRCaptureView::Capture_RenderThread));
}

void FSLHDRCaptureView::RequestCapture(int32 FrameIndex)
{
	ENQUEUE_RENDER_COMMAND(SLHDRCaptureRequest)([this, FrameIndex](FRHICommandListImmediate&)
	{
		RequestedFrame_RT = FrameIndex;
	});
}

FScreenPassTexture FSLHDRCaptureView::Capture_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	FScreenPassTexture SceneColor(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid() || RequestedFrame_RT == INDEX_NONE)
		return SceneColor;

	// Copy just the view rect so later passes cannot alter what we read back.
	const FIntRect ViewRect = SceneColor.ViewRect;
	FRDGTextureDesc Desc = SceneColor.Texture->Desc;
	Desc.Extent = ViewRect.Size();
	Desc.Flags = TexCreate_ShaderResource;
	Desc.NumMips = 1;
	FRDGTextureRef Copy = GraphBuilder.CreateTexture(Desc, TEXT("SLHDRCapture.SceneColor"));

	FRHICopyTextureInfo CopyInfo;
	CopyInfo.SourcePosition = FIntVector(ViewRect.Min.X, ViewRect.Min.Y, 0);
	CopyInfo.Size = FIntVector(ViewRect.Width(), ViewRect.Height(), 1);
	AddCopyTexturePass(GraphBuilder, SceneColor.Texture, Copy, CopyInfo);

	{
		FScopeLock Lock(&CaptureLock);
		GraphBuilder.QueueTextureExtraction(Copy, &CapturedTexture);
		CapturedRect = FIntRect(FIntPoint::ZeroValue, ViewRect.Size());
		// Scene colour is pre-exposed by the previous frame's eye adaptation exposure.
		const float Exposure = View.GetLastEyeAdaptationExposure();
		CapturedPreExposure = Exposure > 0.f ? Exposure : 1.f;
		CapturedFrame = RequestedFrame_RT;
	}
	RequestedFrame_RT = INDEX_NONE;

	return SceneColor;
}

bool FSLHDRCaptureView::ReadCapture(int32 FrameIndex, TArray<FLinearColor>& OutPixels, FIntPoint& OutSize, float& OutPreExposure)
{
	FlushRenderingCommands();

	TRefCountPtr<IPooledRenderTarget> Texture;
	FIntRect Rect;
	float PreExposure = 1.f;
	{
		FScopeLock Lock(&CaptureLock);
		if (CapturedFrame != FrameIndex || !CapturedTexture.IsValid())
			return false;
		Texture = MoveTemp(CapturedTexture);
		Rect = CapturedRect;
		PreExposure = CapturedPreExposure;
		CapturedFrame = INDEX_NONE;
	}

	TArray<FLinearColor>* Out = &OutPixels;
	ENQUEUE_RENDER_COMMAND(SLHDRCaptureRead)([Texture, Rect, Out](FRHICommandListImmediate& RHICmdList)
	{
		// RCM_MinMax keeps raw float values instead of remapping them into [0, 1].
		RHICmdList.ReadSurfaceData(Texture->GetRHI(), Rect, *Out, FReadSurfaceDataFlags(RCM_MinMax));
	});
	FlushRenderingCommands();

	OutSize = Rect.Size();
	OutPreExposure = PreExposure;
	if (OutPixels.Num() != OutSize.X * OutSize.Y)
		return false;

	for (FLinearColor& C : OutPixels)
	{
		C.R = FMath::IsFinite(C.R) ? C.R : 0.f;
		C.G = FMath::IsFinite(C.G) ? C.G : 0.f;
		C.B = FMath::IsFinite(C.B) ? C.B : 0.f;
		C.A = 1.f;
	}
	return true;
}
