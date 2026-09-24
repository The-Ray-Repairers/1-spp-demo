// Copyright Epic Games, Inc. All Rights Reserved.

#include "SLPathTracerSubsystem.h"
#include "SL_Test.h"
#include "SLDenoiser.h"
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

TAutoConsoleVariable<int32> CVarSLPathTracerTargetSPP(
	TEXT("SL.PathTracer.TargetSPP"),
	1,
	TEXT("Target Samples Per Pixel for the real-time path tracer or its software fallback.\n"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		if (IConsoleVariable* EngineSPPCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel")))
		{
			EngineSPPCVar->Set(Var->GetInt(), ECVF_SetByConsole);
		}
	}),
	ECVF_Default);

extern TAutoConsoleVariable<int32> CVarSLDenoiserMethod;

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
	NoiseFallbackViewExtension.Reset();
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

	const int32 TargetSPP = CVarSLPathTracerTargetSPP.GetValueOnGameThread();

	if (!IsRayTracingEnabled())
	{
		UE_LOG(LogSL_Test, Warning,
			TEXT("SL.PathTracer: hardware ray tracing is not available — "
			     "activating the software noise fallback (see SLNoiseFallbackViewExtension)."));

		// Activate the software fallback: injects Poisson shot noise before the
		// tonemapper each frame to approximate the visual appearance of a real 1-spp
		// path-traced output.  The two on-screen messages make it unmistakable to any
		// developer that this is NOT the physically-based solution.
		NoiseFallbackViewExtension =
			FSceneViewExtensions::NewExtension<FSLNoiseFallbackViewExtension>(GetWorld());

		if (GEngine)
		{
			// Key 0xA010 / 0xA011: arbitrary stable IDs so repeated PIE sessions
			// don't stack duplicate messages.
			GEngine->AddOnScreenDebugMessage(
				0xA010, 3600.f, FColor::Red,
				TEXT("  !! SOFTWARE NOISE FALLBACK ACTIVE !! "
				     "-- output is NOT physically-based  "));
			GEngine->AddOnScreenDebugMessage(
				0xA011, 3600.f, FColor::Orange,
				FString::Printf(TEXT("  Hardware ray tracing unavailable. "
				     "Noise approximates %d-spp variance. "
				     "Adjust: SL.NoiseFallback.Scale (default 1.0)  "), TargetSPP));
		}
		return;
	}

	// Normal path: enable the built-in path tracer for the game viewport.
	// Override the engine's built-in SamplesPerPixel so our CVar acts as the central control.
	if (IConsoleVariable* EngineSPPCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel")))
	{
		EngineSPPCVar->Set(TargetSPP, ECVF_SetByConsole);
	}

	// Equivalent to the "Enable Path Tracing" Blueprint node.
	UKismetRenderingLibrary::EnablePathTracing(true);
}

void USLPathTracerSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (GEngine && GetWorld() && !GetWorld()->IsNetMode(NM_DedicatedServer))
	{
		const int32 MethodInt = CVarSLDenoiserMethod.GetValueOnGameThread();
		FString MethodName;
		
		switch (MethodInt)
		{
		case 0:
			MethodName = TEXT("None (Passthrough)");
			break;
		case 1:
			MethodName = TEXT("Gaussian Blur");
			break;
		case 2:
			MethodName = TEXT("Bilateral Filter");
			break;
		case 3:
			MethodName = TEXT("Neural Placeholder");
			break;
		default:
			MethodName = FString::Printf(TEXT("Unknown (%d)"), MethodInt);
			break;
		}

		// Key 0xA012: Stable ID so we overwrite the same message every frame
		// instead of spamming the screen with new lines.
		GEngine->AddOnScreenDebugMessage(
			0xA012, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("  Denoiser Active: %s (Method %d)  "), *MethodName, MethodInt));
	}
}

TStatId USLPathTracerSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USLPathTracerSubsystem, STATGROUP_Tickables);
}
