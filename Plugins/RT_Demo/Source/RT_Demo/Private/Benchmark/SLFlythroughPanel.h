#pragma once

#include "CoreMinimal.h"

class USLFlythroughSubsystem;
class SWidget;

/** Creates the Slate flythrough benchmark panel widget. */
TSharedRef<SWidget> CreateFlythroughPanel(USLFlythroughSubsystem* Subsystem);
