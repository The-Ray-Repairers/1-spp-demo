// Copyright Epic Games, Inc. All Rights Reserved.

#include "SLPathTracerSubsystem.h"
#include "SL_Test.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderUtils.h"
#include "SceneView.h"

static TAutoConsoleVariable<int32> CVarSLPathTracerAutoEnable(
	TEXT("SL.PathTracer.AutoEnable"),
	1,
	TEXT("Turn on the path tracer for the game viewport when a game world begins play.\n")
	TEXT("0: leave the viewport alone (enable it yourself with ShowFlag.PathTracing 1 or the Path Tracing view mode)\n")
	TEXT("1: enable (default)"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSLPathTracerResetEveryFrame(
	TEXT("SL.PathTracer.ResetEveryFrame"),
	1,
	TEXT("Restart path tracer accumulation on every frame so each frame is an independent 1-spp sample.\n")
	TEXT("0: standard behaviour, the image accumulates while the camera and scene are static\n")
	TEXT("1: reset every frame (default)"),
	ECVF_Default);

FSLPathTracerResetViewExtension::FSLPathTracerResetViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FSLPathTracerResetViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	// Offline renders (Movie Render Queue) drive accumulation themselves, so leave those views alone.
	if (InViewFamily.EngineShowFlags.PathTracing && !InView.bIsOfflineRender && CVarSLPathTracerResetEveryFrame.GetValueOnGameThread() != 0)
	{
		InView.bForcePathTracerReset = true;
	}
}

void USLPathTracerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	ResetViewExtension = FSceneViewExtensions::NewExtension<FSLPathTracerResetViewExtension>(GetWorld());
}

void USLPathTracerSubsystem::Deinitialize()
{
	ResetViewExtension.Reset();

	Super::Deinitialize();
}

void USLPathTracerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (CVarSLPathTracerAutoEnable.GetValueOnGameThread() == 0)
	{
		return;
	}

	if (!IsRayTracingEnabled())
	{
		UE_LOG(LogSL_Test, Warning, TEXT("SL.PathTracer: hardware ray tracing is not available on this machine, the path tracer stays off."));
		return;
	}

	// Equivalent to the "Enable Path Tracing" Blueprint node: sets the PathTracing show flag on the game viewport.
	UKismetRenderingLibrary::EnablePathTracing(true);
}
