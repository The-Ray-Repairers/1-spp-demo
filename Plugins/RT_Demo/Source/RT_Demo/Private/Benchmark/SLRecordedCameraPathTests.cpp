#if WITH_DEV_AUTOMATION_TESTS
#include "SLRecordedCameraPath.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "SLFlythroughSubsystem.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRecordedCameraPathTest, "SL.Flythrough.RecordedPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRecordedCameraPathTest::RunTest(const FString& Parameters)
{
	FSLRecordedCameraSample A, B;
	A.Rotation = FRotator(0, 170, 0).Quaternion();
	B.Rotation = FRotator(0, -170, 0).Quaternion();
	B.Position = FVector(100, 200, 300);
	B.FOV = 110;
	const auto Mid = FSLRecordedCameraPath::Interpolate(A, B, 0.5);
	TestTrue(TEXT("Position uses timestamp interpolation"), Mid.Position.Equals(FVector(50, 100, 150)));
	TestTrue(TEXT("Rotation takes shortest arc across yaw wrap"), FMath::IsNearlyEqual(FMath::Abs(Mid.Rotation.Rotator().Yaw), 180.0, 0.001));
	TestEqual(TEXT("Lens interpolates"), Mid.FOV, 100.f);

	FSLRecordedCameraPath Path;
	Path.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Path.Name = TEXT("Test path");
	Path.Map = TEXT("/Game/ArbitraryScene");
	Path.Frames = {A, Mid, B};
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RecordedPathTest_") + Path.Id + TEXT(".json"));
	TestTrue(TEXT("Save runtime path"), Path.Save(File));
	FSLRecordedCameraPath Loaded;
	TestTrue(TEXT("Load runtime path"), Loaded.Load(File));
	TestEqual(TEXT("Stable ID survives save"), Loaded.Id, Path.Id);
	TestEqual(TEXT("Frame schedule survives save"), Loaded.Frames.Num(), 3);
	if (Loaded.Frames.Num() == 3)
		TestTrue(TEXT("Last position survives save"), Loaded.Frames[2].Position.Equals(B.Position));
	FString Json;
	FFileHelper::LoadFileToString(Json, *File);
	FFileHelper::SaveStringToFile(Json.Replace(TEXT("\"version\": 1"), TEXT("\"version\": 999")), *File);
	TestFalse(TEXT("Reject unknown schema without changing loaded data"), Loaded.Load(File));
	TestEqual(TEXT("Failed load is transactional"), Loaded.Id, Path.Id);
	FFileHelper::SaveStringToFile(TEXT("{\"version\":1,\"frames\":[null]}"), *File);
	TestFalse(TEXT("Reject malformed file"), Loaded.Load(File));
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRecordedPathRoutingTest, "SL.Flythrough.RecordedPathRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRecordedPathRoutingTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* Subsystem = NewObject<USLFlythroughSubsystem>(World);
	FSLRecordedCameraPath A;
	A.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	A.Name = TEXT("Routing fixture");
	A.Map = World->GetOutermost()->GetName();
	A.Frames.SetNum(2);
	FSLRecordedCameraPath B = A;
	B.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	B.Frames[1].Position.X = 100;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BenchmarkPaths"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString FileA = FPaths::Combine(Directory, A.Id + TEXT(".json"));
	const FString FileB = FPaths::Combine(Directory, B.Id + TEXT(".json"));
	TestTrue(TEXT("Save A"), A.Save(FileA));
	TestTrue(TEXT("Save B"), B.Save(FileB));
	Subsystem->RefreshPaths();
	TestTrue(TEXT("Select recording without a scenario actor"), Subsystem->SelectPath(A.Id));
	const FString RootA = Subsystem->GetRendersRoot();
	TestTrue(TEXT("Results use path ID"), RootA.Contains(A.Id));
	TestTrue(TEXT("Select second recording"), Subsystem->SelectPath(B.Id));
	TestTrue(TEXT("Different paths have isolated results"), RootA != Subsystem->GetRendersRoot());
	TestFalse(TEXT("Unknown path cannot change selection"), Subsystem->SelectPath(TEXT("missing")));
	TestTrue(TEXT("Selection preserved after rejection"), Subsystem->GetRendersRoot().Contains(B.Id));
	TestFalse(TEXT("Other path's GT is not available"), Subsystem->HasGTBaseline());
	IFileManager::Get().Delete(*FileB);
	B.Frames[1].Position.X += 1;
	const FString OldRoot = Subsystem->GetRendersRoot();
	TestTrue(TEXT("Save revised content"), B.Save(FileB));
	Subsystem->RefreshPaths();
	TestTrue(TEXT("Content change isolates previous GT and results"), OldRoot != Subsystem->GetRendersRoot());
	IFileManager::Get().Delete(*FileA);
	IFileManager::Get().Delete(*FileB);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRecordedPathViewTest, "SL.Flythrough.RecordedPathRenderedView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRecordedPathViewTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* PC = World->SpawnActor<APlayerController>();
	// This isolated world has no local player/login lifecycle to create its manager.
	if (PC && !PC->PlayerCameraManager)
	{
		PC->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>();
		if (PC->PlayerCameraManager) PC->PlayerCameraManager->InitializeFor(PC);
	}
	if (!TestNotNull(TEXT("Player controller"), PC) || !TestNotNull(TEXT("Camera manager"), PC->PlayerCameraManager.Get()))
	{
		World->DestroyWorld(false);
		return false;
	}
	PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
	// CreateWorld has not run the controller's normal PostInitializeComponents.
	if (!World->GetFirstPlayerController()) World->AddController(PC);
	auto* Subsystem = NewObject<USLFlythroughSubsystem>(World);
	Subsystem->FlythroughCamera = World->SpawnActor<ACameraActor>();
	PC->SetViewTarget(Subsystem->FlythroughCamera);
	PC->PlayerCameraManager->UpdateCamera(0.f);
	const FVector OriginalView = PC->PlayerCameraManager->GetCameraCacheView().Location;
	FSLRecordedCameraPath Path;
	Path.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Path.Name = TEXT("Preview trim fixture");
	Path.Map = World->GetOutermost()->GetName();
	Path.Frames.SetNum(2);
	Path.Frames[0].Position = FVector(100, 200, 300);
	Path.Frames[0].Rotation = FRotator(15, 35, 0).Quaternion();
	Path.Frames[0].FOV = 70;
	Path.Frames[1].Position = FVector(-250, 400, 150);
	Path.Frames[1].Rotation = FRotator(-10, -60, 0).Quaternion();
	Path.Frames[1].FOV = 105;
	Subsystem->RecordedPaths.Add(Path);
	Subsystem->SelectedPathId = Path.Id;
	// No world/controller ticks: the same constraint as paused benchmark capture.
	// Inspect the camera cache and player viewpoint, not merely the camera actor.
	for (int32 Frame : {0, 1, 0})
	{
		TestTrue(TEXT("Evaluate recorded frame"), Subsystem->EvaluateSequenceFrame(Frame));
		const auto& Expected = Path.Frames[Frame];
		const auto& View = PC->PlayerCameraManager->GetCameraCacheView();
		TestTrue(TEXT("Rendered location follows recording"), View.Location.Equals(Expected.Position, 0.001));
		TestTrue(TEXT("Rendered rotation follows recording"), View.Rotation.Quaternion().Equals(Expected.Rotation, 0.001));
		TestEqual(TEXT("Rendered lens follows recording"), View.FOV, Expected.FOV);
		FVector PlayerLocation;
		FRotator PlayerRotation;
		PC->GetPlayerViewPoint(PlayerLocation, PlayerRotation);
		TestTrue(TEXT("Player viewport uses recorded pose"), PlayerLocation.Equals(Expected.Position, 0.001));
		TestFalse(TEXT("View no longer stuck at original pose"), PlayerLocation.Equals(OriginalView));
	}
	// Editing can scrub outside the selected range, but playback honors the range.
	Subsystem->RecordedPaths[0].Frames.Add(Path.Frames[0]);
	Subsystem->ResetTrimRange();
	Subsystem->bPreviewing = true;
	TestTrue(TEXT("Scrub updates preview"), Subsystem->SeekPreviewFrame(1));
	TestTrue(TEXT("Mark start"), Subsystem->SetTrimStartAtPreview());
	TestTrue(TEXT("Scrub endpoint"), Subsystem->SeekPreviewFrame(2));
	TestTrue(TEXT("Mark end"), Subsystem->SetTrimEndAtPreview());
	TestTrue(TEXT("Scrub full source outside selected range"), Subsystem->SeekPreviewFrame(0));
	TestFalse(TEXT("Reject inverted range"), Subsystem->SetTrimEndAtPreview());
	TestEqual(TEXT("Rejected mark preserves end"), Subsystem->GetTrimEndFrame(), 2);
	TestFalse(TEXT("Reject out of bounds seek"), Subsystem->SeekPreviewFrame(3));
	Subsystem->TogglePreviewPlayback();
	TestEqual(TEXT("Playback starts inside selected range"), Subsystem->GetPreviewFrame(), 1);
	Subsystem->PreviewStartTime -= 1.0;
	Subsystem->Tick(0.f);
	TestTrue(TEXT("Preview stays open at end for editing"), Subsystem->IsPreviewing());
	TestFalse(TEXT("Playback pauses at end"), Subsystem->IsPreviewPlaying());
	TestEqual(TEXT("Playback holds selected endpoint"), Subsystem->GetPreviewFrame(), 2);
	TestTrue(TEXT("Held pose reaches camera cache"), PC->PlayerCameraManager->GetCameraCacheView().Location.Equals(Path.Frames[0].Position));
	Subsystem->ResetTrimRange();
	TestEqual(TEXT("Reset restores original start"), Subsystem->GetTrimStartFrame(), 0);
	Subsystem->SeekPreviewFrame(1);
	Subsystem->SetTrimStartAtPreview();
	const FString OriginalRoot = Subsystem->GetRendersRoot();
	const bool bSaved = Subsystem->SaveTrimmedPath();
	TestTrue(TEXT("Save trimmed preview through subsystem"), bSaved);
	if (bSaved)
	{
		TestFalse(TEXT("Saving closes preview"), Subsystem->IsPreviewing());
		TestEqual(TEXT("Saved copy is selected with retained frames"), Subsystem->GetPathFrameCount(), 2);
		TestTrue(TEXT("Saved copy has its own results root"), OriginalRoot != Subsystem->GetRendersRoot());
		TestFalse(TEXT("Saved copy requires a new GT"), Subsystem->HasGTBaseline());
		if (const auto* Saved = Subsystem->GetRecordedPath())
			IFileManager::Get().Delete(*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BenchmarkPaths"), Saved->Id + TEXT(".json")));
	}
	Subsystem->bPreviewing = false;
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRecordedPathTrimTest, "SL.Flythrough.RecordedPathTrim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRecordedPathTrimTest::RunTest(const FString& Parameters)
{
	FSLRecordedCameraPath Source;
	Source.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Source.Name = TEXT("Trim fixture");
	Source.Map = TEXT("/Game/TestScene");
	Source.FPS = 24;
	Source.Frames.SetNum(6);
	for (int32 I = 0; I < 6; ++I)
	{
		Source.Frames[I].Position = FVector(I * 10, I, 50);
		Source.Frames[I].FOV = 60 + I;
	}
	FSLRecordedCameraPath Trimmed;
	TestTrue(TEXT("Trim inclusive range"), Source.MakeTrimmedCopy(2, 4, Trimmed));
	TestEqual(TEXT("Both endpoints included"), Trimmed.Frames.Num(), 3);
	TestEqual(TEXT("Source samples preserved"), Source.Frames.Num(), 6);
	TestTrue(TEXT("Independent path identity"), Trimmed.Id != Source.Id);
	TestEqual(TEXT("FPS preserved"), Trimmed.FPS, 24);
	TestEqual(TEXT("Map preserved"), Trimmed.Map, Source.Map);
	if (Trimmed.Frames.Num() == 3)
	{
		TestTrue(TEXT("Exact start pose"), Trimmed.Frames[0].Position == Source.Frames[2].Position);
		TestTrue(TEXT("Exact end pose"), Trimmed.Frames[2].Position == Source.Frames[4].Position);
		TestEqual(TEXT("Exact lens"), Trimmed.Frames[2].FOV, Source.Frames[4].FOV);
	}
	const FString Id = Trimmed.Id;
	TestFalse(TEXT("Negative start rejected"), Source.MakeTrimmedCopy(-1, 4, Trimmed));
	TestFalse(TEXT("Out of range end rejected"), Source.MakeTrimmedCopy(2, 6, Trimmed));
	TestFalse(TEXT("Single frame rejected"), Source.MakeTrimmedCopy(2, 2, Trimmed));
	TestFalse(TEXT("Reversed range rejected"), Source.MakeTrimmedCopy(4, 2, Trimmed));
	TestEqual(TEXT("Rejected edit leaves output untouched"), Trimmed.Id, Id);
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrimTest_") + Id + TEXT(".json"));
	TestTrue(TEXT("Save trimmed recording"), Trimmed.Save(File));
	FSLRecordedCameraPath Loaded;
	TestTrue(TEXT("Load trimmed recording"), Loaded.Load(File));
	TestEqual(TEXT("Saved trimmed frame count"), Loaded.Frames.Num(), 3);
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRecordedPathDeleteTest, "SL.Flythrough.RecordedPathDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRecordedPathDeleteTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* Subsystem = NewObject<USLFlythroughSubsystem>(World);
	FSLRecordedCameraPath A;
	A.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	A.Name = TEXT("Delete fixture");
	A.Map = World->GetOutermost()->GetName();
	A.Frames.SetNum(2);
	FSLRecordedCameraPath B = A;
	B.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BenchmarkPaths"));
	const FString ImportedName = TEXT("DeleteTest_") + A.Id;
	const FString FileA = FPaths::Combine(Directory, ImportedName + TEXT(".json"));
	const FString FileB = FPaths::Combine(Directory, B.Id + TEXT(".json"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	TestTrue(TEXT("Save import with filename different from ID"), A.Save(FileA));
	TestTrue(TEXT("Save unrelated recording"), B.Save(FileB));
	Subsystem->RefreshPaths();
	TestFalse(TEXT("Cannot delete Level Sequence selection"), Subsystem->DeleteSelectedPath());
	TestTrue(TEXT("Select imported recording"), Subsystem->SelectPath(A.Id));
	for (bool* BusyFlag : {&Subsystem->bRecording, &Subsystem->bPreviewing, &Subsystem->bBatchActive, &Subsystem->bComputingMetrics})
	{
		*BusyFlag = true;
		TestFalse(TEXT("Deletion blocked during active operation"), Subsystem->DeleteSelectedPath());
		*BusyFlag = false;
	}
	Subsystem->Phase = ESLFlythroughPhase::Rendering;
	TestFalse(TEXT("Deletion blocked during capture"), Subsystem->DeleteSelectedPath());
	Subsystem->Phase = ESLFlythroughPhase::Idle;
	const FString Root = Subsystem->GetRendersRoot();
	const FString Sentinel = FPaths::Combine(Root, TEXT("delete_test_sentinel.txt"));
	IFileManager::Get().MakeDirectory(*Root, true);
	TestTrue(TEXT("Create result sentinel"), FFileHelper::SaveStringToFile(TEXT("keep"), *Sentinel));
	TestTrue(TEXT("Delete selected import"), Subsystem->DeleteSelectedPath());
	TestFalse(TEXT("Original file removed"), IFileManager::Get().FileExists(*FileA));
	TestTrue(TEXT("Other recording retained"), IFileManager::Get().FileExists(*FileB));
	TestTrue(TEXT("Results retained"), IFileManager::Get().FileExists(*Sentinel));
	TestFalse(TEXT("Selection reset"), Subsystem->HasRecordedPath());
	TestFalse(TEXT("Deleted path unavailable"), Subsystem->SelectPath(A.Id));
	TArray<FString> Archived;
	const FString DeletedDirectory = FPaths::Combine(Directory, TEXT("Deleted"));
	IFileManager::Get().FindFiles(Archived, *FPaths::Combine(DeletedDirectory, ImportedName + TEXT("_*.json")), true, false);
	TestEqual(TEXT("One recoverable recording"), Archived.Num(), 1);
	if (Archived.Num() == 1)
	{
		const FString Archive = FPaths::Combine(DeletedDirectory, Archived[0]);
		FSLRecordedCameraPath Recovered;
		TestTrue(TEXT("Archive retains valid recording"), Recovered.Load(Archive));
		TestEqual(TEXT("Archive retains ID"), Recovered.Id, A.Id);
		TestTrue(TEXT("Restore recording"), IFileManager::Get().Move(*FileA, *Archive, false, false));
		Subsystem->RefreshPaths();
		TestTrue(TEXT("Restored recording selectable"), Subsystem->SelectPath(A.Id));
		TestEqual(TEXT("Restored recording reuses results root"), Subsystem->GetRendersRoot(), Root);
	}
	Subsystem->SelectPath(B.Id);
	IFileManager::Get().Delete(*FileB);
	TestFalse(TEXT("Missing file deletion fails safely"), Subsystem->DeleteSelectedPath());
	TestTrue(TEXT("Failed deletion preserves selection"), Subsystem->HasRecordedPath());
	IFileManager::Get().Delete(*FileA);
	IFileManager::Get().Delete(*Sentinel);
	IFileManager::Get().DeleteDirectory(*Root, false, false);
	World->DestroyWorld(false);
	return true;
}
#endif
