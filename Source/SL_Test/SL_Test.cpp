// Copyright Epic Games, Inc. All Rights Reserved.

#include "SL_Test.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FSL_TestModule, SL_Test, "SL_Test");

DEFINE_LOG_CATEGORY(LogSL_Test)

void FSL_TestModule::StartupModule()
{
	// /Project is automatically mapped to <ProjectDir>/Shaders/ by UE5 at engine startup,
	// before any game module's StartupModule() is called.
	// No additional shader directory registration is required here.
}

void FSL_TestModule::ShutdownModule()
{
	// Shader source directory mappings are global; no per-entry removal API
	// exists, so we leave the mapping in place for the lifetime of the process.
}