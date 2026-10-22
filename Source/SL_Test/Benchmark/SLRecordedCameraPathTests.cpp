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
	Path.Id = TEXT("view-test");
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
	World->DestroyWorld(false);
	return true;
}
#endif
