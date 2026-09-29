#pragma once

#include "CoreMinimal.h"
#include "SLGMToolConfig.h"

/** UI-independent validation/readback. No state cache and no automatic writes. */
class FSLGMCommandService
{
public:
	static bool Read(const FSLGMToolEntry& Entry, FString& Value, FText& Error);
	static bool Apply(const FSLGMToolEntry& Entry, const FString& Input, FText& Result);
	/** Stops at the first failure. Already executed commands are not rolled back. */
	static bool ExecuteBatch(const FSLGMToolEntry& Entry, class APlayerController* Player, FText& Result);
};
