#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "SLLiveComparisonUserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "SLGMToolsComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLComparisonSwitchTest, "SL.Comparison.LiveMethods",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FSLComparisonSwitchTest::RunTest(const FString& Parameters)
{
    UWorld* World = nullptr;
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
    if (!TestNotNull(TEXT("Run in a DX12 game instance"), World)) return false;
    auto* Config = GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (!TestNotNull(TEXT("GM config"), Config)) return false;
    const auto* SVGF = Config->Entries.FindByPredicate([](const FSLGMToolEntry& E) {
        return E.Control == ESLGMControlType::RenderingAction && E.Action == ESLComparisonAction::SVGF;
    });
    const auto* Method = Config->Entries.FindByPredicate([](const FSLGMToolEntry& E) { return E.ConsoleVariable == TEXT("SL.Denoiser.Method"); });
    if (!TestNotNull(TEXT("SVGF profile"), SVGF) || !TestNotNull(TEXT("Method selector"), Method)) return false;
    auto* Comparison = World->GetSubsystem<USLComparisonSubsystem>();
    auto* Widget = CreateWidget<USLLiveComparisonUserWidget>(World, Config->LiveComparisonWidgetClass);
    if (!TestNotNull(TEXT("Configured UMG presentation"), Widget)) return false;
    Widget->Style = Config->LiveComparison;
    TSharedPtr<SWidget> Presentation = Widget->TakeWidget();
    auto* Caption = Cast<UTextBlock>(Widget->WidgetTree->FindWidget(TEXT("AfterText")));
    if (!TestNotNull(TEXT("Runtime caption widget"), Caption)) return false;
    if (!TestTrue(TEXT("Enable SVGF wipe"), Comparison->Execute(*SVGF, Config) && Comparison->SetLiveComparisonEnabled(true, *SVGF, Config)))
    { AddError(Comparison->GetStatus().ToString()); return false; }
    Comparison->SetComparisonPosition(0.27f);
    TestTrue(TEXT("SVGF caption matches configured mode"), Comparison->GetLiveAlgorithmLabel().EqualTo(SVGF->Label));
    for (int32 Value : {1, 2, 0})
    {
        FText Result;
        TestTrue(TEXT("Select filter through the same GM action"), Comparison->SelectPreviewMethod(*Method, Value, Result));
        TestTrue(TEXT("Wipe remains enabled"), Comparison->IsLiveComparisonEnabled());
        TestFalse(TEXT("SVGF is no longer the input to the new filter"), Comparison->IsSVGFPreview());
        TestEqual(TEXT("Divider position preserved"), Comparison->GetComparisonPosition(), 0.27f);
        TestEqual(TEXT("Actual renderer method matches selection"), IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.Method"))->GetInt(), Value);
        const auto* Option = Method->RadioOptions.FindByPredicate([Value](const FSLGMRadioOption& O) { return O.Value == Value; });
        if (TestNotNull(TEXT("Configured option"), Option))
        {
            const auto* Raw = Config->Entries.FindByPredicate([](const FSLGMToolEntry& E) {
                return E.Control == ESLGMControlType::RenderingAction && E.Action == ESLComparisonAction::Raw;
            });
            const FText Expected = Value == 0 && Raw ? Raw->Label : Option->Label;
            TestTrue(TEXT("Caption matches actual selected filter"), Comparison->GetLiveAlgorithmLabel().EqualTo(Expected));
            FFormatNamedArguments Args;
            Args.Add(TEXT("Algorithm"), Expected);
            TestTrue(TEXT("UMG text updates on the mode-change event"), Caption->GetText().EqualTo(FText::Format(Widget->Style.AfterLabelFormat, Args)));
        }
    }
    TestTrue(TEXT("Switch back to SVGF"), Comparison->Execute(*SVGF, Config));
    TestTrue(TEXT("SVGF wipe remains enabled"), Comparison->IsLiveComparisonEnabled());
    TestTrue(TEXT("SVGF caption restored"), Comparison->GetLiveAlgorithmLabel().EqualTo(SVGF->Label));
    auto* Player = World->GetFirstPlayerController();
    auto* Panel = NewObject<USLGMToolsComponent>(Player);
    Panel->RegisterComponent();
    const bool bPreviousCursor = Player->bShowMouseCursor;
    const bool bPreviousMoveIgnored = Player->IsMoveInputIgnored();
    const bool bPreviousLookIgnored = Player->IsLookInputIgnored();
    Panel->OpenPanel();
    TestTrue(TEXT("Extracted GM panel opens"), Panel->IsPanelOpen());
    Panel->MinimizePanel();
    TestTrue(TEXT("GM panel minimizes"), Panel->IsPanelMinimized());
    TestTrue(TEXT("Minimizing keeps comparison enabled"), Comparison->IsLiveComparisonEnabled());
    Panel->OpenPanel();
    TestTrue(TEXT("GM panel reopens"), Panel->IsPanelOpen());
    TestEqual(TEXT("Panel lifecycle preserves divider"), Comparison->GetComparisonPosition(), .27f);
    Panel->ClosePanel();
    TestFalse(TEXT("GM panel closes"), Panel->IsPanelOpen());
    TestFalse(TEXT("Closing exits comparison"), Comparison->IsLiveComparisonEnabled());
    TestEqual(TEXT("Closing restores cursor"), Player->bShowMouseCursor, bPreviousCursor);
    TestEqual(TEXT("Closing restores move input"), Player->IsMoveInputIgnored(), bPreviousMoveIgnored);
    TestEqual(TEXT("Closing restores look input"), Player->IsLookInputIgnored(), bPreviousLookIgnored);
    Panel->DestroyComponent();
    Comparison->Restore();
    TestFalse(TEXT("Restore exits live comparison"), Comparison->IsLiveComparisonEnabled());
    Presentation.Reset();
    Widget->ReleaseSlateResources(true);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLComparisonConfigTest, "SL.Comparison.Configuration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSLComparisonConfigTest::RunTest(const FString& Parameters)
{
    const auto* Config = GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (!TestNotNull(TEXT("GM configuration"), Config)) return false;
    for (const auto& Entry : Config->Entries)
        for (const auto& Setting : Entry.RenderSettings)
            TestNotNull(Entry.Id.ToString() + TEXT(": registered setting ") + Setting.Name,
                IConsoleManager::Get().FindConsoleVariable(*Setting.Name));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLComparisonUMGTest, "SL.Comparison.UMG",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLComparisonUMGTest::RunTest(const FString& Parameters)
{
    const auto* Config = GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (!TestNotNull(TEXT("GM configuration"), Config)) return false;
    if (!TestNotNull(TEXT("Designer widget class is configured"), Config->LiveComparisonWidgetClass.Get())) return false;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    // Construct both paths without showing a window or drawing a frame.
    for (UClass* Class : {USLLiveComparisonUserWidget::StaticClass(), Config->LiveComparisonWidgetClass.Get()})
    {
        auto* Widget = CreateWidget<USLLiveComparisonUserWidget>(World, Class);
        if (!TestNotNull(TEXT("Widget can be instantiated"), Widget)) continue;
        Widget->Style = Config->LiveComparison;
        TSharedPtr<SWidget> Slate = Widget->TakeWidget();
        auto* Canvas = Cast<UCanvasPanel>(Widget->WidgetTree->RootWidget);
        TestNotNull(TEXT("Real UMG root canvas"), Canvas);
        auto* Handle = Cast<UBorder>(Widget->WidgetTree->FindWidget(TEXT("DragHandle")));
        auto* Line = Cast<UImage>(Widget->WidgetTree->FindWidget(TEXT("Divider")));
        auto* Before = Cast<UTextBlock>(Widget->WidgetTree->FindWidget(TEXT("BeforeText")));
        auto* After = Cast<UTextBlock>(Widget->WidgetTree->FindWidget(TEXT("AfterText")));
        TestNotNull(TEXT("Bound drag handle"), Handle);
        TestNotNull(TEXT("Bound divider"), Line);
        if (TestNotNull(TEXT("Bound before label"), Before)) TestTrue(TEXT("Before text comes from config"), Before->GetText().EqualTo(Config->LiveComparison.BeforeLabel));
        if (TestNotNull(TEXT("Bound after label"), After)) TestTrue(TEXT("After text comes from config"), After->GetText().EqualTo(Config->LiveComparison.AfterLabel));
        if (Handle) TestNotNull(TEXT("Handle has an editable canvas slot"), Cast<UCanvasPanelSlot>(Handle->Slot));
        TestTrue(TEXT("Inactive widget does not intercept input"), Widget->GetVisibility() == ESlateVisibility::Collapsed);
        // Rebuilding/releasing may occur on viewport changes or screen transitions.
        Slate.Reset();
        Widget->ReleaseSlateResources(true);
    }
    return !HasAnyErrors();
}
#endif
