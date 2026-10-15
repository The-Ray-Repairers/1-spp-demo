#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"
#include "RendererInterface.h"   // IPooledRenderTarget

/**
 * Captures the main view's linear-HDR scene colour for one requested frame,
 * after the SL denoiser and before tonemapping (the MotionBlur post-process
 * hook). Used by the flythrough benchmark to save EXR frames for HDR metrics.
 *
 * Runs at a lower priority than FSLNoiseFallbackViewExtension so its MotionBlur
 * callback sees the denoised output. Values stay pre-exposed, exactly what the
 * tonemapper consumes (and what Ray Repair's ue_buffers color holds); the
 * View.PreExposure used is returned alongside so runs can be checked for it.
 *
 * Usage (game thread):
 *   RequestCapture(Frame)  — before the frame whose screenshot is requested
 *   ReadCapture(Frame,...) — after that screenshot has been delivered
 */
class FSLHDRCaptureView : public FWorldSceneViewExtension
{
public:
	FSLHDRCaptureView(const FAutoRegister& Register, UWorld* World) : FWorldSceneViewExtension(Register, World) {}

	virtual int32 GetPriority() const override { return -100; }
	virtual void SetupViewFamily(FSceneViewFamily&) override {}
	virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/** Capture the next rendered main view. Ordered with render commands, so it cannot hit an earlier frame. */
	void RequestCapture(int32 FrameIndex);

	/**
	 * Read back the capture for FrameIndex as pre-exposed linear RGB (alpha = 1,
	 * non-finite values zeroed). Flushes rendering; returns false if that frame
	 * was not captured.
	 */
	bool ReadCapture(int32 FrameIndex, TArray<FLinearColor>& OutPixels, FIntPoint& OutSize, float& OutPreExposure);

private:
	FScreenPassTexture Capture_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	// Render thread only
	int32 RequestedFrame_RT = INDEX_NONE;

	// Written on the render thread, read on the game thread after a flush
	FCriticalSection CaptureLock;
	TRefCountPtr<IPooledRenderTarget> CapturedTexture;
	FIntRect CapturedRect;
	float CapturedPreExposure = 1.f;
	int32 CapturedFrame = INDEX_NONE;
};
