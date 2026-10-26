#include "SLFlythroughSubsystem.h"
#include "SLFlythroughScenario.h"
#include "SLComparisonView.h"
#include "SLComparisonSubsystem.h"
#include "SLPathTracerSubsystem.h"
#include "SL_Test.h"
#include "DLSSLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequenceID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
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
#include "Misc/PackageName.h"
#include "Misc/Crc.h"
#include "Async/Async.h"
#include "SceneViewExtension.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "UnrealClient.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "Editor/EditorPerformanceSettings.h"
#endif

DECLARE_CYCLE_STAT(TEXT("SLFlythroughSubsystem"), STAT_SLFlythrough, STATGROUP_Tickables);

static FString GetBenchmarkMapPackage(const UWorld* World)
{
	FString MapPackage = World->GetOutermost()->GetName();
	FString MapLeaf = FPackageName::GetShortName(MapPackage);
	if (MapLeaf.StartsWith(TEXT("UEDPIE_")))
	{
		int32 Separator = INDEX_NONE;
		if (MapLeaf.FindChar(TEXT('_'), Separator))
		{
			Separator = MapLeaf.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Separator + 1);
			if (Separator != INDEX_NONE) MapLeaf = MapLeaf.Mid(Separator + 1);
		}
	}
	return FPaths::Combine(FPaths::GetPath(MapPackage), MapLeaf);
}

/** Run folder names can be reused after deletion; the capture id ties a summary to one GT capture. */
static FString GetSummaryGTKey(const FString& GTDir, const FSLFlythroughRunManifest& GTManifest)
{
	return FPaths::GetCleanFilename(GTDir) + TEXT("|") + GTManifest.CaptureId;
}

static FString GetScenarioFolder(const UWorld* World, const ASLFlythroughScenario* Scenario)
{
	return FString::Printf(TEXT("%s_%08x"), *Scenario->ScenarioId.ToString(),
		FCrc::StrCrc32(*GetBenchmarkMapPackage(World)));
}

static TAutoConsoleVariable<int32> CVarSLFlythroughAllowSoftwareFallback(
	TEXT("SL.Flythrough.AllowSoftwareFallback"), 0,
	TEXT("Allow synthetic flythrough pipeline tests when hardware ray tracing is unavailable.\n")
	TEXT("0: require hardware ray tracing (default). 1: allow the existing noise fallback.\n")
	TEXT("Synthetic PNGs/metrics go to renders/software_fallback and are not ray-traced benchmarks.\n")
	TEXT("Changes during capture apply after the current run finishes."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
	{
		if (!GEngine) return;
		for (const auto& Context : GEngine->GetWorldContexts())
			if (UWorld* World = Context.World())
				if (auto* Flythrough = World->GetSubsystem<USLFlythroughSubsystem>(); Flythrough && !Flythrough->IsBusy())
				{
					Flythrough->OnFallbackModeChanged();
				}
	}), ECVF_Default);

static TAutoConsoleVariable<int32> CVarSLFlythroughGTSPP(
	TEXT("SL.Flythrough.GTSPP"), 64,
	TEXT("Samples per pixel accumulated for the flythrough ground truth (default 64).\n")
	TEXT("The GT method and its results folder are named <N>SPP_GT. Changes during capture apply to the next run."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
	{
		if (!GEngine) return;
		for (const auto& Context : GEngine->GetWorldContexts())
			if (UWorld* World = Context.World())
				if (auto* Flythrough = World->GetSubsystem<USLFlythroughSubsystem>(); Flythrough && !Flythrough->IsBusy())
				{
					Flythrough->ApplyGTSPP();
					Flythrough->RefreshSummaries();
					Flythrough->OnStateChanged.Broadcast();
				}
	}), ECVF_Default);

static FAutoConsoleCommandWithWorldAndArgs CmdSLFlythroughRunAll(
	TEXT("SL.Flythrough.RunAll"),
	TEXT("Capture the GT (SL.Flythrough.GTSPP) and every flythrough method for this level's scenario, then compute metrics.\n")
	TEXT("Pass 'quit' to exit when finished, e.g. -ExecCmds=\"SL.Flythrough.RunAll quit\"."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (auto* Flythrough = World ? World->GetSubsystem<USLFlythroughSubsystem>() : nullptr)
			Flythrough->StartBatch(Args.Contains(TEXT("quit")));
	}));

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
	RefreshPaths();
}

void USLFlythroughSubsystem::Deinitialize()
{
	if (bPreviewing) StopPathPreview();
	if (bRecording) DiscardRecording();
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

	// 1SPP_RayRepair
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_RayRepair");
		M.SPP = 1;
		M.DenoiserMethod = 5;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// 1SPP_NBDirect
	{
		FSLFlythroughMethod M;
		M.Name = TEXT("1SPP_NBDirect");
		M.SPP = 1;
		M.DenoiserMethod = 6;
		M.bAccumulate = false;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("1")});
		Methods.Add(MoveTemp(M));
	}

	// <N>SPP_GT — ground truth baseline; N comes from SL.Flythrough.GTSPP
	{
		FSLFlythroughMethod M;
		M.DenoiserMethod = 0; // no denoising on GT
		M.bAccumulate = true;
		M.bIsGT = true;
		M.CVarOverrides.Add({TEXT("SL.PathTracer.ResetEveryFrame"), TEXT("0")});
		Methods.Add(MoveTemp(M));
	}
	ApplyGTSPP();
}

void USLFlythroughSubsystem::ApplyGTSPP()
{
	if (IsBusy()) return;
	const int32 SPP = FMath::Clamp(CVarSLFlythroughGTSPP.GetValueOnGameThread(), 1, 65536);
	for (FSLFlythroughMethod& Method : Methods)
		if (Method.bIsGT)
		{
			// Each sample count gets its own results folder, so GTs at different SPP never mix.
			Method.SPP = SPP;
			Method.Name = FString::Printf(TEXT("%dSPP_GT"), SPP);
		}
}

FString USLFlythroughSubsystem::GetGTMethodName() const
{
	const FSLFlythroughMethod* GT = Methods.FindByPredicate([](const FSLFlythroughMethod& M) { return M.bIsGT; });
	return GT ? GT->Name : FString();
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
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("renders")));
	if ((IsBusy() || Phase == ESLFlythroughPhase::Finished) && !CurrentRunRoot.IsEmpty()) return CurrentRunRoot;
	const FString ModeRoot = IsUsingSoftwareFallback() ? FPaths::Combine(Root, TEXT("software_fallback")) : Root;
	ASLFlythroughScenario* Scenario = nullptr;
	FText Reason;
	return ResolveScenario(Scenario, Reason) ? FPaths::Combine(ModeRoot, GetPathFolder()) : ModeRoot;
}

bool USLFlythroughSubsystem::IsUsingSoftwareFallback() const
{
	// Pin the mode for a running capture even if the console switch changes.
	return (IsBusy() || Phase == ESLFlythroughPhase::Finished) && !CurrentRunRoot.IsEmpty() ? bSoftwareFallbackRun
		: !IsRayTracingEnabled() && CVarSLFlythroughAllowSoftwareFallback.GetValueOnGameThread() != 0;
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
	return HasCompletedRun(GetGTMethodName());
}

// -----------------------------------------------------------------------
// Summary Cache
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::RefreshSummaries()
{
	ASLFlythroughScenario* Scenario = nullptr;
	FText Reason;
	if (!ResolveScenario(Scenario, Reason))
	{
		CachedSummaries.Reset();
		CompletedMethods.Reset();
		return;
	}
	RefreshSummariesForRoot(GetRendersRoot());
}

void USLFlythroughSubsystem::OnFallbackModeChanged()
{
	if (IsBusy()) return;
	CurrentRunRoot.Reset();
	RefreshSummaries();
	OnStateChanged.Broadcast();
}

void USLFlythroughSubsystem::RefreshSummariesForRoot(const FString& RendersRoot)
{
	FSLFlythroughMetrics::ReadSummaryCSV(RendersRoot, CachedSummaries);
	const FString GTDir = FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(RendersRoot, GetGTMethodName()));
	FString ScoredGT;
	FSLFlythroughRunManifest GTManifest;
	if (GTDir.IsEmpty() || !FSLFlythroughMetrics::ReadRunManifest(GTDir, GTManifest)
		|| !FFileHelper::LoadFileToString(ScoredGT, *FPaths::Combine(RendersRoot, TEXT("summary_gt_run.txt")))
		|| ScoredGT.TrimStartAndEnd() != GetSummaryGTKey(GTDir, GTManifest))
	{
		CachedSummaries.Reset();
	}
	else
	{
		CachedSummaries.RemoveAll([&](const FSLMethodSummary& Summary)
		{
			const FString LatestRun = FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(RendersRoot, Summary.Method));
			FSLFlythroughRunManifest RunManifest;
			return LatestRun.IsEmpty() || FPaths::GetCleanFilename(LatestRun) != Summary.RunId
				|| !FSLFlythroughMetrics::ReadRunManifest(LatestRun, RunManifest)
				|| !RunManifest.IsComparableTo(GTManifest);
		});
	}
	// Slate polls run colors every paint; validate frame files only on refresh.
	CompletedMethods.Reset();
	for (const FSLFlythroughMethod& Method : Methods)
	{
		if (!FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(RendersRoot, Method.Name)).IsEmpty())
			CompletedMethods.Add(Method.Name);
	}
}

// -----------------------------------------------------------------------
// Scenario discovery and camera-only sequence validation
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::ResolveScenario(ASLFlythroughScenario*& OutScenario, FText& Reason) const
{
	OutScenario = nullptr;
	if (GetRecordedPath()) return true;
	for (TActorIterator<ASLFlythroughScenario> It(GetWorld()); It; ++It)
	{
		if (OutScenario)
		{
			Reason = FText::FromString(TEXT("Place exactly one Flythrough Scenario actor in this level."));
			return false;
		}
		OutScenario = *It;
	}
	if (!OutScenario || !OutScenario->Sequence || !OutScenario->Camera)
	{
		Reason = FText::FromString(TEXT("Place a Flythrough Scenario actor and assign its Sequence and Camera."));
		return false;
	}
	const FString Id = OutScenario->ScenarioId.ToString();
	bool bInvalidId = OutScenario->ScenarioId.IsNone();
	for (const TCHAR C : Id)
		bInvalidId |= !FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-');
	if (bInvalidId)
	{
		Reason = FText::FromString(TEXT("Scenario Id must use only letters, numbers, '_' or '-'."));
		return false;
	}
	UMovieScene* Scene = OutScenario->Sequence->GetMovieScene();
	if (!Scene || Scene->GetSpawnableCount() != 0 || Scene->GetPlaybackRange().IsEmpty()
		|| !Scene->GetPlaybackRange().HasLowerBound() || !Scene->GetPlaybackRange().HasUpperBound()
		|| Scene->GetDisplayRate().Denominator != 1 || Scene->GetDisplayRate().Numerator <= 0)
	{
		Reason = FText::FromString(TEXT("Sequence needs a nonempty range, integer display rate, and no spawnables."));
		return false;
	}
	const FMovieSceneObjectBindingIDs* Tagged = Scene->AllTaggedBindings().Find(OutScenario->CameraBindingTag);
	if (OutScenario->CameraBindingTag.IsNone() || !Tagged || Tagged->IDs.Num() != 1)
	{
		Reason = FText::FromString(TEXT("Tag exactly one camera actor binding with the configured Camera Binding Tag."));
		return false;
	}
	const FGuid CameraGuid = Tagged->IDs[0].GetGuid();
	if (OutScenario->Camera->GetWorld() != GetWorld())
	{
		Reason = FText::FromString(TEXT("The benchmark camera must belong to this level."));
		return false;
	}
	bool bHasCamera = false;
	for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = Scene->GetPossessable(Index);
		if (Possessable.GetGuid() == CameraGuid && !Possessable.GetParent().IsValid())
		{
#if WITH_EDITORONLY_DATA
			const UClass* BindingClass = Possessable.GetPossessedObjectClass();
			if (BindingClass && !BindingClass->IsChildOf(ACameraActor::StaticClass()))
			{
				Reason = FText::FromString(TEXT("The tagged binding must be a CameraActor."));
				return false;
			}
#endif
			bHasCamera = true;
		}
		else if (Possessable.GetParent() != CameraGuid)
		{
			Reason = FText::FromString(TEXT("Sequence may bind only the benchmark camera and its components."));
			return false;
		}
		else
		{
#if WITH_EDITORONLY_DATA
			const UClass* BindingClass = Possessable.GetPossessedObjectClass();
			if (BindingClass && !BindingClass->IsChildOf(UCameraComponent::StaticClass()))
			{
				Reason = FText::FromString(TEXT("Sequence may bind only the benchmark camera and its camera component."));
				return false;
			}
#endif
		}
	}
	if (!bHasCamera)
	{
		Reason = FText::FromString(TEXT("The tagged binding must be the root camera actor."));
		return false;
	}
	for (const UMovieSceneTrack* Track : Scene->GetTracks())
		if (!Track->IsA<UMovieSceneCameraCutTrack>())
		{
			Reason = FText::FromString(TEXT("Sequence cannot contain event, audio, or subsequence tracks."));
			return false;
		}
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(Scene)->GetBindings())
		for (const UMovieSceneTrack* Track : Binding.GetTracks())
			if (!Track->IsA<UMovieScene3DTransformTrack>() && !Track->IsA<UMovieScenePropertyTrack>())
			{
				Reason = FText::FromString(TEXT("Camera bindings may contain only transform and property tracks."));
				return false;
			}
	return true;
}

bool USLFlythroughSubsystem::ValidateSequenceBindings(FText& Reason)
{
	// Possessable classes are editor-only data; check the objects actually bound in every build.
	UMovieScene* Scene = ActiveScenario->Sequence->GetMovieScene();
	UCameraComponent* CameraComponent = FlythroughCamera->GetCameraComponent();
	for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = Scene->GetPossessable(Index);
		const bool bRoot = !Possessable.GetParent().IsValid();
		const TArray<UObject*> Bound = SequencePlayer->GetBoundObjects(
			UE::MovieScene::FFixedObjectBindingID(Possessable.GetGuid(), MovieSceneSequenceID::Root));
		for (const UObject* Object : Bound)
		{
			const bool bAllowed = bRoot ? Object == FlythroughCamera
				: Cast<UCameraComponent>(Object) && Cast<UCameraComponent>(Object)->GetOwner() == FlythroughCamera;
			if (!bAllowed)
			{
				Reason = FText::FromString(FString::Printf(TEXT("Sequence binding '%s' resolves to %s, not the benchmark camera."),
					*Possessable.GetName(), *GetNameSafe(Object)));
				return false;
			}
		}
		if (bRoot && Bound.Num() != 1)
		{
			Reason = FText::FromString(TEXT("The tagged camera binding did not resolve to the scenario camera."));
			return false;
		}
	}
	return CameraComponent != nullptr;
}

bool USLFlythroughSubsystem::EvaluateSequenceFrame(int32 FrameIndex)
{
	if (const auto* Path = GetRecordedPath())
	{
		if (!FlythroughCamera || !Path->Frames.IsValidIndex(FrameIndex)) return false;
		const auto& Frame = Path->Frames[FrameIndex];
		FlythroughCamera->SetActorLocationAndRotation(Frame.Position, Frame.Rotation);
		auto* Camera = FlythroughCamera->GetCameraComponent();
		Camera->FieldOfView = Frame.FOV;
		Camera->AspectRatio = Frame.AspectRatio;
		Camera->bConstrainAspectRatio = Frame.bConstrainAspectRatio;
	}
	else
	{
		const FFrameTime Position(SequenceStartFrame + FrameIndex);
		// Jump evaluates synchronously unless queued during another sequence evaluation.
		SequencePlayer->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(Position, EUpdatePositionMethod::Jump));
		if (SequencePlayer->GetCurrentTime().Time != Position)
		{
			UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Sequence did not evaluate display frame %d."), Position.FrameNumber.Value);
			return false;
		}
	}
	// Camera property tracks may write post-process settings; keep the method's SPP override pinned.
	auto& PP = FlythroughCamera->GetCameraComponent()->PostProcessSettings;
	PP.bOverride_PathTracingSamplesPerPixel = true;
	PP.PathTracingSamplesPerPixel = TargetSPP;
	// Gameplay is paused during capture. Moving the actor alone leaves the rendered
	// view cached at the player's last pose. Both path sources must publish each
	// evaluated pose/lens to the camera manager before capture or preview.
	GetWorld()->GetFirstPlayerController()->PlayerCameraManager->UpdateCamera(0.f);
	return true;
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
	UKismetRenderingLibrary::EnablePathTracing(!bSoftwareFallbackRun);
	if (bSoftwareFallbackRun)
	{
		const auto* PathTracer = GetWorld()->GetSubsystem<USLPathTracerSubsystem>();
		// Reuse the normal viewport fallback; do not inject noise twice.
		// Also support SL.PathTracer.AutoEnable=0, which skips its startup view.
		if (!PathTracer || !PathTracer->HasNoiseFallbackView())
			SoftwareFallbackView = FSceneViewExtensions::NewExtension<FSLNoiseFallbackViewExtension>(GetWorld(), true);
	}

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
	// Keep full speed when the window loses focus so long captures can run in the background.
	SetCVar(TEXT("t.IdleWhenNotForeground"), TEXT("0"));
#if WITH_EDITOR
	if (UEditorPerformanceSettings* Performance = GetMutableDefault<UEditorPerformanceSettings>())
	{
		bSavedEditorThrottle = Performance->bThrottleCPUWhenNotForeground;
		Performance->bThrottleCPUWhenNotForeground = false;
	}
#endif

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
#if WITH_EDITOR
	if (UEditorPerformanceSettings* Performance = GetMutableDefault<UEditorPerformanceSettings>())
		Performance->bThrottleCPUWhenNotForeground = bSavedEditorThrottle;
#endif
	FApp::SetFixedDeltaTime(SavedFixedDeltaTime);
	FApp::SetUseFixedTimeStep(bSavedUseFixedDeltaTime);
	UKismetRenderingLibrary::EnablePathTracing(bSavedPathTracing);
	UDLSSLibrary::EnableDLSS(bSavedDLSS);
	UDLSSLibrary::EnableDLSSRR(bSavedRR);
}

// -----------------------------------------------------------------------
// Camera Setup / Teardown
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::SetupCamera()
{
	auto* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager || (!ActiveScenario && !GetRecordedPath())) return false;

	bSpawnedPlaybackCamera = GetRecordedPath() != nullptr;
	FlythroughCamera = bSpawnedPlaybackCamera ? GetWorld()->SpawnActor<ACameraActor>() : ActiveScenario->Camera.Get();
	if (!FlythroughCamera) return false;
	const UCameraComponent* CameraComponent = FlythroughCamera->GetCameraComponent();
	SavedCameraPostProcessSettings = CameraComponent->PostProcessSettings;
	SavedCameraActorTransform = FlythroughCamera->GetActorTransform();
	SavedCameraComponentTransform = CameraComponent->GetRelativeTransform();
	SavedCameraFieldOfView = CameraComponent->FieldOfView;
	SavedCameraAspectRatio = CameraComponent->AspectRatio;
	SavedCameraOrthoWidth = CameraComponent->OrthoWidth;
	bSavedCameraConstrainAspectRatio = CameraComponent->bConstrainAspectRatio;
	PreviousViewTarget = PC->GetViewTarget();
	bWasPaused = PC->IsPaused();
	if (!bSpawnedPlaybackCamera)
	{
		FMovieSceneSequencePlaybackSettings Settings;
		Settings.bDisableCameraCuts = true;
		// Restore KeepState sections too. Play() normally enables this capture, but the benchmark only jumps.
		Settings.FinishCompletionStateOverride = EMovieSceneCompletionModeOverride::ForceRestoreState;
		ALevelSequenceActor* CreatedActor = nullptr;
		SequencePlayer = ULevelSequencePlayer::CreateLevelSequencePlayer(GetWorld(), ActiveScenario->Sequence, Settings, CreatedActor);
		SequenceActor = CreatedActor;
		if (!SequencePlayer || !SequenceActor)
		{
			MetricsStatus = TEXT("Unable to create a player for the scenario Sequence.");
			return false;
		}
		SequencePlayer->GetEvaluationTemplate().EnableGlobalPreAnimatedStateCapture();
		TArray<AActor*> CameraBinding{FlythroughCamera};
		SequenceActor->SetBindingByTag(ActiveScenario->CameraBindingTag, CameraBinding, false);
	}
	PC->SetViewTarget(FlythroughCamera);
	PC->SetPause(true);
	FText BindingReason;
	if (!EvaluateSequenceFrame(0))
	{
		MetricsStatus = TEXT("The scenario Sequence did not evaluate its first frame.");
		return false;
	}
	if (!bSpawnedPlaybackCamera && !ValidateSequenceBindings(BindingReason))
	{
		MetricsStatus = BindingReason.ToString();
		return false;
	}

	SourcePostProcessSettings.Reset();
	FPostProcessSettings ComparablePostProcess = FlythroughCamera->GetCameraComponent()->PostProcessSettings;
	ComparablePostProcess.bOverride_PathTracingSamplesPerPixel = false;
	ComparablePostProcess.PathTracingSamplesPerPixel = 0;
	FPostProcessSettings::StaticStruct()->ExportText(SourcePostProcessSettings,
		&ComparablePostProcess, nullptr, nullptr, PPF_None, nullptr);
	auto& PP = FlythroughCamera->GetCameraComponent()->PostProcessSettings;
	PP.bOverride_PathTracingSamplesPerPixel = true;
	PP.PathTracingSamplesPerPixel = TargetSPP;
	return true;
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
		if (SequencePlayer) SequencePlayer->RestoreState();
		UCameraComponent* CameraComponent = FlythroughCamera->GetCameraComponent();
		FlythroughCamera->SetActorTransform(SavedCameraActorTransform);
		CameraComponent->SetRelativeTransform(SavedCameraComponentTransform);
		CameraComponent->FieldOfView = SavedCameraFieldOfView;
		CameraComponent->AspectRatio = SavedCameraAspectRatio;
		CameraComponent->OrthoWidth = SavedCameraOrthoWidth;
		CameraComponent->bConstrainAspectRatio = bSavedCameraConstrainAspectRatio;
		CameraComponent->PostProcessSettings = SavedCameraPostProcessSettings;
		if (bSpawnedPlaybackCamera) FlythroughCamera->Destroy();
		bSpawnedPlaybackCamera = false;
		FlythroughCamera = nullptr;
		PreviousViewTarget = nullptr;
	}
	if (SequenceActor) SequenceActor->Destroy();
	SequenceActor = nullptr;
	SequencePlayer = nullptr;
	ActiveScenario = nullptr;

	if (AccumulationView)
	{
		FlushRenderingCommands();
		AccumulationView.Reset();
		FlushRenderingCommands();
	}
	if (SoftwareFallbackView)
	{
		FlushRenderingCommands();
		SoftwareFallbackView.Reset();
		FlushRenderingCommands();
	}
}

// -----------------------------------------------------------------------
// Flythrough Control
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::CanStartFlythrough(FText& Reason) const
{
	if (bPreviewing) { Reason = FText::FromString(TEXT("Stop the camera preview first.")); return false; }
	if (bRecording) { Reason = FText::FromString(TEXT("Stop recording before capturing.")); return false; }
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
	if (!IsRayTracingEnabled() && CVarSLFlythroughAllowSoftwareFallback.GetValueOnGameThread() == 0)
	{
		Reason = FText::FromString(TEXT("Hardware ray tracing is unavailable. For synthetic pipeline tests, enter SL.Flythrough.AllowSoftwareFallback 1."));
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
	ASLFlythroughScenario* Scenario = nullptr;
	if (!ResolveScenario(Scenario, Reason)) return false;
	return true;
}

static const TCHAR* PartialTimingsFile = TEXT("timings_partial.csv");

/** Same scene, camera, settings and method; only then may frames from two sessions share a run. */
static bool IsSameCapture(const FSLFlythroughRunManifest& A, const FSLFlythroughRunManifest& B)
{
	return A.IsComparableTo(B) && A.Method == B.Method && A.SPP == B.SPP && A.DenoiserMethod == B.DenoiserMethod;
}

bool USLFlythroughSubsystem::ChooseRunDirectory(const FString& MethodDir, FSLFlythroughRunManifest& Manifest, bool& bOutAlreadyComplete)
{
	bOutAlreadyComplete = false;
	TArray<FString> RunNames;
	IFileManager::Get().FindFiles(RunNames, *FPaths::Combine(MethodDir, TEXT("run_*")), false, true);
	RunNames.Sort([](const FString& A, const FString& B) { return FCString::Atoi(*A.Mid(4)) > FCString::Atoi(*B.Mid(4)); });
	const FString NewestRun = RunNames.IsEmpty() ? FString() : FPaths::Combine(MethodDir, RunNames[0]);
	FSLFlythroughRunManifest Existing;
	const bool bNewestMatches = !NewestRun.IsEmpty() && FSLFlythroughMetrics::ReadRunManifest(NewestRun, Existing)
		&& IsSameCapture(Existing, Manifest);

	if (bNewestMatches && FSLFlythroughMetrics::IsRunComplete(NewestRun))
	{
		// A batch re-run keeps finished work; an explicit Render click always captures anew.
		if (bBatchActive)
		{
			CurrentRunDir = NewestRun;
			bOutAlreadyComplete = true;
			return true;
		}
	}
	else if (bNewestMatches)
	{
		// Resume an interrupted capture: keep every leading frame that has both its PNG and timing.
		TMap<int32, double> Timings;
		FString Partial;
		if (FFileHelper::LoadFileToString(Partial, *FPaths::Combine(NewestRun, PartialTimingsFile)))
		{
			TArray<FString> Lines;
			Partial.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Index, Ms;
				if (Line.Split(TEXT(","), &Index, &Ms) && Index.IsNumeric())
					Timings.Add(FCString::Atoi(*Index), FCString::Atod(*Ms));
			}
		}
		int32 ResumeFrame = 0;
		while (ResumeFrame < TotalFrames && Timings.Contains(ResumeFrame)
			&& IFileManager::Get().FileExists(*FPaths::Combine(NewestRun, TEXT("frames"),
				FString::Printf(TEXT("frame_%04d.png"), ResumeFrame))))
		{
			FSLFrameTiming Timing;
			Timing.FrameIndex = ResumeFrame;
			Timing.RenderTimeMs = Timings[ResumeFrame];
			FrameTimings.Add(Timing);
			++ResumeFrame;
		}
		CurrentRunDir = NewestRun;
		CurrentFrame = ResumeFrame;
		Manifest.CaptureId = Existing.CaptureId;
		UE_LOG(LogSL_Test, Display, TEXT("Flythrough: resuming %s at frame %d/%d."), *NewestRun, ResumeFrame, TotalFrames);
		return true;
	}

	CurrentRunDir = FPaths::Combine(MethodDir, FSLFlythroughMetrics::GetNextRunId(MethodDir));
	if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(CurrentRunDir, TEXT("frames")), true))
	{
		MetricsStatus = TEXT("Unable to create the flythrough output directory.");
		return false;
	}
	if (bSoftwareFallbackRun)
	{
		const FString Notice = TEXT("SYNTHETIC SOFTWARE FALLBACK - PIPELINE TEST ONLY\n")
			TEXT("Viewport raster rendering plus simulated noise; not hardware path tracing.\n")
			TEXT("The GT uses 1/sqrt(SPP) noise scaling, not accumulated rays.\n")
			TEXT("Quality metrics and timings are not valid ray-traced benchmark results.\n");
		if (!FFileHelper::SaveStringToFile(Notice, *FPaths::Combine(CurrentRunDir, TEXT("capture_mode.txt")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			MetricsStatus = TEXT("Unable to write the synthetic capture notice.");
			return false;
		}
		UE_LOG(LogSL_Test, Display, TEXT("Flythrough: SYNTHETIC software fallback test. Output: %s"), *CurrentRunDir);
	}
	// Persist the capture identity before any frame is written. Incomplete runs are ignored for scoring.
	Manifest.CaptureId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!FSLFlythroughMetrics::WriteRunManifest(CurrentRunDir, Manifest))
	{
		MetricsStatus = TEXT("Unable to write run_manifest.json; capture was not started.");
		return false;
	}
	return true;
}

void USLFlythroughSubsystem::RestartCurrentFrame()
{
	// Render time includes the whole frame, so a frame that stalled while minimized starts over.
	bFrameInterrupted = false;
	if (FScreenshotRequest::IsScreenshotRequested())
		FScreenshotRequest::Reset();
	if (!EvaluateSequenceFrame(CurrentFrame))
	{
		CancelFlythrough();
		MetricsStatus = FString::Printf(TEXT("Capture stopped: Sequence frame %d could not be evaluated."), CurrentFrame);
		return;
	}
	if (AccumulationView)
	{
		AccumulationView->Samples.Store(0);
		AccumulationView->bResetAccumulation.Store(true);
	}
	Phase = ESLFlythroughPhase::WarmUp;
	FrameStartTime = FPlatformTime::Seconds();
	UE_LOG(LogSL_Test, Display, TEXT("Flythrough: window restored; re-rendering frame %d."), CurrentFrame);
}

bool USLFlythroughSubsystem::StartFlythrough()
{
	ApplyGTSPP();
	FText Reason;
	if (!CanStartFlythrough(Reason))
	{
		UE_LOG(LogSL_Test, Warning, TEXT("Flythrough: Cannot start — %s"), *Reason.ToString());
		return false;
	}

	const FSLFlythroughMethod& Method = *GetSelectedMethod();
	FText ScenarioReason;
	ASLFlythroughScenario* Scenario = nullptr;
	if (!ResolveScenario(Scenario, ScenarioReason)) return false;
	ActiveScenario = Scenario;
	if (const auto* Path = GetRecordedPath())
	{
		TargetFPS = Path->FPS;
		SequenceStartFrame = 0;
		TotalFrames = Path->Frames.Num();
	}
	else
	{
		const UMovieScene* MovieScene = ActiveScenario->Sequence->GetMovieScene();
		TargetFPS = MovieScene->GetDisplayRate().Numerator;
		const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
		SequenceStartFrame = FFrameRate::TransformTime(FFrameTime(PlaybackRange.GetLowerBoundValue()),
			MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).CeilToFrame().Value;
		const int32 EndFrame = FFrameRate::TransformTime(FFrameTime(PlaybackRange.GetUpperBoundValue()),
			MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).CeilToFrame().Value;
		TotalFrames = EndFrame - SequenceStartFrame;
	}
	if (TotalFrames <= 0)
	{
		ActiveScenario = nullptr;
		MetricsStatus = TEXT("Sequence playback range has no display frames.");
		return false;
	}
	FixedDeltaTime = 1.0 / static_cast<double>(TargetFPS);
	FlythroughDuration = TotalFrames * FixedDeltaTime;
	CurrentMethodName = Method.Name;
	MetricsStatus.Reset();
	bSoftwareFallbackRun = !IsRayTracingEnabled() && CVarSLFlythroughAllowSoftwareFallback.GetValueOnGameThread() != 0;
	const FString DefaultRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("renders")));
	const FString ModeRoot = bSoftwareFallbackRun ? FPaths::Combine(DefaultRoot, TEXT("software_fallback")) : DefaultRoot;
	CurrentRunRoot = FPaths::Combine(ModeRoot, GetPathFolder());
	// Release comparison overlays, view extensions and renderer overrides first.
	if (auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>(); Comparison && Comparison->OwnsRenderer())
		Comparison->Restore();

	// Setup frames. The output directory is chosen once the capture identity is known.
	CurrentFrame = 0;
	bFrameInterrupted = false;
	FrameTimings.Reset();
	FrameTimings.Reserve(TotalFrames);
	TargetSPP = Method.SPP;

	// Setup viewport size tracking
	CaptureSize = GetWorld()->GetGameViewport()->Viewport->GetSizeXY();

	// Setup camera
	if (!SetupCamera())
	{
		if (MetricsStatus.IsEmpty()) MetricsStatus = TEXT("Unable to set up the scenario camera.");
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: %s"), *MetricsStatus);
		ReleaseCamera();
		CurrentRunRoot.Reset();
		Phase = ESLFlythroughPhase::Idle;
		OnStateChanged.Broadcast();
		return false;
	}

	// Apply method settings
	ApplyMethodSettings(Method);

	// Persist the capture identity before any frame is written. Incomplete runs are ignored.
	FSLFlythroughRunManifest Manifest;
	Manifest.Method = Method.Name;
	Manifest.SPP = Method.SPP;
	Manifest.DenoiserMethod = Method.DenoiserMethod;
	Manifest.Map = GetBenchmarkMapPackage(GetWorld());
	if (const auto* Path = GetRecordedPath())
	{
		Manifest.CameraPath = TEXT("recorded:") + Path->Id;
		Manifest.CameraParameters = TEXT("recorded_v1;playback=2;perspective;default_camera_postprocess");
	}
	else
	{
		Manifest.CameraPath = ActiveScenario->Sequence->GetPathName();
		Manifest.CameraParameters = FString::Printf(TEXT("scenario=%s;camera_tag=%s;start_frame=%d"),
			*ActiveScenario->ScenarioId.ToString(), *ActiveScenario->CameraBindingTag.ToString(), SequenceStartFrame);
	}
	// Fingerprint every sampled camera state so route and FOV edits invalidate older GT runs.
	FString CameraSamples;
	CameraSamples.Reserve(TotalFrames * 160);
	bool bSampledCamera = true;
	for (int32 FrameIndex = 0; FrameIndex < TotalFrames && bSampledCamera; ++FrameIndex)
	{
		bSampledCamera = EvaluateSequenceFrame(FrameIndex);
		const UCameraComponent* CameraComponent = FlythroughCamera->GetCameraComponent();
		FPostProcessSettings ComparablePostProcess = CameraComponent->PostProcessSettings;
		ComparablePostProcess.bOverride_PathTracingSamplesPerPixel = false;
		ComparablePostProcess.PathTracingSamplesPerPixel = 0;
		FString PostProcessText;
		FPostProcessSettings::StaticStruct()->ExportText(PostProcessText,
			&ComparablePostProcess, nullptr, nullptr, PPF_None, nullptr);
		// The component's world transform includes both actor and camera-component transform tracks.
		CameraSamples += FString::Printf(TEXT("%s;fov=%.9g;aspect=%.9g;constrain=%d;projection=%d;ortho=%.9g;pp=%08x;"),
			*CameraComponent->GetComponentTransform().ToString(), CameraComponent->FieldOfView,
			CameraComponent->AspectRatio, CameraComponent->bConstrainAspectRatio ? 1 : 0,
			static_cast<int32>(CameraComponent->ProjectionMode), CameraComponent->OrthoWidth,
			FCrc::StrCrc32(*PostProcessText));
	}
	Manifest.CameraParameters += FString::Printf(TEXT(";sample_crc=%08x"), FCrc::StrCrc32(*CameraSamples));
	bSampledCamera = bSampledCamera && EvaluateSequenceFrame(0);
	Manifest.Width = CaptureSize.X;
	Manifest.Height = CaptureSize.Y;
	Manifest.FrameCount = TotalFrames;
	Manifest.TargetFPS = TargetFPS;
	Manifest.DurationSeconds = FlythroughDuration;
	Manifest.bSoftwareFallback = bSoftwareFallbackRun;
	// Snapshot renderer setting families instead of maintaining a list of individual CVars.
	// The method-specific sample count and reset mode are intentionally excluded.
	TArray<FString> SharedSettings;
	const auto CollectSetting = FConsoleObjectVisitor::CreateLambda([&](const TCHAR* Name, IConsoleObject* Object)
	{
		const FString Key(Name);
		if (Key == TEXT("r.PathTracing.SamplesPerPixel")
			|| Key == TEXT("SL.PathTracer.TargetSPP")
			|| Key == TEXT("SL.PathTracer.ResetEveryFrame")) return;
		if (const IConsoleVariable* Variable = Object->AsVariable())
			SharedSettings.Add(Key + TEXT("=") + Variable->GetString());
	});
	static const TCHAR* SettingFamilies[] = {
		TEXT("r.PathTracing."), TEXT("r.Tonemapper."), TEXT("r.EyeAdaptation."),
		TEXT("r.DefaultFeature."), TEXT("r.ScreenPercentage"), TEXT("SL.PathTracer.")
	};
	for (const TCHAR* Prefix : SettingFamilies)
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(CollectSetting, Prefix);
	SharedSettings.Sort();
	Manifest.RenderSettings = FString::Printf(TEXT("path_tracing=%d;exposure_bias=%.6f"),
		bSoftwareFallbackRun ? 0 : 1,
		FlythroughCamera->GetCameraComponent()->PostProcessSettings.AutoExposureBias);
	Manifest.RenderSettings += FString::Printf(TEXT(";source_postprocess_crc=%08x"),
		FCrc::StrCrc32(*SourcePostProcessSettings));
	for (const FString& Setting : SharedSettings)
		Manifest.RenderSettings += TEXT(";") + Setting;
	bool bAlreadyComplete = false;
	if (!bSampledCamera || !ChooseRunDirectory(FPaths::Combine(CurrentRunRoot, CurrentMethodName), Manifest, bAlreadyComplete))
	{
		ReleaseCamera();
		RestoreSettings();
		if (!bSampledCamera)
			MetricsStatus = TEXT("The scenario Sequence could not be evaluated at every frame; capture was not started.");
		CurrentRunRoot.Reset();
		Phase = ESLFlythroughPhase::Idle;
		OnStateChanged.Broadcast();
		return false;
	}
	if (bAlreadyComplete)
	{
		// Batch re-runs skip methods whose identical capture already finished.
		ReleaseCamera();
		RestoreSettings();
		Phase = ESLFlythroughPhase::Finished;
		MetricsStatus = FString::Printf(TEXT("%s is already captured in %s; skipped."), *CurrentMethodName, *CurrentRunDir);
		UE_LOG(LogSL_Test, Display, TEXT("Flythrough: %s"), *MetricsStatus);
		RefreshSummariesForRoot(CurrentRunRoot);
		OnStateChanged.Broadcast();
		return true;
	}
	if (CurrentFrame > 0 && !EvaluateSequenceFrame(CurrentFrame))
	{
		ReleaseCamera();
		RestoreSettings();
		MetricsStatus = TEXT("The scenario Sequence could not be evaluated at the resume frame.");
		CurrentRunRoot.Reset();
		Phase = ESLFlythroughPhase::Idle;
		OnStateChanged.Broadcast();
		return false;
	}

	// For multi-SPP accumulation, create a view extension to track samples
	if (Method.bAccumulate && !bSoftwareFallbackRun)
	{
		AccumulationView = FSceneViewExtensions::NewExtension<FSLComparisonView>(GetWorld());
		AccumulationView->bReference = true;
		AccumulationView->DenoiserOverride = -1;
		AccumulationView->Samples.Store(0);
		AccumulationView->bResetAccumulation.Store(true);
	}

	// Register screenshot delegate
	ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(
		this, &USLFlythroughSubsystem::OnScreenshotCaptured);

	// The camera is already evaluated at the first frame to capture.
	// Warm up for one frame to let settings apply
	Phase = ESLFlythroughPhase::WarmUp;
	FrameStartTime = FPlatformTime::Seconds();
	MetricsStatus.Reset();
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Display, TEXT("Flythrough: %s %s at frame %d/%d, %d FPS, SPP=%d. Output: %s"),
		CurrentFrame > 0 ? TEXT("Resumed") : TEXT("Started"), *CurrentMethodName, CurrentFrame, TotalFrames,
		TargetFPS, TargetSPP, *CurrentRunDir);

	return true;
}

void USLFlythroughSubsystem::CancelFlythrough()
{
	if (bRecording) { DiscardRecording(); return; }
	if (bPreviewing) { StopPathPreview(); return; }
	if (!IsBusy()) return;

	UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle);
	ScreenshotHandle.Reset();

	if (FScreenshotRequest::IsScreenshotRequested())
		FScreenshotRequest::Reset();

	ReleaseCamera();
	RestoreSettings();

	Phase = ESLFlythroughPhase::Idle;
	MetricsStatus = TEXT("Flythrough cancelled.");
	RefreshSummariesForRoot(CurrentRunRoot);
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Cancelled at frame %d/%d"), CurrentFrame, TotalFrames);
}

// -----------------------------------------------------------------------
// Batch capture (console / command line)
// -----------------------------------------------------------------------

bool USLFlythroughSubsystem::StartBatch(bool bQuitWhenDone)
{
	if (IsBusy() || bBatchActive)
	{
		UE_LOG(LogSL_Test, Warning, TEXT("Flythrough batch: a capture is already running."));
		return false;
	}
	// Reject immediately when hardware ray tracing is unavailable and software fallback is not opted in.
	// CanStartFlythrough repeats this check per-method during TickBatch, but failing here avoids a
	// 30-second startup wait and prevents null-pointer crashes that can occur while ticking before
	// the viewport is fully ready.
	if (!IsRayTracingEnabled() && CVarSLFlythroughAllowSoftwareFallback.GetValueOnGameThread() == 0)
	{
		const FString Msg =
			TEXT("Flythrough batch: hardware ray tracing was not detected on this machine. ")
			TEXT("Enable software fallback with \"SL.Flythrough.AllowSoftwareFallback 1\" to run synthetic ")
			TEXT("pipeline tests, or use a machine with a hardware ray-tracing capable GPU. ")
			TEXT("Note: software fallback results go to renders/software_fallback/ and are not valid ray-traced benchmarks.");
		UE_LOG(LogSL_Test, Error, TEXT("%s"), *Msg);
		if (bQuitWhenDone)
			FPlatformMisc::RequestExitWithStatus(false, 1);
		return false;
	}
	// GT first so a failed baseline stops the batch before the test methods run.
	ApplyGTSPP();
	BatchQueue.Reset();
	const int32 GTIndex = Methods.IndexOfByPredicate([](const FSLFlythroughMethod& M) { return M.bIsGT; });
	if (GTIndex != INDEX_NONE) BatchQueue.Add(GTIndex);
	for (int32 Index = 0; Index < Methods.Num(); ++Index)
		if (Index != GTIndex) BatchQueue.Add(Index);
	bBatchActive = true;
	bBatchQuitWhenDone = bQuitWhenDone;
	bBatchRunInFlight = false;
	BatchWaitStart = FPlatformTime::Seconds();
	UE_LOG(LogSL_Test, Display, TEXT("Flythrough batch: queued %d methods."), BatchQueue.Num());
	return true;
}

void USLFlythroughSubsystem::TickBatch()
{
	if (bBatchRunInFlight)
	{
		bBatchRunInFlight = false;
		if (Phase != ESLFlythroughPhase::Finished)
		{
			EndBatch(false, MetricsStatus.IsEmpty() ? TEXT("Capture was cancelled.") : MetricsStatus);
			return;
		}
		BatchWaitStart = FPlatformTime::Seconds();
	}
	if (BatchQueue.IsEmpty())
	{
		const bool bScored = ComputeMetrics();
		EndBatch(bScored, MetricsStatus);
		return;
	}
	SelectMethod(BatchQueue[0]);
	FText Reason;
	if (!CanStartFlythrough(Reason))
	{
		// The player controller and viewport may not exist yet right after startup.
		if (FPlatformTime::Seconds() - BatchWaitStart > 30.0)
			EndBatch(false, Reason.ToString());
		return;
	}
	if (!StartFlythrough())
	{
		EndBatch(false, MetricsStatus.IsEmpty() ? TEXT("Unable to start the flythrough.") : MetricsStatus);
		return;
	}
	UE_LOG(LogSL_Test, Display, TEXT("Flythrough batch: capturing %s (%d left after this)."),
		*CurrentMethodName, BatchQueue.Num() - 1);
	BatchQueue.RemoveAt(0);
	bBatchRunInFlight = true;
}

void USLFlythroughSubsystem::EndBatch(bool bSucceeded, const FString& Message)
{
	bBatchActive = false;
	BatchQueue.Reset();
	if (bSucceeded)
	{
		UE_LOG(LogSL_Test, Display, TEXT("Flythrough batch: done. %s"), *Message);
		for (const FSLMethodSummary& S : CachedSummaries)
			UE_LOG(LogSL_Test, Display, TEXT("Flythrough result: %s %s render=%.2fms PSNR=%.3f SSIM=%.4f MSE=%.3f CNR=%.3f"),
				*S.Method, *S.RunId, S.AvgRenderMs, S.AvgPSNR, S.AvgSSIM, S.AvgMSE, S.AvgCNR);
	}
	else
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough batch: failed. %s"), *Message);
	}
	// Notify the UI that the batch is no longer active (bBatchActive is now false).
	OnStateChanged.Broadcast();
	if (bBatchQuitWhenDone)
	{
		// RequestExitWithStatus sets GIsRequestingExit, which can cause Slate to begin tearing
		// down objects while we are still inside the current Tick call chain. Any pending Slate
		// repaint that was invalidated by the OnStateChanged broadcast above may then read a
		// partially-destroyed object and crash. Defer to a new game-thread task so the request
		// is processed after the current tick (and all Slate repaints it triggered) fully unwinds.
		const int32 ExitCode = bSucceeded ? 0 : 1;
		AsyncTask(ENamedThreads::GameThread, [ExitCode]()
		{
			FPlatformMisc::RequestExitWithStatus(false, ExitCode);
		});
	}
}

// -----------------------------------------------------------------------
// Tick
// -----------------------------------------------------------------------

void USLFlythroughSubsystem::Tick(float DeltaTime)
{
	if (bRecording) { TickRecording(); return; }
	if (bPreviewing)
	{
		if (bPreviewPlaying)
		{
			const int32 Frame = PreviewAnchorFrame + int32((FPlatformTime::Seconds() - PreviewStartTime) * GetPathFPS());
			PreviewFrame = FMath::Min(Frame, TrimEndFrame);
			if (!EvaluateSequenceFrame(PreviewFrame)) StopPathPreview();
			else if (Frame >= TrimEndFrame) bPreviewPlaying = false;
		}
		return;
	}
	if (IsBusy())
	{
		// A minimized game draws no viewport, so no samples or screenshots arrive. Wait without
		// counting toward the timeout, then redo the frame so its render time stays valid.
		if (GEngine && GEngine->AreAllWindowsHidden())
		{
			bFrameInterrupted = true;
			return;
		}
		if (bFrameInterrupted)
		{
			RestartCurrentFrame();
			return;
		}
		auto* Viewport = GetWorld()->GetGameViewport();
		if (!Viewport || !Viewport->Viewport || Viewport->Viewport->GetSizeXY() != CaptureSize
			|| (Phase != ESLFlythroughPhase::WarmUp && FPlatformTime::Seconds() - FrameStartTime > 600.0))
		{
			CancelFlythrough();
			MetricsStatus = TEXT("Capture stopped: viewport changed or frame timed out.");
			return;
		}
	}
	else if (bBatchActive)
	{
		TickBatch();
		return;
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

	if (Method.bAccumulate && !bSoftwareFallbackRun)
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
		// Software fallback emulates SPP through noise scaling; it has no PT sample counter.
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
	if (Phase != ESLFlythroughPhase::AwaitingScreenshot || bFrameInterrupted) return;
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
	// Written after the PNG so a resumed run never trusts a timing without its frame.
	if (!FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d,%.3f\n"), CurrentFrame, LastFrameTimeMs),
		*FPaths::Combine(CurrentRunDir, PartialTimingsFile), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append))
	{
		UE_LOG(LogSL_Test, Error, TEXT("Flythrough: Failed to record the timing for frame %d"), CurrentFrame);
		CancelFlythrough();
		return;
	}

	AdvanceFrame();
}

void USLFlythroughSubsystem::AdvanceFrame()
{
	++CurrentFrame;

	if (CurrentFrame >= TotalFrames)
	{
		FinishFlythrough();
		return;
	}

	// Move camera to next position
	if (!EvaluateSequenceFrame(CurrentFrame))
	{
		CancelFlythrough();
		MetricsStatus = FString::Printf(TEXT("Capture stopped: Sequence frame %d could not be evaluated."), CurrentFrame);
		return;
	}

	// Reset accumulation for multi-SPP. The reference view is an offline render, which
	// ignores camera motion, so the next view must restart accumulation explicitly.
	if (AccumulationView)
	{
		AccumulationView->Samples.Store(0);
		AccumulationView->bResetAccumulation.Store(true);
	}

	// Continue rendering
	Phase = ESLFlythroughPhase::WarmUp;

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
	IFileManager::Get().Delete(*FPaths::Combine(CurrentRunDir, PartialTimingsFile));

	// Cleanup
	UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle);
	ScreenshotHandle.Reset();
	ReleaseCamera();
	RestoreSettings();

	Phase = ESLFlythroughPhase::Finished;
	MetricsStatus = FString::Printf(TEXT("Flythrough complete: %s — %d frames captured."),
		*CurrentMethodName, TotalFrames);

	RefreshSummariesForRoot(CurrentRunRoot);
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: %s complete. Output: %s"), *CurrentMethodName, *CurrentRunDir);
}

// -----------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------

FText USLFlythroughSubsystem::GetStatusText() const
{
	if (bPreviewing) return FText::FromString(MetricsStatus.IsEmpty() ? TEXT("Scrub to a pose, then set the start and end. Save Trimmed Copy to benchmark that range.") : MetricsStatus);
	if (bRecording && !MetricsStatus.IsEmpty()) return FText::FromString(MetricsStatus);
	if (bRecording) return FText::FromString(FString::Printf(TEXT("Recording %s: %.1f s. Close GM to move; reopen to stop and save."),
		*Recording.Name, Recording.Frames.Num() / 30.0));
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
	if (bPreviewing) { Reason = FText::FromString(TEXT("Stop the camera preview first.")); return false; }
	if (bRecording) { Reason = FText::FromString(TEXT("Stop recording before computing metrics.")); return false; }
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
	ASLFlythroughScenario* Scenario = nullptr;
	if (!ResolveScenario(Scenario, Reason)) return false;
	if (!HasGTBaseline())
	{
		Reason = FText::FromString(FString::Printf(TEXT("Render a %s baseline first."), *GetGTMethodName()));
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
	MetricsProgress = 0.f;
	MetricsStatus = TEXT("Computing metrics...");
	OnStateChanged.Broadcast();

	const FString Root = GetRendersRoot();
	const FString GTDir = FSLFlythroughMetrics::FindLatestRun(FPaths::Combine(Root, GetGTMethodName()));
	FSLFlythroughRunManifest GTManifest;
	if (GTDir.IsEmpty() || !FSLFlythroughMetrics::ReadRunManifest(GTDir, GTManifest))
	{
		MetricsStatus = FString::Printf(TEXT("The GT run has no valid capture manifest. Render a new %s baseline."), *GetGTMethodName());
		bComputingMetrics = false;
		OnStateChanged.Broadcast();
		return false;
	}

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
	TArray<FString> MismatchedMethods;

	const int32 TotalMethodCount = FMath::Max(1, Methods.Num());
	int32 MethodIndex = 0;
	for (const FSLFlythroughMethod& Method : Methods)
	{
		// Update and broadcast progress at the start of each method slot so the
		// panel status text and progress bar refresh even though this loop is synchronous.
		MetricsProgress = (float)MethodIndex / (float)TotalMethodCount;
		++MethodIndex;
		OnStateChanged.Broadcast();

		const FString MethodDir = FPaths::Combine(Root, Method.Name);
		const FString LatestRun = FSLFlythroughMetrics::FindLatestRun(MethodDir);
		if (LatestRun.IsEmpty() || !FSLFlythroughMetrics::IsRunComplete(LatestRun))
			continue;
		FSLFlythroughRunManifest TestManifest;
		if (!FSLFlythroughMetrics::ReadRunManifest(LatestRun, TestManifest)
			|| !TestManifest.IsComparableTo(GTManifest))
		{
			UE_LOG(LogSL_Test, Warning, TEXT("Flythrough: %s capture configuration does not match GT; skipping %s."),
				*Method.Name, *LatestRun);
			MismatchedMethods.Add(Method.Name);
			continue;
		}

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
	const FString MismatchNote = MismatchedMethods.IsEmpty() ? FString()
		: FString::Printf(TEXT(" Skipped (capture differs from GT; re-render): %s."), *FString::Join(MismatchedMethods, TEXT(", ")));
	if (Summaries.IsEmpty() || !FSLFlythroughMetrics::WriteSummaryCSV(Root, Summaries)
		|| !FFileHelper::SaveStringToFile(GetSummaryGTKey(GTDir, GTManifest),
			*FPaths::Combine(Root, TEXT("summary_gt_run.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		bComputingMetrics = false;
		MetricsStatus = TEXT("Metrics failed: no matching frames or summary CSV could not be saved.") + MismatchNote;
		OnStateChanged.Broadcast();
		return false;
	}
	CachedSummaries = Summaries;

	bComputingMetrics = false;
	MetricsProgress = 1.f;
	MetricsStatus = FString::Printf(TEXT("Metrics computed for %d methods."), Summaries.Num()) + MismatchNote;
	OnStateChanged.Broadcast();

	UE_LOG(LogSL_Test, Log, TEXT("Flythrough: Metrics computed for %d methods. Summary at %s"),
		Summaries.Num(), *FPaths::Combine(Root, TEXT("summary_metrics.csv")));

	return true;
}

// Recorded paths are user data: no editor APIs or project-specific pawn dependencies.
static FString RecordedPathsDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BenchmarkPaths"));
}

const FSLRecordedCameraPath* USLFlythroughSubsystem::GetRecordedPath() const
{
	return RecordedPaths.FindByPredicate([this](const FSLRecordedCameraPath& P) { return P.Id == SelectedPathId; });
}

FString USLFlythroughSubsystem::GetSelectedPathName() const
{
	const auto* Path = GetRecordedPath();
	return Path ? Path->Name : TEXT("Level Sequence (scene default)");
}

FString USLFlythroughSubsystem::GetPathFolder() const
{
	if (const auto* Path = GetRecordedPath())
	{
		// Earlier playback did not refresh the rendered camera cache. Keep those
		// captures out of completion detection, resume, metrics, and batch skipping.
		FString Samples = TEXT("playback=2;") + FString::FromInt(Path->FPS);
		for (const auto& F : Path->Frames)
			Samples += FString::Printf(TEXT(";%.17g;%.17g;%.17g;%.17g;%.17g;%.17g;%.17g;%.9g;%.9g;%d"),
				F.Position.X, F.Position.Y, F.Position.Z, F.Rotation.X, F.Rotation.Y, F.Rotation.Z, F.Rotation.W,
				F.FOV, F.AspectRatio, F.bConstrainAspectRatio);
		return FString::Printf(TEXT("recorded_%08x/%s_%08x"), FCrc::StrCrc32(*Path->Map), *Path->Id, FCrc::StrCrc32(*Samples));
	}
	ASLFlythroughScenario* Scenario = nullptr;
	FText Reason;
	return ResolveScenario(Scenario, Reason) && Scenario ? GetScenarioFolder(GetWorld(), Scenario) : FString();
}

void USLFlythroughSubsystem::RefreshPaths()
{
	if (!CanChangePath()) return;
	RecordedPaths.Reset();
	RecordedPathFiles.Reset();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(RecordedPathsDirectory(), TEXT("*.json")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		FSLRecordedCameraPath Path;
		if (Path.Load(FPaths::Combine(RecordedPathsDirectory(), File)) && Path.Map == GetBenchmarkMapPackage(GetWorld())
			&& !RecordedPaths.ContainsByPredicate([&Path](const FSLRecordedCameraPath& P) { return P.Id == Path.Id; }))
		{
			RecordedPathFiles.Add(Path.Id, FPaths::ConvertRelativePathToFull(FPaths::Combine(RecordedPathsDirectory(), File)));
			RecordedPaths.Add(MoveTemp(Path));
		}
	}
	if (!GetRecordedPath()) SelectedPathId.Reset();
	ResetTrimRange();
	CurrentRunRoot.Reset();
	Phase = ESLFlythroughPhase::Idle;
	RefreshSummaries();
	OnStateChanged.Broadcast();
}

bool USLFlythroughSubsystem::SelectPath(const FString& Id)
{
	if (!CanChangePath()) return false;
	if (!Id.IsEmpty() && !RecordedPaths.ContainsByPredicate([&Id](const FSLRecordedCameraPath& P) { return P.Id == Id; })) return false;
	SelectedPathId = Id;
	ResetTrimRange();
	CurrentRunRoot.Reset();
	Phase = ESLFlythroughPhase::Idle;
	MetricsStatus.Reset();
	RefreshSummaries();
	OnStateChanged.Broadcast();
	return true;
}

static bool ReadRecordingCamera(UWorld* World, FSLRecordedCameraSample& Sample)
{
	const auto* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager || PC->IsPaused()) return false;
	const FMinimalViewInfo& View = PC->PlayerCameraManager->GetCameraCacheView();
	// Version 1 records perspective lenses; unsupported projections must never be silently changed.
	if (View.ProjectionMode != ECameraProjectionMode::Perspective || View.Location.ContainsNaN()
		|| View.Rotation.ContainsNaN() || !FMath::IsFinite(View.FOV) || View.FOV <= 1 || View.FOV >= 179
		|| !FMath::IsFinite(View.AspectRatio) || View.AspectRatio < 0.1f || View.AspectRatio > 10.f) return false;
	Sample.Position = View.Location;
	Sample.Rotation = View.Rotation.Quaternion();
	Sample.FOV = View.FOV;
	Sample.AspectRatio = View.AspectRatio;
	Sample.bConstrainAspectRatio = View.bConstrainAspectRatio;
	return true;
}

bool USLFlythroughSubsystem::StartRecording(const FString& Name)
{
	if (!CanChangePath()) return false;
	if (const auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>(); Comparison && Comparison->IsBusy()) return false;
	FSLRecordedCameraSample First;
	if (!ReadRecordingCamera(GetWorld(), First))
	{
		MetricsStatus = TEXT("Recording needs an unpaused game and a perspective camera.");
		return false;
	}
	MetricsStatus.Reset();
	CurrentRunRoot.Reset();
	Recording = FSLRecordedCameraPath();
	Recording.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Recording.Name = Name.TrimStartAndEnd().Left(128);
	if (Recording.Name.IsEmpty()) Recording.Name = TEXT("Camera path");
	Recording.Map = GetBenchmarkMapPackage(GetWorld());
	Recording.Frames.Add(First);
	PreviousRecordedSample = First;
	PreviousRecordedTime = 0;
	RecordingStartTime = FPlatformTime::Seconds();
	bRecording = true;
	Phase = ESLFlythroughPhase::Idle;
	OnStateChanged.Broadcast();
	return true;
}

void USLFlythroughSubsystem::TickRecording()
{
	FSLRecordedCameraSample Current;
	if (!ReadRecordingCamera(GetWorld(), Current))
	{
		DiscardRecording();
		MetricsStatus = TEXT("Recording discarded: camera became unavailable, unsupported, or game paused.");
		return;
	}
	const double Now = FPlatformTime::Seconds() - RecordingStartTime;
	// Interpolate between observed views, never simulate pawn input during benchmark replay.
	const bool bWasBelowLimit = Recording.Frames.Num() < 18000;
	while (Recording.Frames.Num() < 18000 && double(Recording.Frames.Num()) / Recording.FPS <= Now)
	{
		const double Time = double(Recording.Frames.Num()) / Recording.FPS;
		const double Alpha = FMath::Clamp((Time - PreviousRecordedTime) / FMath::Max(Now - PreviousRecordedTime, 1.e-9), 0.0, 1.0);
		Recording.Frames.Add(FSLRecordedCameraPath::Interpolate(PreviousRecordedSample, Current, Alpha));
	}
	PreviousRecordedSample = Current;
	PreviousRecordedTime = Now;
	if (bWasBelowLimit && Recording.Frames.Num() == 18000) StopRecording();
}

bool USLFlythroughSubsystem::StopRecording()
{
	if (!bRecording) return false;
	if (Recording.Frames.Num() < 2)
	{
		MetricsStatus = TEXT("Record at least two frames before saving.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*RecordedPathsDirectory(), true);
	if (!Recording.Save(FPaths::Combine(RecordedPathsDirectory(), Recording.Id + TEXT(".json"))))
	{
		MetricsStatus = TEXT("Could not save camera path. Check Saved/BenchmarkPaths is writable and retry Stop and Save.");
		return false;
	}
	bRecording = false;
	const FString Id = Recording.Id;
	Recording = FSLRecordedCameraPath();
	RefreshPaths();
	SelectPath(Id);
	MetricsStatus = TEXT("Camera path saved and selected. Render GT, then the test methods.");
	return true;
}

void USLFlythroughSubsystem::DiscardRecording()
{
	bRecording = false;
	Recording = FSLRecordedCameraPath();
	MetricsStatus = TEXT("Recording discarded.");
	OnStateChanged.Broadcast();
}

bool USLFlythroughSubsystem::StartPathPreview()
{
	if (!CanChangePath() || !GetRecordedPath()) return false;
	if (const auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>(); Comparison && Comparison->IsBusy()) return false;
	if (!SetupCamera()) { ReleaseCamera(); return false; }
	bPreviewing = true;
	MetricsStatus.Reset();
	PreviewFrame = TrimStartFrame;
	bPreviewPlaying = false;
	if (!SeekPreviewFrame(PreviewFrame)) { StopPathPreview(); return false; }
	OnStateChanged.Broadcast();
	return true;
}

void USLFlythroughSubsystem::StopPathPreview()
{
	if (!bPreviewing) return;
	ReleaseCamera();
	bPreviewing = false;
	bPreviewPlaying = false;
	MetricsStatus = TEXT("Camera preview finished.");
	OnStateChanged.Broadcast();
}

int32 USLFlythroughSubsystem::GetPathFrameCount() const
{
	const auto* Path = GetRecordedPath();
	return Path ? Path->Frames.Num() : 0;
}

int32 USLFlythroughSubsystem::GetPathFPS() const
{
	const auto* Path = GetRecordedPath();
	return Path ? Path->FPS : 30;
}

bool USLFlythroughSubsystem::SeekPreviewFrame(int32 Frame)
{
	if (!bPreviewing || Frame < 0 || Frame >= GetPathFrameCount()) return false;
	bPreviewPlaying = false;
	PreviewFrame = Frame;
	return EvaluateSequenceFrame(Frame);
}

void USLFlythroughSubsystem::TogglePreviewPlayback()
{
	if (!bPreviewing) return;
	if (bPreviewPlaying) { bPreviewPlaying = false; return; }
	if (PreviewFrame < TrimStartFrame || PreviewFrame >= TrimEndFrame) PreviewFrame = TrimStartFrame;
	if (!EvaluateSequenceFrame(PreviewFrame)) { StopPathPreview(); return; }
	PreviewAnchorFrame = PreviewFrame;
	PreviewStartTime = FPlatformTime::Seconds();
	bPreviewPlaying = true;
}

bool USLFlythroughSubsystem::SetTrimStartAtPreview()
{
	if (!bPreviewing || PreviewFrame >= TrimEndFrame) return false;
	bPreviewPlaying = false;
	TrimStartFrame = PreviewFrame;
	return true;
}

bool USLFlythroughSubsystem::SetTrimEndAtPreview()
{
	if (!bPreviewing || PreviewFrame <= TrimStartFrame) return false;
	bPreviewPlaying = false;
	TrimEndFrame = PreviewFrame;
	return true;
}

void USLFlythroughSubsystem::ResetTrimRange()
{
	if (!CanChangePath() && !bPreviewing) return;
	bPreviewPlaying = false;
	TrimStartFrame = 0;
	TrimEndFrame = FMath::Max(0, GetPathFrameCount() - 1);
	if (!bPreviewing) PreviewFrame = 0;
}

bool USLFlythroughSubsystem::SaveTrimmedPath()
{
	if (!bPreviewing) return false;
	bPreviewPlaying = false;
	const auto* Path = GetRecordedPath();
	FSLRecordedCameraPath Trimmed;
	if (!Path || !Path->MakeTrimmedCopy(TrimStartFrame, TrimEndFrame, Trimmed)) return false;
	IFileManager::Get().MakeDirectory(*RecordedPathsDirectory(), true);
	if (!Trimmed.Save(FPaths::Combine(RecordedPathsDirectory(), Trimmed.Id + TEXT(".json"))))
	{
		MetricsStatus = TEXT("Could not save trimmed path. Check Saved/BenchmarkPaths is writable and retry.");
		return false;
	}
	StopPathPreview();
	RefreshPaths();
	SelectPath(Trimmed.Id);
	MetricsStatus = TEXT("Trimmed copy saved and selected. Render GT and methods for this path.");
	OnStateChanged.Broadcast();
	return true;
}

bool USLFlythroughSubsystem::DeleteSelectedPath()
{
	if (!CanChangePath() || !GetRecordedPath()) return false;
	const FString* Source = RecordedPathFiles.Find(SelectedPathId);
	const FString Directory = FPaths::ConvertRelativePathToFull(RecordedPathsDirectory());
	FSLRecordedCameraPath OnDisk;
	// Only act on the discovered file, and reject files replaced since the last refresh.
	if (!Source || !FPaths::IsUnderDirectory(*Source, Directory)
		|| !OnDisk.Load(*Source) || OnDisk.Id != SelectedPathId || OnDisk.Map != GetBenchmarkMapPackage(GetWorld()))
	{
		MetricsStatus = TEXT("The recording file is missing or changed. Refresh Camera Paths before deleting.");
		return false;
	}
	const FString DeletedDirectory = FPaths::Combine(Directory, TEXT("Deleted"));
	const FString Destination = FPaths::Combine(DeletedDirectory,
		FPaths::GetBaseFilename(*Source) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
	if (!IFileManager::Get().MakeDirectory(*DeletedDirectory, true)
		|| !IFileManager::Get().Move(*Destination, **Source, false, false))
	{
		MetricsStatus = TEXT("Could not delete the recording. Check folder permissions and retry.");
		return false;
	}
	const FString Name = GetSelectedPathName();
	SelectedPathId.Reset();
	RefreshPaths();
	MetricsStatus = FString::Printf(TEXT("Deleted '%s'. Recording moved to Saved/BenchmarkPaths/Deleted; rendered results retained."), *Name);
	OnStateChanged.Broadcast();
	return true;
}
