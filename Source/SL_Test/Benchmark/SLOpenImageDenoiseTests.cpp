#if WITH_DEV_AUTOMATION_TESTS
#include "SLOpenImageDenoise.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLOpenImageDenoiseSettingsTest, "SL.Flythrough.RendererOIDN",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLOpenImageDenoiseSettingsTest::RunTest(const FString& Parameters)
{
    FText Reason;
    if (!TestTrue(TEXT("Engine OIDN plugin loaded and registered its settings"), SLOpenImageDenoise::IsAvailable(Reason)))
    {
        AddError(Reason.ToString());
        return false;
    }
    TMap<FString, FString> Saved;
    SLOpenImageDenoise::Apply([&](const TCHAR* Name, const TCHAR* Value) {
        auto* Var = IConsoleManager::Get().FindConsoleVariable(Name);
        if (!TestNotNull(FString::Printf(TEXT("Engine setting %s exists"), Name), Var)) return;
        Saved.Add(Name, Var->GetString());
        Var->Set(Value, ECVF_SetByConsole);
        if (FCString::IsNumeric(Value))
            TestEqual(FString::Printf(TEXT("Engine accepts %s"), Name), Var->GetInt(), FCString::Atoi(Value));
        else
            TestEqual(FString::Printf(TEXT("Engine accepts %s"), Name), Var->GetString(), FString(Value));
    });
    for (const auto& Pair : Saved)
        IConsoleManager::Get().FindConsoleVariable(*Pair.Key)->Set(*Pair.Value, ECVF_SetByConsole);
    return true;
}
#endif
