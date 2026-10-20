#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/Scene.h"
#include "SLFlythroughMetrics.h"
#include "SLFlythroughSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FSLFlythroughStateChanged);

/** Describes a benchmarkable rendering method. */
struct FSLFlythroughMethod
{
	/** Display name and directory name, e.g. "1SPP_Noisy". */
	FString Name;
	/** Target samples per pixel for the path tracer. */
	int32 SPP = 1;
	/** SL.Denoiser.Method value (-1 = do not set). */
	int32 DenoiserMethod = -1;
	/** Whether to accumulate samples (offline-style for GT). */
	bool bAccumulate = false;
	/** The reference every method is scored against. Its SPP and name follow SL.Flythrough.GTSPP. */
	bool bIsGT = false;
	/** Additional console variable overrides applied before rendering. */
	TArray<TPair<FString, FString>> CVarOverrides;
};

/** Flythrough benchmark state machine phases. */
UENUM()
enum class ESLFlythroughPhase : uint8
{
	Idle,
	/** Waiting one frame for settings to apply before capturing begins. */
	WarmUp,
	/** Rendering and capturing frames. */
	Rendering,
	/** Accumulating SPP for the current frame (multi-SPP GT mode). */
	Accumulating,
	/** Waiting for the screenshot delegate to fire. */
	AwaitingScreenshot,
	/** Flythrough complete. */
	Finished
};

/**
 * Manages deterministic camera flythrough benchmarks for comparing denoiser
 * quality against a high-SPP ground truth baseline.
 *
 * Usage (from the GM panel):
 *  1. Select a method
 *  2. Click "Render Flythrough" — the subsystem samples the level's camera sequence
 *     and captures each frame as a PNG
 *  3. Once the GT baseline (64SPP_GT by default) exists, click "Compute Metrics" to score
 *     all rendered methods
 */
UCLASS()
class USLFlythroughSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// ---- Lifecycle ----
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return IsBusy() || bBatchActive; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	// ---- Method Registry ----
	const TArray<FSLFlythroughMethod>& GetMethods() const { return Methods; }
	int32 GetSelectedMethodIndex() const { return SelectedMethod; }
	void SelectMethod(int32 Index);
	const FSLFlythroughMethod* GetSelectedMethod() const;

	// ---- Flythrough Control ----
	bool CanStartFlythrough(FText& Reason) const;
	bool StartFlythrough();
	void CancelFlythrough();
	bool IsBusy() const { return Phase != ESLFlythroughPhase::Idle && Phase != ESLFlythroughPhase::Finished; }
	/** Capture GT and every method in order, then compute metrics. Used by SL.Flythrough.RunAll. */
	bool StartBatch(bool bQuitWhenDone);

	// ---- Metrics ----
	bool CanComputeMetrics(FText& Reason) const;
	bool ComputeMetrics();
	bool IsComputingMetrics() const { return bComputingMetrics; }

	// ---- Status ----
	FText GetStatusText() const;
	float GetProgress() const;
	int32 GetCurrentFrame() const { return CurrentFrame; }
	int32 GetTotalFrames() const { return TotalFrames; }

	// ---- Run Detection ----
	bool HasCompletedRun(const FString& MethodName) const;
	bool HasGTBaseline() const;
	/** e.g. "64SPP_GT"; also the GT results folder name. */
	FString GetGTMethodName() const;
	/** Re-read SL.Flythrough.GTSPP into the GT method. Ignored while a capture is running. */
	void ApplyGTSPP();

	// ---- Summary Data (for UI charts) ----
	const TArray<FSLMethodSummary>& GetSummaries() const { return CachedSummaries; }
	void RefreshSummaries();
	void OnFallbackModeChanged();

	// ---- Telemetry ----
	double GetLastFrameTimeMs() const { return LastFrameTimeMs; }
	double GetCurrentFPS() const { return LastFrameTimeMs > 0.0 ? 1000.0 / LastFrameTimeMs : 0.0; }

	// ---- Events ----
	FSLFlythroughStateChanged OnStateChanged;

	// ---- Renders directory ----
	FString GetRendersRoot() const;
	/** Opt-in synthetic pipeline testing on machines without hardware ray tracing. */
	bool IsUsingSoftwareFallback() const;

private:
	void RefreshSummariesForRoot(const FString& RendersRoot);
	void RegisterMethods();
	bool ResolveScenario(class ASLFlythroughScenario*& OutScenario, FText& Reason) const;
	bool SetupCamera();
	void ReleaseCamera();
	bool ValidateSequenceBindings(FText& Reason);
	bool EvaluateSequenceFrame(int32 FrameIndex);
	void ApplyMethodSettings(const FSLFlythroughMethod& Method);
	void RestoreSettings();
	void CaptureFrame();
	void OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Pixels);
	void AdvanceFrame();
	void FinishFlythrough();
	void TickRendering();
	void TickAccumulating();
	void RestartCurrentFrame();
	bool ChooseRunDirectory(const FString& MethodDir, FSLFlythroughRunManifest& Manifest, bool& bOutAlreadyComplete);
	void TickBatch();
	void EndBatch(bool bSucceeded, const FString& Message);

	// ---- Method Registry ----
	TArray<FSLFlythroughMethod> Methods;
	int32 SelectedMethod = 0;

	// ---- Flythrough State ----
	ESLFlythroughPhase Phase = ESLFlythroughPhase::Idle;
	int32 CurrentFrame = 0;
	int32 TotalFrames = 0;
	double FlythroughDuration = 0.0;
	int32 TargetFPS = 30;
	double FixedDeltaTime = 1.0 / 30.0;
	int32 SequenceStartFrame = 0;

	// ---- Camera ----
	UPROPERTY(Transient) TObjectPtr<class ACameraActor> FlythroughCamera;
	UPROPERTY(Transient) TObjectPtr<class ALevelSequenceActor> SequenceActor;
	UPROPERTY(Transient) TObjectPtr<class ULevelSequencePlayer> SequencePlayer;
	UPROPERTY(Transient) TObjectPtr<class ASLFlythroughScenario> ActiveScenario;
	UPROPERTY(Transient) TObjectPtr<AActor> PreviousViewTarget;
	FString SourcePostProcessSettings;
	FPostProcessSettings SavedCameraPostProcessSettings;
	// Fallback for pre-animated state; restored after the sequence player's own restore.
	FTransform SavedCameraActorTransform;
	FTransform SavedCameraComponentTransform;
	float SavedCameraFieldOfView = 90.f;
	float SavedCameraAspectRatio = 1.f;
	float SavedCameraOrthoWidth = 512.f;
	bool bSavedCameraConstrainAspectRatio = false;
	bool bWasPaused = false;

	// ---- Output ----
	FString CurrentRunDir;
	FString CurrentRunRoot;
	FString CurrentMethodName;

	// ---- Timing ----
	TArray<FSLFrameTiming> FrameTimings;
	double FrameStartTime = 0.0;
	double LastFrameTimeMs = 0.0;

	// ---- Screenshot ----
	FDelegateHandle ScreenshotHandle;
	FIntPoint CaptureSize = FIntPoint::ZeroValue;

	// ---- Accumulation (multi-SPP) ----
	TSharedPtr<class FSLComparisonView, ESPMode::ThreadSafe> AccumulationView;
	// Only owned here if normal startup did not create the existing fallback view.
	TSharedPtr<class FSLNoiseFallbackViewExtension, ESPMode::ThreadSafe> SoftwareFallbackView;
	bool bSoftwareFallbackRun = false;
	int32 TargetSPP = 1;

	// ---- Settings Restore ----
	TMap<FString, FString> SavedCVars;
	double SavedFixedDeltaTime = 0.0;
	bool bSavedUseFixedDeltaTime = false;
	bool bSavedPathTracing = false;
	bool bSavedDLSS = false;
	bool bSavedRR = false;
	bool bSavedEditorThrottle = false;
	/** All windows were minimized mid-frame; nothing renders then, so the frame is redone. */
	bool bFrameInterrupted = false;

	// ---- Batch capture ----
	TArray<int32> BatchQueue;
	bool bBatchActive = false;
	bool bBatchQuitWhenDone = false;
	bool bBatchRunInFlight = false;
	double BatchWaitStart = 0.0;

	// ---- Metrics ----
	bool bComputingMetrics = false;
	FString MetricsStatus;

	// ---- Summary Cache ----
	TArray<FSLMethodSummary> CachedSummaries;
	TSet<FString> CompletedMethods;
};
