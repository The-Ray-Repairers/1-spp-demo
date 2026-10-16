#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "SLFlythroughCameraPath.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLFlythroughRoomPathTest,
	"SL.Flythrough.RoomCameraPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLFlythroughRoomPathTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Standalone room selects authored route"), SLFlythroughCameraPath::IsRoomMap(TEXT("LVL_RT_Room")));
	TestTrue(TEXT("PIE room selects authored route"), SLFlythroughCameraPath::IsRoomMap(TEXT("UEDPIE_0_LVL_RT_Room")));
	TestFalse(TEXT("Other maps retain their existing orbit"), SLFlythroughCameraPath::IsRoomMap(TEXT("LVL_Other")));

	FAutomationEditorCommonUtils::LoadMap(TEXT("/Game/Levels/LVL_RT_Room"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("Room loaded"), World)) return false;
	if (!TestEqual(TEXT("Validating the actual room asset"), World->GetMapName(), FString(TEXT("LVL_RT_Room")))) return false;

	TArray<FBox> Obstacles;
	TArray<FVector> Subjects;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		const FBox Bounds = It->GetComponentsBoundingBox(true);
		Obstacles.Add(Bounds.ExpandBy(50.0));
		if (It->GetActorLabel() == TEXT("Sphere") || It->GetActorLabel() == TEXT("Cylinder"))
			Subjects.Add(Bounds.GetCenter());
	}
	TestTrue(TEXT("Room geometry was loaded"), Obstacles.Num() >= 8);
	TestEqual(TEXT("Both benchmark subjects exist"), Subjects.Num(), 2);

	// Check more densely than the 300 captured frames, with a 50 cm safety
	// envelope against the real level geometry rather than duplicated bounds.
	for (int32 Sample = 0; Sample <= 900; ++Sample)
	{
		const FTransform Camera = SLFlythroughCameraPath::EvaluateRoom(Sample / 900.0);
		const FVector Position = Camera.GetLocation();
		for (const FBox& Obstacle : Obstacles)
		{
			if (Obstacle.IsInsideOrOn(Position))
			{
				AddError(FString::Printf(TEXT("Camera enters geometry safety envelope at sample %d: %s"), Sample, *Position.ToString()));
				return false;
			}
		}
		for (const FVector& Subject : Subjects)
		{
			const FVector Local = Camera.InverseTransformPosition(Subject);
			const double HalfWidth = Local.X * FMath::Tan(FMath::DegreesToRadians(SLFlythroughCameraPath::RoomFieldOfView * 0.5));
			if (Local.X <= 0.0 || FMath::Abs(Local.Y) >= HalfWidth || FMath::Abs(Local.Z) >= HalfWidth / (16.0 / 9.0))
			{
				AddError(FString::Printf(TEXT("Benchmark subject leaves the 16:9 camera view at sample %d"), Sample));
				return false;
			}
		}
	}
	TestTrue(TEXT("Loop closes without a camera jump"),
		SLFlythroughCameraPath::EvaluateRoom(0.0).Equals(SLFlythroughCameraPath::EvaluateRoom(1.0), 0.001));
	return true;
}
#endif
