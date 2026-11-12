#include "SLGMToolsComponent.h"
#include "SLGMToolConfig.h"
#include "SLGMPanel.h"
#include "SLGMToolsStyle.h"
#include "SLLiveComparisonUserWidget.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SViewport.h"
#include "Styling/CoreStyle.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "SL_Test.h"

namespace
{
	APlayerController* FindLocalPlayerController(UWorld* World)
	{
		return World && World->IsGameWorld() && GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	}

	/** `gm` works under any GameMode: it adds the GM component to whichever local PlayerController is active. */
	void OpenGMTools(UWorld* InWorld)
	{
		APlayerController* PC = FindLocalPlayerController(InWorld);
		if (!PC && GEngine)
		{
			// Typed from the editor Output Log, the command receives the editor world; fall back to a running game/PIE world.
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if ((PC = FindLocalPlayerController(Context.World())) != nullptr) break;
			}
		}
		if (!PC)
		{
			UE_LOG(LogSL_Test, Warning, TEXT("gm: no local player controller; start play first."));
			return;
		}
		USLGMToolsComponent* GMTools = PC->FindComponentByClass<USLGMToolsComponent>();
		if (!GMTools)
		{
			GMTools = NewObject<USLGMToolsComponent>(PC, TEXT("GMTools_Runtime"));
			GMTools->RegisterComponent();
		}
		// Console submission restores viewport focus. Open on the following tick so
		// that restoration cannot steal focus from the panel's UI input mode.
		PC->GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(GMTools, [GMTools]
		{
			APlayerController* Owner = Cast<APlayerController>(GMTools->GetOwner());
			if (ULocalPlayer* Player = Owner ? Owner->GetLocalPlayer() : nullptr)
			{
				if (Player->ViewportClient && Player->ViewportClient->ViewportConsole)
					Player->ViewportClient->ViewportConsole->FakeGotoState(NAME_None);
			}
			GMTools->OpenPanel();
		}));
	}

	FAutoConsoleCommandWithWorld GGMCommand(
		TEXT("gm"),
		TEXT("Open or restore the runtime GM panel for the local player (works with any PlayerController)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&OpenGMTools));
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
