#pragma once

#include "CoreMinimal.h"

class APlayerController;
class USLGMToolConfig;
class SWidget;

/** Builds the configured rows; viewport ownership and input stay in the component. */
TSharedRef<SWidget> CreateGMPanel(USLGMToolConfig* Configuration, APlayerController* Player, FSimpleDelegate OnMinimize);
