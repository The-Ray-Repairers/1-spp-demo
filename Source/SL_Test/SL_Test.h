// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Main log category used across the project. */
DECLARE_LOG_CATEGORY_EXTERN(LogSL_Test, Log, All);

/**
 * Primary game module.  StartupModule registers the /Project shader virtual
 * directory so that SLNoiseFallback.usf (and any future project shaders) are
 * visible to the shader compiler.
 */
class FSL_TestModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};