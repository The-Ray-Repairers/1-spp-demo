// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Subsystems/WorldSubsystem.h"
#include "SLNoiseFallbackViewExtension.h"
#include "SLPathTracerSubsystem.generated.h"

/**
 * Restarts the path tracer's accumulation for every view of its world, every frame, so each frame is a
 * single independent 1-spp sample even when nothing moved. Without this the built-in path tracer keeps
 * showing the first sample until the camera or the scene changes.
 */
class FSLPathTracerResetViewExtension : public FWorldSceneViewExtension
{
public:
	FSLPathTracerResetViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
};

/**
 * Runs Unreal's built-in path tracer as a real-time 1-spp renderer:
 *  - turns the path tracer on for the game viewport when a game world begins play
 *  - resets accumulation every frame (see FSLPathTracerResetViewExtension)
 * Samples per pixel and the denoisers are configured in Config/DefaultEngine.ini under [SystemSettings].
 * Both behaviours can be toggled at runtime with the SL.PathTracer.* console variables.
 */
UCLASS()
class USLPathTracerSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
    void SetFilterComparisonPosition(float Position)
    {
        if (NoiseFallbackViewExtension) NoiseFallbackViewExtension->SetComparisonPosition(Position);
    }

private:
	TSharedPtr<FSLPathTracerResetViewExtension, ESPMode::ThreadSafe> ResetViewExtension;

	/**
	 * Non-null only when hardware ray tracing is unavailable.
	 * Injects Poisson shot noise before the tonemapper so that the viewport
	 * visually approximates 1-spp path-traced output on machines that lack DXR.
	 * A persistent on-screen warning is also shown whenever this is active.
	 */
	TSharedPtr<FSLNoiseFallbackViewExtension, ESPMode::ThreadSafe> NoiseFallbackViewExtension;
};
