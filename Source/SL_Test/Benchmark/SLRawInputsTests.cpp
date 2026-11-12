#if WITH_DEV_AUTOMATION_TESTS
#include "SLRawInputsSubsystem.h"
#include "SLDenoiser.h"
#include "Debug/GMTools/SLGMToolConfig.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRawInputsTest, "SL.Diagnostics.RawInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRawInputsTest::RunTest(const FString& Parameters)
{
    for (const TCHAR* Name : {TEXT("raw-inputs"), TEXT("SL.Flythrough.Methods"), TEXT("SL.Flythrough.Render"), TEXT("SL.Flythrough.RunAll")})
        TestNotNull(FString::Printf(TEXT("Command %s registered"), Name), IConsoleManager::Get().FindConsoleObject(Name));
    const FSLDenoiserNeuralPlaceholder Grid(0);
    TestEqual(TEXT("Capture always requests the complete grid"), Grid.DebugModeOverride, 0);
    auto* Config = NewObject<USLGMToolConfig>();
    Config->StandardizeRenderingMethods();
    TestNull(TEXT("Diagnostic is not a rendering method"), Config->FindProfile(TEXT("raw-inputs")));
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    auto* Capture = NewObject<USLRawInputsSubsystem>(World);
    const int32 Previous = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.Method"))->GetInt();
    TestFalse(TEXT("Capture rejects missing game viewport"), Capture->Start());
    TestFalse(TEXT("Rejected capture does not retain ownership"), Capture->IsActive());
    Capture->Cancel(); Capture->Cancel();
    TestEqual(TEXT("Rejected capture/cancel preserve filter selection"), IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.Method"))->GetInt(), Previous);
    World->DestroyWorld(false);
    return true;
}
#endif
