#include "Debug/GMTools/SLGMToolsComponent.h"
#include "Debug/GMTools/SLGMToolConfig.h"
#include "SLGMPanel.h"
#include "SLGMToolsStyle.h"
#include "Debug/GMTools/SLLiveComparisonUserWidget.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SViewport.h"
#include "Styling/CoreStyle.h"
#include "RT_Demo.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"

// `gm` works in any project: it attaches the tools to whichever local player
// controller is in play, so no game-side class has to know about this plugin.
static FAutoConsoleCommandWithWorldAndArgs GSLOpenGMPanelCommand(
	TEXT("gm"),
	TEXT("Open or restore the RT_Demo GM tools panel for the local player."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		// The editor's own console passes the editor world; use the running game world instead.
		if ((!World || !World->IsGameWorld()) && GEngine)
		{
			World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
				if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
		}
		APlayerController* PC = World && GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
		if (!PC)
		{
			UE_LOG(LogRTDemo, Warning, TEXT("gm: no local player controller. Start play, then enter gm again."));
			return;
		}
		// Console submission restores viewport focus. Open on the following tick so
		// that restoration cannot steal focus from the panel's UI input mode.
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(PC, [PC]
		{
			if (ULocalPlayer* Player = PC->GetLocalPlayer())
			{
				if (Player->ViewportClient && Player->ViewportClient->ViewportConsole)
					Player->ViewportClient->ViewportConsole->FakeGotoState(NAME_None);
			}
			if (USLGMToolsComponent* Tools = USLGMToolsComponent::GetOrCreate(PC)) Tools->OpenPanel();
		}));
	}));

USLGMToolsComponent* USLGMToolsComponent::GetOrCreate(APlayerController* PlayerController)
{
	if (!PlayerController) return nullptr;
	if (USLGMToolsComponent* Existing = PlayerController->FindComponentByClass<USLGMToolsComponent>()) return Existing;
	USLGMToolsComponent* Created = NewObject<USLGMToolsComponent>(PlayerController, TEXT("RTDemoGMTools"), RF_Transient);
	Created->RegisterComponent();
	return Created;
}

void USLGMToolsComponent::TogglePanel()
{
	if (IsPanelOpen()) MinimizePanel();
	else OpenPanel();
}

void USLGMToolsComponent::OpenPanel()
{
	if (IsPanelOpen()) return;
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC || !PC->IsLocalController() || !PC->GetLocalPlayer() || !PC->GetLocalPlayer()->ViewportClient) return;
	ULocalPlayer* Player = PC->GetLocalPlayer();
	if (!Panel.IsValid())
	{
		const USLGMToolSettings* Settings = GetDefault<USLGMToolSettings>();
		Configuration = Settings->Configuration.LoadSynchronous();
		Panel = CreateGMPanel(Configuration, PC,
			FSimpleDelegate::CreateUObject(this, &USLGMToolsComponent::MinimizePanel));
		Player->ViewportClient->AddViewportWidgetForPlayer(Player, Panel.ToSharedRef(), 100);
		if (Configuration)
		{
			const TSubclassOf<USLLiveComparisonUserWidget> WidgetClass = Configuration->LiveComparisonWidgetClass
				? Configuration->LiveComparisonWidgetClass.Get() : USLLiveComparisonUserWidget::StaticClass();
			LiveComparison = CreateWidget<USLLiveComparisonUserWidget>(PC, WidgetClass);
			if (LiveComparison)
			{
				LiveComparison->Style = Configuration->LiveComparison;
				LiveComparison->AddToPlayerScreen(90);
			}
		}
		// Snapshot once per panel lifetime, not on restore from the minimized state.
		bPreviousCursor = PC->bShowMouseCursor;
	}
	if (MinimizedButton.IsValid())
	{
		Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef());
		MinimizedButton.Reset();
	}
	bMinimized = false;
	Panel->SetVisibility(EVisibility::SelfHitTestInvisible);
	PC->SetIgnoreMoveInput(true);
	PC->SetIgnoreLookInput(true);
	bInputCaptured = true;
	PC->FlushPressedKeys();
	PC->bShowMouseCursor = true;
	FInputModeUIOnly Mode;
	Mode.SetWidgetToFocus(Panel);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
}

void USLGMToolsComponent::MinimizePanel()
{
	if (!IsPanelOpen()) return;
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC || !PC->GetLocalPlayer() || !PC->GetLocalPlayer()->ViewportClient) return;
	ULocalPlayer* Player = PC->GetLocalPlayer();
	// Keep the existing panel so pending edits, status, and scroll position survive.
	Panel->SetVisibility(EVisibility::Collapsed);
	bMinimized = true;
	MinimizedButton = SNew(SBox).HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(16)
		.Visibility(EVisibility::SelfHitTestInvisible)
		[SNew(SButton).ContentPadding(FMargin(12, 8)).IsFocusable(false)
			.ButtonColorAndOpacity(FGMToolsStyle::Get().PanelBackground)
			.OnClicked(FOnClicked::CreateWeakLambda(this, [this]
			{
				OpenPanel();
				return FReply::Handled();
			}))
			[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Configuration ? Configuration->RestorePanelLabel : FText::GetEmpty())]];
	Player->ViewportClient->AddViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef(), 100);
	if (bInputCaptured)
	{
		PC->SetIgnoreMoveInput(false);
		PC->SetIgnoreLookInput(false);
		bInputCaptured = false;
	}
	PC->FlushPressedKeys();
	// Leave the launcher clickable, while returning keyboard input to the viewport.
	PC->bShowMouseCursor = true;
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(Player->ViewportClient->GetGameViewportWidget());
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
}

void USLGMToolsComponent::ClosePanel()
{
	if (!Panel.IsValid()) return;
	if (APlayerController* PC = Cast<APlayerController>(GetOwner()))
	{
		if (auto* Comparison = PC->GetWorld()->GetSubsystem<USLComparisonSubsystem>()) Comparison->DisableLiveComparison();
		if (ULocalPlayer* Player = PC->GetLocalPlayer())
		{
			if (Player->ViewportClient)
			{
				Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, Panel.ToSharedRef());
				if (LiveComparison) LiveComparison->RemoveFromParent();
				if (MinimizedButton.IsValid())
					Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef());
			}
		}
		if (bInputCaptured)
		{
			PC->SetIgnoreMoveInput(false);
			PC->SetIgnoreLookInput(false);
		}
		PC->bShowMouseCursor = bPreviousCursor;
		PC->SetInputMode(FInputModeGameOnly());
		PC->FlushPressedKeys();
	}
	bInputCaptured = false;
	bMinimized = false;
	MinimizedButton.Reset();
	LiveComparison = nullptr;
	Panel.Reset();
	Configuration = nullptr;
}

void USLGMToolsComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	ClosePanel();
	Super::EndPlay(Reason);
}
