#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "SLLiveComparisonUserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameViewportClient.h"
#include "DLSSLibrary.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/IConsoleManager.h"
#include "Camera/CameraActor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/Pawn.h"
#include "EngineUtils.h"

struct FSLRRTestState
{
    FAutomationTestBase* Test = nullptr;
    TWeakObjectPtr<USLComparisonSubsystem> Comparison;
    TStrongObjectPtr<USLLiveComparisonUserWidget> Widget;
    USLGMToolConfig* Config = nullptr;
    FSLGMToolEntry RR, SVGF, Raw, Method, Reference;
    FString SavedReferenceSamples;
    FName CustomProfileId = TEXT("Automation.CustomAccumulation");
    double Started = FPlatformTime::Seconds();
    int32 Stage = 0;
    TArray<FLinearColor> FirstPixels;
    TWeakObjectPtr<AActor> PreviousCamera;
    TWeakObjectPtr<ACameraActor> Camera;
    ~FSLRRTestState()
    {
        if (Camera.IsValid())
        {
            if (auto* PC = Camera->GetWorld()->GetFirstPlayerController()) PC->SetViewTarget(PreviousCamera.Get());
            Camera->Destroy();
        }
    }
};

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FSLCheckRRComparison, TSharedPtr<FSLRRTestState>, State);
bool FSLCheckRRComparison::Update()
{
    auto* Comparison = State->Comparison.Get();
    auto* Test = State->Test;
    if (!Comparison) { Test->AddError(TEXT("Comparison world ended early")); return true; }
    if (State->Stage >= 2)
    {
        if ((Comparison->IsBusy() || FPlatformTime::Seconds() - State->Started < .3) && FPlatformTime::Seconds() - State->Started < 60) return false;
        Test->TestFalse(TEXT("Accumulation completed"), Comparison->IsBusy());
        Test->TestTrue(TEXT("Selected result remains in comparison"), Comparison->IsLiveComparisonEnabled());
        Test->TestEqual(TEXT("Divider preserved after accumulation"), Comparison->GetComparisonPosition(), .27f);
        if (State->Stage == 2)
        {
            Test->TestTrue(TEXT("Reference stays selected instead of returning to RR"), Comparison->IsProfileSelected(State->Reference.Id));
            Test->TestFalse(TEXT("Reference is not RR"), UDLSSLibrary::IsDLSSRREnabled());
            Test->TestTrue(TEXT("16 samples actually accumulated"), Comparison->GetCompletedSamples() >= 16);
            IConsoleManager::Get().FindConsoleVariable(*State->Reference.Profile.SamplesVariable)->Set(1, ECVF_SetByConsole);
            State->Stage = 3; State->Started = FPlatformTime::Seconds(); return false;
        }
        if (State->Stage == 3)
        {
            Test->TestTrue(TEXT("1-SPP reference remains selected"), Comparison->IsProfileSelected(State->Reference.Id));
            Test->TestTrue(TEXT("Caption follows dynamically changed sample count"), Comparison->GetLiveAlgorithmLabel().ToString().Contains(TEXT("1 SPP")));
            auto Custom = State->Reference;
            Custom.Id = State->CustomProfileId;
            Custom.Action = ESLComparisonAction::ApplyProfile;
            Custom.Label = FText::FromString(TEXT("Custom ({Samples} SPP)"));
            Custom.Profile.PreviewLabel = FText::FromString(TEXT("Configured preview {Samples}"));
            Custom.Profile.SamplesVariable.Empty();
            Custom.Profile.SamplesPerPixel = 8;
            Custom.Profile.bSaveImage = false;
            State->Config->Entries.Add(Custom);
            Test->TestTrue(TEXT("New data-only profile executes without algorithm-specific code"), Comparison->Execute(Custom, State->Config));
            State->Stage = 4; State->Started = FPlatformTime::Seconds(); return false;
        }
        if (State->Stage == 4)
        {
            Test->TestTrue(TEXT("Custom profile remains selected"), Comparison->IsProfileSelected(State->CustomProfileId));
            Test->TestTrue(TEXT("Custom profile accumulated 8 samples"), Comparison->GetCompletedSamples() >= 8);
            auto* Custom = State->Config->Entries.FindByPredicate([&](const FSLGMToolEntry& E) { return E.Id == State->CustomProfileId; });
            Custom->Profile.SamplesPerPixel = 32;
            Custom->Profile.PreviewLabel = FText::FromString(TEXT("Edited preset {Samples}"));
            State->Stage = 5; State->Started = FPlatformTime::Seconds(); return false;
        }
        if (State->Stage == 5)
        {
            Test->TestTrue(TEXT("Editing the selected asset restarts actual accumulation"), Comparison->GetCompletedSamples() >= 32);
            Test->TestEqual(TEXT("Caption uses edited asset data"), Comparison->GetLiveAlgorithmLabel().ToString(), FString(TEXT("Edited preset 32")));
            auto* Custom = State->Config->Entries.FindByPredicate([&](const FSLGMToolEntry& E) { return E.Id == State->CustomProfileId; });
            Custom->Profile.SamplesPerPixel = 64;
            Test->TestTrue(TEXT("Start valid capture before invalid live edit"), Comparison->Execute(*Custom, State->Config));
            Custom->Profile.SamplesPerPixel = 0;
            State->Stage = 6; State->Started = FPlatformTime::Seconds(); return false;
        }
        Test->TestTrue(TEXT("Invalid live edit does not block the last valid accumulation"), Comparison->GetCompletedSamples() >= 64);
        Test->TestEqual(TEXT("Rejected edit preserves the active caption"), Comparison->GetLiveAlgorithmLabel().ToString(), FString(TEXT("Edited preset 64")));
        auto* LongCapture = State->Config->Entries.FindByPredicate([&](const FSLGMToolEntry& E) { return E.Id == State->CustomProfileId; });
        LongCapture->Profile.SamplesPerPixel = 4096;
        Test->TestTrue(TEXT("Start a long accumulation"), Comparison->Execute(*LongCapture, State->Config));
        Test->TestTrue(TEXT("Accumulation is in progress"), Comparison->IsBusy());
        FText FilterResult;
        Test->TestTrue(TEXT("GM filter can replace an in-progress accumulation"), Comparison->SelectPreviewMethod(State->Method, 1, FilterResult));
        Test->TestFalse(TEXT("Previous accumulation is cancelled"), Comparison->IsBusy());
        Test->TestTrue(TEXT("Can switch away from a retained reference"), Comparison->Execute(State->RR, State->Config));
        Test->TestTrue(TEXT("RR keeps wipe after reference"), Comparison->IsLiveComparisonEnabled() && UDLSSLibrary::IsDLSSRREnabled());
        Comparison->Restore();
        State->Config->Entries.RemoveAll([&](const FSLGMToolEntry& E) { return E.Id == State->CustomProfileId; });
        IConsoleManager::Get().FindConsoleVariable(*State->Config->ReferenceSamplesVariable)->Set(*State->SavedReferenceSamples, ECVF_SetByConsole);
        Test->TestNull(TEXT("Restore releases the extra capture"), Comparison->GetRawComparisonTarget());
        State->Widget->RemoveFromParent();
        return true;
    }
    const uint32 RequiredFrames = State->Stage == 0 ? 8 : 16;
    if (Comparison->GetIndependentCompositeCount() < RequiredFrames)
    {
        if (FPlatformTime::Seconds() - State->Started < 60) return false;
        Test->AddError(TEXT("Independent raw/current compositor did not render"));
        Comparison->Restore(); State->Widget->RemoveFromParent();
        return true;
    }
    auto* Caption = Cast<UTextBlock>(State->Widget->WidgetTree->FindWidget(TEXT("AfterText")));
    FFormatNamedArguments Args;
    Args.Add(TEXT("Algorithm"), State->RR.Label);
    Test->TestTrue(TEXT("Actual viewport widget displays RR label"), Caption && Caption->GetText().EqualTo(FText::Format(State->Widget->Style.AfterLabelFormat, Args)));
    Test->TestTrue(TEXT("RR is actually enabled"), UDLSSLibrary::IsDLSSRREnabled());
    Test->TestTrue(TEXT("Wipe survives RR selection"), Comparison->IsLiveComparisonEnabled());
    Test->TestEqual(TEXT("Divider preserved across renderers"), Comparison->GetComparisonPosition(), .27f);
    TArray<FLinearColor> Pixels;
    auto* Target = Comparison->GetRawComparisonTarget();
    const bool bRead = Target && Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax));
    Test->TestTrue(TEXT("Raw baseline contains rendered pixels"), bRead && Pixels.Num() > 1);
    float Min = TNumericLimits<float>::Max(), Max = 0;
    for (const auto& Pixel : Pixels) { Min = FMath::Min(Min, Pixel.GetLuminance()); Max = FMath::Max(Max, Pixel.GetLuminance()); }
    Test->AddInfo(FString::Printf(TEXT("Raw capture pixels=%d luminance=[%g,%g]"), Pixels.Num(), Min, Max));
    Test->TestTrue(TEXT("Raw baseline is not black or a clear-color placeholder"), Max > 0 && Max - Min > .001f);
    if (State->Stage++ == 0)
    {
        State->FirstPixels = MoveTemp(Pixels);
        return false;
    }
    Test->TestTrue(TEXT("Stationary raw baseline generates a fresh 1-SPP sample"), Pixels != State->FirstPixels);
    Test->TestTrue(TEXT("Disable wipe"), Comparison->SetLiveComparisonEnabled(false, State->Raw, State->Config));
    Test->TestTrue(TEXT("Re-enable on selected RR"), Comparison->SetLiveComparisonEnabled(true, State->Raw, State->Config));
    Test->TestTrue(TEXT("Checkbox does not switch RR to SVGF"), UDLSSLibrary::IsDLSSRREnabled() && Comparison->GetLiveAlgorithmLabel().EqualTo(State->RR.Label));
    FText Result;
    Test->TestTrue(TEXT("GM method selector works from RR"), Comparison->SelectPreviewMethod(State->Method, 1, Result));
    Test->TestTrue(TEXT("Method selection keeps wipe"), Comparison->IsLiveComparisonEnabled());
    Test->TestTrue(TEXT("Switch SVGF with wipe"), Comparison->Execute(State->SVGF, State->Config));
    Test->TestTrue(TEXT("Switch raw with wipe"), Comparison->Execute(State->Raw, State->Config));
    Test->TestTrue(TEXT("Raw keeps wipe"), Comparison->IsLiveComparisonEnabled());
    Test->TestEqual(TEXT("Divider still preserved"), Comparison->GetComparisonPosition(), .27f);
    Test->TestTrue(TEXT("Select RR before reference task"), Comparison->Execute(State->RR, State->Config));
    auto* Samples = IConsoleManager::Get().FindConsoleVariable(*State->Config->ReferenceSamplesVariable);
    State->SavedReferenceSamples = Samples->GetString();
    Samples->Set(16, ECVF_SetByConsole);
    Test->TestTrue(TEXT("Start reference without losing wipe"), Comparison->Execute(State->Reference, State->Config));
    Test->TestTrue(TEXT("Reference keeps comparison widget active"), Comparison->IsLiveComparisonEnabled());
    State->Started = FPlatformTime::Seconds();
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLComparisonRRTest, "SL.Comparison.LiveRR",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FSLComparisonRRTest::RunTest(const FString&)
{
    UWorld* World = nullptr;
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
    if (!TestNotNull(TEXT("DX12 game world"), World)) return false;
    auto* Config = GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (!TestNotNull(TEXT("GM config"), Config)) return false;
    auto State = MakeShared<FSLRRTestState>();
    State->Test = this; State->Config = Config;
    // The demo's default pawn can fall while automation waits for startup.
    // Use the map-authored start as a stable camera instead of testing empty sky.
    auto* PC = World->GetFirstPlayerController();
    FVector CameraLocation = PC->GetSpawnLocation();
    FRotator CameraRotation = PC->GetControlRotation();
    if (const APawn* Pawn = PC->GetPawn()) CameraLocation.Z += Pawn->BaseEyeHeight;
    for (TActorIterator<APlayerStart> It(World); It; ++It)
    {
        CameraLocation = It->GetActorLocation(); CameraRotation = It->GetActorRotation();
        break;
    }
    for (TActorIterator<ACameraActor> It(World); It; ++It)
    { CameraLocation = It->GetActorLocation(); CameraRotation = It->GetActorRotation(); break; }
    State->PreviousCamera = PC->GetViewTarget();
    State->Camera = World->SpawnActor<ACameraActor>(CameraLocation, CameraRotation);
    if (!TestNotNull(TEXT("Stable rendering test camera"), State->Camera.Get())) return false;
    PC->SetViewTarget(State->Camera.Get());
    AddInfo(FString::Printf(TEXT("Test camera: %s"), *CameraLocation.ToString()));
    for (const auto& Entry : Config->Entries)
    {
        if (Entry.Control == ESLGMControlType::RenderingAction)
        {
            if (Entry.Action == ESLComparisonAction::RR) State->RR = Entry;
            if (Entry.Action == ESLComparisonAction::SVGF) State->SVGF = Entry;
            if (Entry.Action == ESLComparisonAction::Raw) State->Raw = Entry;
            if (Entry.Action == ESLComparisonAction::Reference) State->Reference = Entry;
        }
        if (Entry.ConsoleVariable == TEXT("SL.Denoiser.Method")) State->Method = Entry;
    }
    auto* Comparison = World->GetSubsystem<USLComparisonSubsystem>();
    State->Comparison = Comparison;
    FText Reason;
    if (!Comparison->CanExecute(ESLComparisonAction::RR, Reason)) { AddError(Reason.ToString()); return false; }
    State->Widget.Reset(CreateWidget<USLLiveComparisonUserWidget>(World, Config->LiveComparisonWidgetClass));
    State->Widget->Style = Config->LiveComparison;
    State->Widget->AddToViewport(90);
    bool bOK = Comparison->Execute(State->SVGF, Config) && Comparison->SetLiveComparisonEnabled(true, State->SVGF, Config);
    Comparison->SetComparisonPosition(.27f);
    bOK = bOK && Comparison->Execute(State->RR, Config);
    if (!TestTrue(TEXT("Switch from SVGF wipe to RR wipe"), bOK))
    { AddError(Comparison->GetStatus().ToString()); Comparison->Restore(); State->Widget->RemoveFromParent(); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FSLCheckRRComparison(State));
    return true;
}
#endif
