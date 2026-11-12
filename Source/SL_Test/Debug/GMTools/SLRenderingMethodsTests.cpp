#if WITH_DEV_AUTOMATION_TESTS
#include "SLRenderingMethods.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLRenderingMethodsTest, "SL.GM.RenderingMethods",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLRenderingMethodsTest::RunTest(const FString& Parameters)
{
    auto* Config = NewObject<USLGMToolConfig>();
    FSLGMToolEntry LegacyRaw;
    LegacyRaw.Id = TEXT("LegacyRaw");
    LegacyRaw.Control = ESLGMControlType::RenderingAction;
    LegacyRaw.Action = ESLComparisonAction::Raw;
    Config->Entries.Add(LegacyRaw);
    FSLGMToolEntry Restore;
    Restore.Control = ESLGMControlType::RenderingAction;
    Restore.Action = ESLComparisonAction::Restore;
    Config->Entries.Add(Restore);
    Config->StandardizeRenderingMethods();
    Config->StandardizeRenderingMethods();
    TestEqual(TEXT("Normalization is idempotent and preserves Restore"), Config->Entries.Num(), 8);
    TestTrue(TEXT("Restore remains after the method list"), Config->Entries.Last().Action == ESLComparisonAction::Restore);
    for (int32 Index = 0; Index < SLRenderingMethods::Get().Num(); ++Index)
    {
        const auto& Definition = SLRenderingMethods::Get()[Index];
        const auto& Entry = Config->Entries[Index];
        TestEqual(TEXT("Profile label follows shared order"), Entry.Label.ToString(), FString(Definition.Label));
        TestTrue(TEXT("Shared ID resolves to the matching profile"), Config->FindProfile(FName(Definition.Id)) == &Entry);
        TestFalse(TEXT("Result ID contains no slash"), FString(Definition.Id).Contains(TEXT("/")));
    }
    const auto& Reference = Config->Entries[1].Profile;
    TestEqual(TEXT("Reference accumulates 512 samples"), Reference.SamplesPerPixel, 512);
    TestTrue(TEXT("Reference accumulation enabled"), Reference.bAccumulate);
    TestTrue(TEXT("Reference ignores legacy sample control"), Reference.SamplesVariable.IsEmpty());
    TestEqual(TEXT("Baseline resolves to raw method"), Config->BaselineProfileId, Config->Entries[0].Id);
    const auto* OIDN = Config->FindProfile(TEXT("OpenImageDenoise"));
    if (TestNotNull(TEXT("OIDN profile exists after repeated migration"), OIDN))
    {
        TestTrue(TEXT("OIDN selects its engine backend"), OIDN->Profile.Backend == ESLGMRenderBackend::OpenImageDenoise);
        TestTrue(TEXT("OIDN wipe has an independent unfiltered input"), OIDN->Profile.ComparisonSource == ESLGMComparisonSource::IndependentRaw);
        TestEqual(TEXT("OIDN denoises one sample"), OIDN->Profile.SamplesPerPixel, 1);
        TestFalse(TEXT("OIDN is not the accumulated reference"), OIDN->Profile.bAccumulate);
    }
    for (const auto& Pair : TArray<TPair<FName, FString>>{{TEXT("Ours"), TEXT("5")}, {TEXT("KPN_Direct"), TEXT("6")}})
    {
        const auto* Profile = Config->FindProfile(Pair.Key);
        const auto* Setting = Profile ? Profile->RenderSettings.FindByPredicate([](const FSLGMRenderSetting& S) { return S.Name == TEXT("SL.Denoiser.Method"); }) : nullptr;
        if (TestNotNull(TEXT("Neural profile has a filter selection"), Setting)) TestEqual(TEXT("Neural profile selects the incoming implementation"), Setting->Value, Pair.Value);
    }
    return true;
}
#endif
