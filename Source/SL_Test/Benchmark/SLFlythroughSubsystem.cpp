#include "SLFlythroughSubsystem.h"
#include "SLComparisonView.h"
#include "SLComparisonSubsystem.h"
#include "SL_Test.h"
#include "DLSSLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "UnrealClient.h"

DECLARE_CYCLE_STAT(TEXT("SLFlythroughSubsystem"), STAT_SLFlythrough, STATGROUP_Tickables);

TStatId USLFlythroughSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USLFlythroughSubsystem, STATGROUP_Tickables);
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	RegisterMethods();
	RefreshSummaries();
}

void USLFlythroughSubsystem::Deinitialize()
{
	if (IsBusy()) CancelFlythrough();
	Super::Deinitialize();
}

// -----------------------------------------------------------------------
// Method Registry
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::RegisterMethods()
{
	Methods.Reset();

	// 1SPP_Noisy — raw path tracer, no denoising
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_Noisy");
		M.SPP = 1;
		M.DenoiserMethod = 0;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 1SPP_Gaussian
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_Gaussian");
		M.SPP = 1;
		M.DenoiserMethod = 1;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 1SPP_Bilateral
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_Bilateral");
		M.SPP = 1;
		M.DenoiserMethod = 2;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 1SPP_NeuralPlaceholder
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_NeuralPlaceholder");
		M.SPP = 1;
		M.DenoiserMethod = 3;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 1SPP_NNE
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_NNE");
		M.SPP = 1;
		M.DenoiserMethod = 4;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 64SPP_GT — ground truth baseline
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("64SPP_GT");
		M.SPP = 64;
		M.DenoiserMethod = 0; // no denoising on GT
		M.bAccumulate = true;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("0")});
		Methods.Add(MoveTemp(M));
	}

	// 1024SPP_GT — high-quality reference; preferred by Compute Metrics when complete
	{
		FSLFlythroughMethod M;
		M.Name = HighQualityReferenceName;
		M.SPP = 1024;
		M.DenoiserMethod = 0; // no denoising on GT
		M.bAccumulate = true;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("0")});
		Methods.Add(MoveTemp(M));
	}
}

void USLFlythroughSubsystem::SelectMethod(int32 Index)
{
	if (!IsBusy() && Index >= 0 && Index < Methods.Num())
		SelectedMethod = Index;
}

const FSLFlythroughMethod* USLFlythroughSubsystem::GetSelectedMethod() const
{
	return Methods.IsValidIndex(SelectedMethod) ? &Methods[SelectedMethod] : nullptr;
}

// -----------------------------------------------------------------------
// Renders Directory
// -----------------------------------------------------------------------

FString USLFlythroughSubsystem::GetRendersRoot() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("renders")));
}

// -----------------------------------------------------------------------
// Run Detection
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::HasCompletedRun(const FString& MethodName) const
{
	return CompletedMethods.Contains(MethodName);
}

bool USLFlythroughSubsystem::HasGTBaseline() const
{
	return !GetReferenceMethodName().IsEmpty();
}

FString USLFlythroughSubsystem::GetReferenceMethodName() const
{
	// Highest-SPP reference with a completed run wins.
	for (const TCHAR* Name : { HighQualityReferenceName, TEXT("64SPP_GT") })
	{
		if (HasCompletedRun(Name)) return Name;
	}
	return FString();
}

bool USLFlythroughSubsystem::NeedsReferencePrompt() const
{
	// Reference runs themselves never need one; everything else is scored against it.
	const FSLFlythroughMethod* Method = GetSelectedMethod();
	return Method && !Method->bAccumulate && !HasCompletedRun(HighQualityReferenceName);
}

bool USLFlythroughSubsystem::StartReferenceFlythrough()
{
	const int32 Index = Methods.IndexOfByPredicate([](const FSLFlythroughMethod& M) { return M.Name == HighQualityReferenceName; });
	if (Index == INDEX_NONE || IsBusy()) return false;
	SelectMethod(Index);
	return StartFlythrough();
}

// -----------------------------------------------------------------------
// Summary Cache
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::RefreshSummaries()
{
	FSLFlythroughMetrics::ReadSummaryCSV(GetRendersRoot(), CachedSummaries);
	// Slate polls run colors every paint; validate frame files only on refresh.
	CompletedMethods.Reset();
	for (const FSLFlythroughMethod& Method : Methods)
	{
		if (!FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(GetRendersRoot(), Method.Name)).IsEmpty())
			CompletedMethods.Add(Method.Name);
	}
}

// -----------------------------------------------------------------------
// Camera Path — Circular Orbit
// -----------------------------------------------------------------------

FTransform USLFlythroughSubsystem::GetCameraTransformAtTime(double Time) const
{
	// Full 360° orbit over FlythroughDuration
	const double Angle = (Time / FlythroughDuration) * 2.0 * PI;
	const FVector Position = OrbitCenter + FVector(
		OrbitRadius * FMath::Cos(Angle),
		OrbitRadius * FMath::Sin(Angle),
		OrbitHeight);
	const FVector LookDir = (OrbitCenter - Position).GetSafeNormal();
	FRotator Rotation = LookDir.Rotation();
	Rotation.Pitch = OrbitPitch;
	return FTransform(Rotation, Position);
}

// -----------------------------------------------------------------------
// Settings Management
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::ApplyMethodSettings(const FSLFlythroughMethod& Method)
{
	SavedCVars.Reset();
	bSavedPathTracing = GetWorld()->GetGameViewport()->EngineShowFlags.PathTracing;
	bSavedDLSS = UDLSSLibrary::IsDLSSEnabled();
	bSavedRR = UDLSSLibrary::IsDLSSRREnabled();
	UDLSSLibrary::EnableDLSSRR(false);
	UDLSSLibrary::EnableDLSS(false);
	UKismetRenderingLibrary::EnablePathTracing(true);

	// Save & set SPP
	auto SetCVar = [this](const FString& Name, const FString& Value)
	{
		if (auto* Var = IConsoleManager::Get().FindConsoleVariable(*Name))
		{
			if (!SavedCVars.Contains(Name))
				SavedCVars.Add(Name, Var->GetString());
			Var->Set(*Value, ECVF_SetByConsole);
		}
	};

	SetCVar(TEXT("r.PathTracing.SamplesPerPixel"), FString::FromInt(Method.SPP));
	SetCVar(TEXT("SL.PathTracer.TargetSPP"), FString::FromInt(Method.SPP));
	SetCVar(TEXT("r.PathTracing.Denoiser"), TEXT("0"));
	SetCVar(TEXT("r.PathTracing.SpatialDenoiser"), TEXT("0"));
	SetCVar(TEXT("r.PathTracing.TemporalDenoiser"), TEXT("0"));
	SetCVar(TEXT("r.PathTracing.FrameIndependentTemporalSeed"), TEXT("0"));
	SetCVar(TEXT("r.ScreenshotDelegate"), TEXT("1"));
	SetCVar(TEXT("r.DefaultFeature.AutoExposure"), TEXT("0"));
	SetCVar(TEXT("r.EyeAdaptationQuality"), TEXT("0"));

	if (Method.DenoiserMethod >= 0)
		SetCVar(TEXT("SL.Denoiser.Method"), FString::FromInt(Method.DenoiserMethod));

	for (const auto& Override : Method.CVarOverrides)
		SetCVar(Override.Key, Override.Value);

	// Lock fixed delta time for deterministic frame pacing
	SavedFixedDeltaTime = FApp::GetFixedDeltaTime();
	bSavedUseFixedDeltaTime = FApp::UseFixedTimeStep();
	FApp::SetFixedDeltaTime(FixedDeltaTime);
	FApp::SetUseFixedTimeStep(true);
}

void USLFlythroughSubsystem::RestoreSettings()
{
	// TargetSPP's change delegate also writes the engine SPP; restore it first.
	const FString TargetName = TEXT("SL.PathTracer.TargetSPP");
	if (const FString* SavedTarget = SavedCVars.Find(TargetName))
	{
		if (auto* Var = IConsoleManager::Get().FindConsoleVariable(*TargetName))
			Var->Set(**SavedTarget, ECVF_SetByConsole);
	}
	for (const auto& Pair : SavedCVars)
	{
		if (Pair.Key == TargetName) continue;
		if (auto* Var = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
			Var->Set(*Pair.Value, ECVF_SetByConsole);
	}
	SavedCVars.Reset();
	FApp::SetFixedDeltaTime(SavedFixedDeltaTime);
	FApp::SetUseFixedTimeStep(bSavedUseFixedDeltaTime);
	UKismetRenderingLibrary::EnablePathTracing(bSavedPathTracing);
	UDLSSLibrary::EnableDLSS(bSavedDLSS);
	UDLSSLibrary::EnableDLSSRR(bSavedRR);
}

// -----------------------------------------------------------------------
// Camera Setup / Teardown
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::SetupCamera()
{
	auto* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;

	const auto POV = PC->PlayerCameraManager->GetCameraCacheView();
	OrbitCenter = POV.Location;
	PreviousViewTarget = PC->GetViewTarget();
	bWasPaused = PC->IsPaused();

	FlythroughCamera = GetWorld()->SpawnActor<ACameraActor>(POV.Location, POV.Rotation);
	if (FlythroughCamera)
	{
		FlythroughCamera->GetCameraComponent()->FieldOfView = POV.FOV;
		FlythroughCamera->GetCameraComponent()->PostProcessSettings = POV.PostProcessSettings;
		auto& PP = FlythroughCamera->GetCameraComponent()->PostProcessSettings;
		PP.bOverride_PathTracingSamplesPerPixel = true;
		PP.PathTracingSamplesPerPixel = TargetSPP;
		PC->SetViewTarget(FlythroughCamera);
		PC->SetPause(true);
	}
}

void USLFlythroughSubsystem::ReleaseCamera()
{
	if (FlythroughCamera)
	{
		if (auto* PC = GetWorld()->GetFirstPlayerController())
		{
			if (PreviousViewTarget) PC->SetViewTarget(PreviousViewTarget);
			PC->SetPause(bWasPaused);
		}
		FlythroughCamera->Destroy();
		FlythroughCamera = nullptr;
		PreviousViewTarget = nullptr;
	}

	if (AccumulationView)
	{
		FlushRenderingCommands();
		AccumulationView.Reset();
		FlushRenderingCommands();
	}
}

// -----------------------------------------------------------------------
// Flythrough Control
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::CanStartFlythrough(FText& Reason) const
{
	if (IsBusy())
	{
		Reason = FText::FromString(TEXT("A flythrough is already in progress."));
		return false;
	}
	if (const auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>(); Comparison && Comparison->IsBusy())
	{
		Reason = FText::FromString(TEXT("Wait for the comparison capture to finish."));
		return false;
	}
	if (!IsRayTracingEnabled())
	{
		Reason = FText::FromString(TEXT("Flythrough benchmarks require hardware ray tracing."));
		return false;
	}
	if (FScreenshotRequest::IsScreenshotRequested())
	{
		Reason = FText::FromString(TEXT("Another screenshot is in progress."));
		return false;
	}
	if (!GetWorld()->GetFirstPlayerController() || !GetWorld()->GetGameViewport())
	{
		Reason = FText::FromString(TEXT("Start the game first."));
		return false;
	}
	if (!GetWorld()->GetGameViewport()->Viewport)
	{
		Reason = FText::FromString(TEXT("The game viewport is not ready."));
		return false;
	}
	if (GetWorld()->GetGameViewport()->Viewport->GetSizeXY().GetMin() <= 0)
	{
		Reason = FText::FromString(TEXT("The viewport has no valid image size."));
		return false;
	}
	if (!GetSelectedMethod())
	{
		Reason = FText::FromString(TEXT("No method selected."));
		return false;
	}
	return true;
}

bool USLFlythroughSubsystem::StartFlythrough()
{
	FText Reason;
	if (!CanStartFlythrough(Reason))
	{
		UE_LOG(LogSL_Test, Warning, TEXT("Flythrough: Cannot start — %s"), *Reason.ToString());
		return false;
	}

	const FSLFlythroughMethod& Method = *GetSelectedMethod();
	CurrentMethodName = Method.Name;
	// Release comparison overlays, view extensions and renderer overrides first.
	if (auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>(); Comparison && Comparison->OwnsRenderer())
		Comparison->Restore();

	// Setup output directory
	const FString MethodDir = FPaths::Combine(GetRendersRoot(), CurrentMethodName);
	const FString RunId = FSLFlythroughMetrics::GetNextRunId(MethodDir);
	CurrentRunDir = FPaths::Combine(MethodDir, RunId);
	if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(CurrentRunDir, TEXT("frames")), true))
	{
		MetricsStatus = TEXT("Unable to create the flythrough output directory.");
		return false;
	}

	// Setup frames
	TotalFrames = FMath::RoundToInt32(FlythroughDuration * TargetFPS);
	CurrentFrame = 0;
	FlythroughTime = 0.0;
	FixedDeltaTime = 1.0 / (double)TargetFPS;
	FrameTimings.Reset();
	FrameTimings.Reserve(TotalFrames);
	TargetSPP = Method.SPP;

	// Setup viewport size tracking
	CaptureSize = GetWorld()->GetGameViewport()->Viewport->GetSizeXY();

	// Setup camera
	SetupCamera();
	if (!FlythroughCamera)
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Failed to create camera."));
		return false;
	}

	// Apply method settings
	ApplyMethodSettings(Method);

	// For multi-SPP accumulation, create a view extension to track samples
	if (Method.bAccumulate)
	{
		AccumulationView = FSceneViewExtensions::NewExtension<FSLComparisonView>(GetWorld());
		AccumulationView->bReference = true;
		AccumulationView->DenoiserOverride = -1;
		AccumulationView->Samples.Store(0);
	}

	// Register screenshot delegate
	ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(
		this, &USLFlythroughSubsystem::OnScreenshotCaptured);

	// Position camera at frame 0
	FlythroughCamera->SetActorTransform(GetCameraTransformAtTime(0.0));
	GetWorld()->GetFirstPlayerController()->PlayerCameraManager->UpdateCamera(0.f);

	// Warm up for one frame to let settings apply
	Phase = ESLFlythroughPhase::WarmUp;
	FrameStartTime = FPlatformTime::Seconds();
	MetricsStatus.Reset();
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Started %s — %d frames at %d FPS, SPP=%d"),
		*CurrentMethodName, TotalFrames, TargetFPS, TargetSPP);

	return true;
}

void USLFlythroughSubsystem::CancelFlythrough()
{
	if (!IsBusy()) return;

	UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle);
	ScreenshotHandle.Reset();

	if (FScreenshotRequest::IsScreenshotRequested())
		FScreenshotRequest::Reset();

	ReleaseCamera();
	RestoreSettings();

	Phase = ESLFlythroughPhase::Idle;
	MetricsStatus = TEXT("Flythrough cancelled.");
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Cancelled at frame %d/%d"), CurrentFrame, TotalFrames);
}

// -----------------------------------------------------------------------
// Tick
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::Tick(float DeltaTime)
{
	if (IsBusy())
	{
		auto* Viewport = GetWorld()->GetGameViewport();
		if (!Viewport || !Viewport->Viewport || Viewport->Viewport->GetSizeXY() != CaptureSize
			|| (Phase != ESLFlythroughPhase::WarmUp && FPlatformTime::Seconds() - FrameStartTime > 600.0))
		{
			CancelFlythrough();
			MetricsStatus = TEXT("Capture stopped: viewport changed or frame timed out.");
			return;
		}
	}
	switch (Phase)
	{
	case ESLFlythroughPhase::WarmUp:
		// After one tick of warm-up, start rendering
		Phase = ESLFlythroughPhase::Rendering;
		break;

	case ESLFlythroughPhase::Rendering:
		TickRendering();
		break;

	case ESLFlythroughPhase::Accumulating:
		TickAccumulating();
		break;

	case ESLFlythroughPhase::AwaitingScreenshot:
		// Waiting for OnScreenshotCaptured delegate
		break;

	case ESLFlythroughPhase::Finished:
		// Stay in finished state until user takes action
		break;

	default:
		break;
	}
}

void USLFlythroughSubsystem::TickRendering()
{
	if (CurrentFrame >= TotalFrames)
	{
		FinishFlythrough();
		return;
	}

	// Check viewport size hasn't changed
	if (GetWorld()->GetGameViewport()->Viewport->GetSizeXY() != CaptureSize)
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Viewport resized during capture."));
		CancelFlythrough();
		return;
	}

	const FSLFlythroughMethod& Method = *GetSelectedMethod();

	if (Method.bAccumulate)
	{
		// The warm-up view applies the camera change before we trust sample counts.
		// PreRenderView can otherwise report the previous camera's completed SPP.
		FlushRenderingCommands();
		if (AccumulationView)
			AccumulationView->Samples.Store(0);

		Phase = ESLFlythroughPhase::Accumulating;
	}
	else
	{
		// Single-SPP: capture immediately
		CaptureFrame();
	}
}

void USLFlythroughSubsystem::TickAccumulating()
{
	if (!AccumulationView) return;

	const uint32 CompletedSamples = AccumulationView->Samples.Load();
	if ((int32)CompletedSamples >= TargetSPP)
	{
		// Accumulation done for this frame, capture it
		CaptureFrame();
	}
	// Otherwise, keep waiting for accumulation to finish
}

// -----------------------------------------------------------------------
// Frame Capture
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::CaptureFrame()
{
	// Request screenshot
	Phase = ESLFlythroughPhase::AwaitingScreenshot;
	FScreenshotRequest::RequestScreenshot(false);
}

void USLFlythroughSubsystem::OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Pixels)
{
	if (Phase != ESLFlythroughPhase::AwaitingScreenshot) return;
	// Include completion/readback of this frame, but exclude PNG compression and disk I/O.
	LastFrameTimeMs = (FPlatformTime::Seconds() - FrameStartTime) * 1000.0;
	FSLFrameTiming Timing;
	Timing.FrameIndex = CurrentFrame;
	Timing.RenderTimeMs = LastFrameTimeMs;
	FrameTimings.Add(Timing);

	// Verify dimensions
	if (FIntPoint(Width, Height) != CaptureSize)
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Screenshot size mismatch (%dx%d vs %dx%d)"),
			Width, Height, CaptureSize.X, CaptureSize.Y);
		CancelFlythrough();
		return;
	}

	// Save as PNG
	const FString FramePath = FPaths::Combine(CurrentRunDir, TEXT("frames"),
		FString::Printf(TEXT("frame_%04d.png"), CurrentFrame));

	TArray64<uint8> PNG;
	FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
	if (PNG.IsEmpty() || !FFileHelper::SaveArrayToFile(PNG, *FramePath))
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Failed to save frame %d"), CurrentFrame);
		CancelFlythrough();
		return;
	}

	AdvanceFrame();
}

void USLFlythroughSubsystem::AdvanceFrame()
{
	++CurrentFrame;
	FlythroughTime = CurrentFrame * FixedDeltaTime;

	if (CurrentFrame >= TotalFrames)
	{
		FinishFlythrough();
		return;
	}

	// Move camera to next position
	FlythroughCamera->SetActorTransform(GetCameraTransformAtTime(FlythroughTime));
	// World gameplay is paused; explicitly refresh the cached camera for the next render.
	GetWorld()->GetFirstPlayerController()->PlayerCameraManager->UpdateCamera(0.f);

	// Reset accumulation for multi-SPP
	if (AccumulationView)
		AccumulationView->Samples.Store(0);

	// Continue rendering
	Phase = GetSelectedMethod()->bAccumulate
		? ESLFlythroughPhase::WarmUp
		: ESLFlythroughPhase::Rendering;

	FrameStartTime = FPlatformTime::Seconds();
	OnStateChanged.Broadcast();
}

void USLFlythroughSubsystem::FinishFlythrough()
{
	// Write timings CSV
	if (!FSLFlythroughMetrics::WriteTimingsCSV(
		FPaths::Combine(CurrentRunDir, TEXT("timings.csv")), FrameTimings))
	{
		CancelFlythrough();
		MetricsStatus = TEXT("Failed to save timings.csv; run is incomplete.");
		return;
	}

	// Cleanup
	UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle);
	ScreenshotHandle.Reset();
	ReleaseCamera();
	RestoreSettings();

	Phase = ESLFlythroughPhase::Finished;
	MetricsStatus = FString::Printf(TEXT("Flythrough complete: %s — %d frames captured."),
		*CurrentMethodName, TotalFrames);

	RefreshSummaries();
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: %s complete. Output: %s"), *CurrentMethodName, *CurrentRunDir);
}

// -----------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------

FText USLFlythroughSubsystem::GetStatusText() const
{
	if (bComputingMetrics) return FText::FromString(MetricsStatus);
	if (Phase == ESLFlythroughPhase::Idle && !MetricsStatus.IsEmpty())
		return FText::FromString(MetricsStatus);
	if (Phase == ESLFlythroughPhase::Finished)
		return FText::FromString(MetricsStatus);
	if (IsBusy())
		return FText::FromString(FString::Printf(TEXT("Rendering %s: frame %d / %d (%.1f ms, %.0f FPS)"),
			*CurrentMethodName, CurrentFrame, TotalFrames, LastFrameTimeMs, GetCurrentFPS()));
	return FText::GetEmpty();
}

float USLFlythroughSubsystem::GetProgress() const
{
	if (TotalFrames == 0) return 0.f;
	return (float)CurrentFrame / (float)TotalFrames;
}

// -----------------------------------------------------------------------
// Metrics Computation
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::CanComputeMetrics(FText& Reason) const
{
	if (IsBusy())
	{
		Reason = FText::FromString(TEXT("A flythrough is in progress."));
		return false;
	}
	if (bComputingMetrics)
	{
		Reason = FText::FromString(TEXT("Metrics are already being computed."));
		return false;
	}
	if (!HasGTBaseline())
	{
		Reason = FText::FromString(TEXT("Render a 1024SPP_GT (or 64SPP_GT) reference first."));
		return false;
	}
	return true;
}

bool USLFlythroughSubsystem::ComputeMetrics()
{
	RefreshSummaries();
	FText Reason;
	if (!CanComputeMetrics(Reason))
	{
		MetricsStatus = Reason.ToString();
		return false;
	}

	bComputingMetrics = true;
	MetricsStatus = TEXT("Computing metrics...");
	OnStateChanged.Broadcast();

	const FString Root = GetRendersRoot();
	const FString ReferenceName = GetReferenceMethodName();
	const FString GTDir = FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(Root, ReferenceName));
	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Scoring against %s (%s)"), *ReferenceName, *GTDir);

	// Load GT frames
	TArray<FString> GTFrameFiles;
	IFileManager::Get().FindFiles(GTFrameFiles, *FPaths::Combine(GTDir, TEXT("frames"), TEXT("*.png")), true, false);
	GTFrameFiles.Sort();

	if (GTFrameFiles.Num() == 0)
	{
		MetricsStatus = TEXT("No GT frames found.");
		bComputingMetrics = false;
		OnStateChanged.Broadcast();
		return false;
	}

	TArray<FSLMethodSummary> Summaries;

	for (const FSLFlythroughMethod& Method : Methods)
	{
		const FString MethodDir = FPaths::Combine(Root, Method.Name);
		const FString LatestRun = FSLFlythroughMetrics::FindLatestRun(MethodDir);
		if (LatestRun.IsEmpty() || !FSLFlythroughMetrics::IsRunComplete(LatestRun))
			continue;

		MetricsStatus = FString::Printf(TEXT("Computing metrics for %s..."), *Method.Name);

		TArray<FString> TestFrameFiles;
		IFileManager::Get().FindFiles(TestFrameFiles, *FPaths::Combine(LatestRun, TEXT("frames"), TEXT("*.png")), true, false);
		TestFrameFiles.Sort();

		if (TestFrameFiles != GTFrameFiles)
		{
			UE_LOG(LogSL_Test, Warning, TEXT("Flythrough: %s frame sequence does not match GT."), *Method.Name);
			continue;
		}
		const int32 FrameCount = GTFrameFiles.Num();
		TArray<FSLFrameMetrics> AllMetrics;
		AllMetrics.Reserve(FrameCount);

		for (int32 i = 0; i < FrameCount; ++i)
		{
			TArray<FColor> TestPixels, GTPixels;
			int32 TW, TH, GW, GH;

			const FString TestPath = FPaths::Combine(LatestRun, TEXT("frames"), TestFrameFiles[i]);
			const FString GTPath = FPaths::Combine(GTDir, TEXT("frames"), GTFrameFiles[i]);

			if (!FSLFlythroughMetrics::LoadPNG(TestPath, TestPixels, TW, TH)) continue;
			if (!FSLFlythroughMetrics::LoadPNG(GTPath, GTPixels, GW, GH)) continue;
			if (TW != GW || TH != GH) continue;

			AllMetrics.Add(FSLFlythroughMetrics::ComputeFrameMetrics(i, TestPixels, GTPixels, TW, TH));
		}

		// Write per-run metrics.csv
		if (AllMetrics.Num() != FrameCount || !FSLFlythroughMetrics::WriteMetricsCSV(FPaths::Combine(LatestRun, TEXT("metrics.csv")), AllMetrics))
		{
			UE_LOG(LogSL_Test, Warning, TEXT("Flythrough: %s metrics failed; check PNG dimensions and output permissions."), *Method.Name);
			continue;
		}

		// Build summary
		FSLMethodSummary Summary;
		Summary.Method = Method.Name;
		// Extract run_NNN from the path
		Summary.RunId = FPaths::GetCleanFilename(LatestRun);
		Summary.AvgRenderMs = FSLFlythroughMetrics::ReadAvgRenderTime(FPaths::Combine(LatestRun, TEXT("timings.csv")));

		if (AllMetrics.Num() > 0)
		{
			for (const auto& M : AllMetrics)
			{
				Summary.AvgMSE += M.MSE;
				Summary.AvgPSNR += M.PSNR;
				Summary.AvgSSIM += M.SSIM;
				Summary.AvgCNR += M.CNR;
			}
			Summary.AvgMSE /= AllMetrics.Num();
			Summary.AvgPSNR /= AllMetrics.Num();
			Summary.AvgSSIM /= AllMetrics.Num();
			Summary.AvgCNR /= AllMetrics.Num();
		}

		Summaries.Add(MoveTemp(Summary));
	}

	// Write summary CSV
	if (Summaries.IsEmpty() || !FSLFlythroughMetrics::WriteSummaryCSV(Root, Summaries))
	{
		bComputingMetrics = false;
		MetricsStatus = TEXT("Metrics failed: no matching frames or summary CSV could not be saved.");
		OnStateChanged.Broadcast();
		return false;
	}
	CachedSummaries = Summaries;

	bComputingMetrics = false;
	MetricsStatus = FString::Printf(TEXT("Metrics computed for %d methods against %s."), Summaries.Num(), *ReferenceName);
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Metrics computed for %d methods. Summary at %s"),
		Summaries.Num(), *FPaths::Combine(Root, TEXT("summary_metrics.csv")));

	return true;
}
