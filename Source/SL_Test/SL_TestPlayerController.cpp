// Copyright Epic Games, Inc. All Rights Reserved.


#include "SL_TestPlayerController.h"
#include "Debug/GMTools/SLGMToolsComponent.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "TimerManager.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "Blueprint/UserWidget.h"
#include "SL_Test.h"
#include "Widgets/Input/SVirtualJoystick.h"

ASL_TestPlayerController::ASL_TestPlayerController()
{
	GMTools = CreateDefaultSubobject<USLGMToolsComponent>(TEXT("GMTools"));
}

void ASL_TestPlayerController::gm()
{
	if (!IsLocalPlayerController()) return;
	// Console submission restores viewport focus. Open on the following tick so
	// that restoration cannot steal focus from the panel's UI input mode.
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]
	{
		if (ULocalPlayer* Player = GetLocalPlayer())
		{
			if (Player->ViewportClient && Player->ViewportClient->ViewportConsole)
				Player->ViewportClient->ViewportConsole->FakeGotoState(NAME_None);
		}
		GMTools->OpenPanel();
	}));
}

void ASL_TestPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// only spawn touch controls on local player controllers
	if (IsLocalPlayerController() && ShouldUseTouchControls())
	{
		// spawn the mobile controls widget
		MobileControlsWidget = CreateWidget<UUserWidget>(this, MobileControlsWidgetClass);

		if (MobileControlsWidget)
		{
			// add the controls to the player screen
			MobileControlsWidget->AddToPlayerScreen(0);

		} else {

			UE_LOG(LogSL_Test, Error, TEXT("Could not spawn mobile controls widget."));

		}

	}
}

void ASL_TestPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		// Add Input Mapping Contexts
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}

			// only add these IMCs if we're not using mobile touch input
			if (!ShouldUseTouchControls())
			{
				for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
				{
					Subsystem->AddMappingContext(CurrentContext, 0);
				}
			}
		}
	}
}

bool ASL_TestPlayerController::ShouldUseTouchControls() const
{
	// are we on a mobile platform? Should we force touch?
	return SVirtualJoystick::ShouldDisplayTouchInterface() || bForceTouchControls;
}
