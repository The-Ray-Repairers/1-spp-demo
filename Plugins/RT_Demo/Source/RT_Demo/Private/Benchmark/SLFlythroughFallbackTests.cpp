#if WITH_DEV_AUTOMATION_TESTS
#include "SLFlythroughSubsystem.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "RenderUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLFlythroughFallbackRoutingTest,
	"SL.Flythrough.SoftwareFallbackRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLFlythroughFallbackRoutingTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Toggle = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Flythrough.AllowSoftwareFallback"));
	if (!TestNotNull(TEXT("Fallback console switch is registered"), Toggle)) return false;
	const int32 PreviousValue = Toggle->GetInt();
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Test world"), World)) return false;
	auto* Flythrough = NewObject<USLFlythroughSubsystem>(World);
	const FString RealRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("renders")));

	Toggle->Set(0, ECVF_SetByConsole);
	TestFalse(TEXT("Disabled switch never selects synthetic mode"), Flythrough->IsUsingSoftwareFallback());
	TestEqual(TEXT("Default results location"), Flythrough->GetRendersRoot(), RealRoot);
	if (!IsRayTracingEnabled())
	{
		FText Reason;
		TestFalse(TEXT("Hardware guard remains enabled by default"), Flythrough->CanStartFlythrough(Reason));
		TestTrue(TEXT("Guard explains the opt-in command"), Reason.ToString().Contains(TEXT("SL.Flythrough.AllowSoftwareFallback 1")));
	}

	Toggle->Set(1, ECVF_SetByConsole);
	TestEqual(TEXT("Opt-in only selects fallback when hardware RT is unavailable"),
		Flythrough->IsUsingSoftwareFallback(), !IsRayTracingEnabled());
	TestEqual(TEXT("Synthetic and real runs use separate roots"), Flythrough->GetRendersRoot(),
		IsRayTracingEnabled() ? RealRoot : FPaths::Combine(RealRoot, TEXT("software_fallback")));
	if (!IsRayTracingEnabled())
	{
		FText Reason;
		TestFalse(TEXT("Fallback still requires a playable viewport"), Flythrough->CanStartFlythrough(Reason));
		TestFalse(TEXT("Hardware guard was bypassed"), Reason.ToString().Contains(TEXT("Hardware ray tracing is unavailable")));
	}
	Toggle->Set(0, ECVF_SetByConsole);
	TestEqual(TEXT("Disabling returns to real results without mixing synthetic data"), Flythrough->GetRendersRoot(), RealRoot);
	Toggle->Set(PreviousValue, ECVF_SetByConsole);
	World->DestroyWorld(false);
	return true;
}
#endif
