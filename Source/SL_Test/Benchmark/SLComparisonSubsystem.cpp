#include "SLComparisonSubsystem.h"
#include "SLFlythroughSubsystem.h"
#include "SLComparisonView.h"
#include "SLGuideProbe.h"
#include "SL_Test.h"
#include "Debug/GMTools/SLGMCommandService.h"
#include "DLSSLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "UnrealClient.h"

const FSLGMToolEntry& USLComparisonSubsystem::ResolveEntry(const FSLGMToolEntry& Entry) const
{
    const auto* Config = Configuration ? Configuration.Get() : GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (Config && !Entry.Id.IsNone())
        if (const auto* Current = Config->Entries.FindByPredicate([&](const FSLGMToolEntry& E) { return E.Id == Entry.Id; })) return *Current;
    return Entry;
}

int32 USLComparisonSubsystem::ReadSamples(const FSLGMToolEntry& Entry) const
{
    if (Entry.Profile.SamplesVariable.IsEmpty()) return Entry.Profile.SamplesPerPixel;
    const auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Entry.Profile.SamplesVariable);
    return Variable ? Variable->GetInt() : 0;
}

bool USLComparisonSubsystem::CanExecute(ESLComparisonAction Action, FText& Reason) const
{
    const auto* Config = Configuration ? Configuration.Get() : GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    if (Config)
        if (const auto* Entry = Config->Entries.FindByPredicate([Action](const FSLGMToolEntry& E) { return E.Control == ESLGMControlType::RenderingAction && E.Action == Action; }))
            return CanExecute(*Entry, Reason);
    Reason = FText::FromString(TEXT("Rendering profile is missing."));
    return false;
}

bool USLComparisonSubsystem::CanExecute(const FSLGMToolEntry& Requested, FText& Reason) const
{
    const auto& Entry = ResolveEntry(Requested);
    auto Reject = [&Reason](const TCHAR* Text) { Reason = FText::FromString(Text); return false; };
    if (const auto* Flythrough = GetWorld()->GetSubsystem<USLFlythroughSubsystem>(); Flythrough && Flythrough->IsBusy())
        return Reject(TEXT("Wait for the flythrough benchmark to finish."));
    if (Entry.Action == ESLComparisonAction::Restore) return OwnsRenderer();
    if (Entry.Action == ESLComparisonAction::OpenResults) return !Output.IsEmpty();
    if (Entry.Id.IsNone()) return Reject(TEXT("Give each rendering profile a unique Id."));
    if (Configuration && Configuration->Entries.FilterByPredicate([&](const FSLGMToolEntry& E) { return E.Id == Entry.Id; }).Num() > 1)
        return Reject(TEXT("Rendering profile Ids must be unique."));
    if (!GetWorld()->GetFirstPlayerController() || !GetWorld()->GetGameViewport()) return Reject(TEXT("Start the game first."));
    if (!GetWorld()->GetGameViewport()->Viewport) return Reject(TEXT("The game viewport is not ready."));
    const auto& Profile = Entry.Profile;
    if ((Profile.Backend != ESLGMRenderBackend::LumenDLSSRR || bLiveComparison) && !IsRayTracingEnabled()) return Reject(TEXT("Hardware ray tracing is not enabled."));
    if (ReadSamples(Entry) < 1) return Reject(TEXT("The configured sample count must be at least 1; check SamplesVariable or SamplesPerPixel."));
    if (Profile.Backend != ESLGMRenderBackend::LumenDLSSRR && !Profile.bAccumulate && ReadSamples(Entry) != 1)
        return Reject(TEXT("Enable Accumulate for multi-sample path tracing."));
    if (Profile.bSaveImage && !Profile.bAccumulate) return Reject(TEXT("Saving a reference requires Accumulate."));
    if ((Profile.bAccumulate || Profile.Backend == ESLGMRenderBackend::LumenDLSSRR || ReadSamples(Entry) != 1)
        && Profile.ComparisonSource != ESLGMComparisonSource::IndependentRaw)
        return Reject(TEXT("Accumulated, multi-sample and Lumen previews require Independent Raw comparison."));
    if (Profile.bAccumulate && Profile.Backend != ESLGMRenderBackend::PathTracing)
        return Reject(TEXT("Accumulation currently requires the Path Tracing backend."));
    for (const auto& Setting : Entry.RenderSettings)
        if (!IConsoleManager::Get().FindConsoleVariable(*Setting.Name))
        { Reason = FText::FromString(TEXT("Unavailable rendering setting: ") + Setting.Name); return false; }
    if (GetWorld()->GetGameInstance()->GetLocalPlayers().Num() != 1) return Reject(TEXT("Comparison requires one local player and one viewport."));
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && Context.World() != GetWorld() && Context.World()->IsGameWorld())
            return Reject(TEXT("Close other game instances before comparing."));
    if (Profile.Backend == ESLGMRenderBackend::SVGF)
    {
        if (IConsoleManager::Get().FindConsoleVariable(TEXT("r.VelocityOutputPass"))->GetInt() != 1)
            return Reject(TEXT("Restart the editor to apply the SVGF startup settings."));
        if (IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate.ProjectGBufferFormat"))->GetInt() != 0)
            return Reject(TEXT("The current GBuffer format is not supported by this SVGF implementation."));
    }
    if (Profile.Backend == ESLGMRenderBackend::LumenDLSSRR &&
        (!UDLSSLibrary::IsDLSSSupported() || !UDLSSLibrary::IsDLSSRRSupported()))
        return Reject(TEXT("DLSS RR is unavailable. Use a supported GPU and DX12 Standalone Game."));
    Reason = FText::GetEmpty();
    return true;
}

void USLComparisonSubsystem::SetVariable(const FString& Name, const FString& Value)
{
    if (auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Name))
    {
        if (!SavedVariables.Contains(Name)) SavedVariables.Add(Name, Variable->GetString());
        Variable->Set(*Value, ECVF_SetByConsole);
    }
}

void USLComparisonSubsystem::ReleaseGuide()
{
    if (Guide) { FlushRenderingCommands(); Guide.Reset(); FlushRenderingCommands(); }
}

void USLComparisonSubsystem::RestoreVariables()
{
    for (const auto& Pair : SavedVariables)
    {
        if (auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
            Variable->Set(*Pair.Value, ECVF_SetByConsole);
    }
}

bool USLComparisonSubsystem::Apply()
{
    const int32 RequestedSamples = ReadSamples(ActiveEntry);
    DisableLiveComparison();
    for (const auto& Setting : ActiveEntry.RenderSettings)
        if (!IConsoleManager::Get().FindConsoleVariable(*Setting.Name)) { Fail(TEXT("Unavailable rendering setting: ") + Setting.Name); return false; }
    if (!bOwned)
    {
        bOwned = true;
        bSavedDLSS = UDLSSLibrary::IsDLSSEnabled();
        bSavedRR = UDLSSLibrary::IsDLSSRREnabled();
        bSavedPathTracing = GetWorld()->GetGameViewport()->EngineShowFlags.PathTracing;
        View = FSceneViewExtensions::NewExtension<FSLComparisonView>(GetWorld());
    }
    ReleaseGuide();
    UDLSSLibrary::EnableDLSSRR(false);
    UDLSSLibrary::EnableDLSS(false);
    // Reset the union of owned settings before applying a mode, so e.g. offline
    // samples and DLSS AA settings cannot leak into the next renderer.
    RestoreVariables();
    // Snapshot all configured values before scalability can change dependent CVars.
    for (const auto& Setting : ActiveEntry.RenderSettings)
        if (!SavedVariables.Contains(Setting.Name)) SavedVariables.Add(Setting.Name, IConsoleManager::Get().FindConsoleVariable(*Setting.Name)->GetString());
    for (const auto& Setting : ActiveEntry.RenderSettings) SetVariable(Setting.Name, Setting.Value);
    const auto& Profile = ActiveEntry.Profile;
    ReferenceSamples = RequestedSamples;
    SetVariable(TEXT("r.PathTracing.SamplesPerPixel"), FString::FromInt(ReferenceSamples));
    View->bReference = Profile.bAccumulate;
    View->DenoiserOverride = -1;
    View->Samples.Store(0);
    UKismetRenderingLibrary::EnablePathTracing(Profile.Backend != ESLGMRenderBackend::LumenDLSSRR);
    if (Profile.Backend == ESLGMRenderBackend::LumenDLSSRR)
    {
        UDLSSLibrary::EnableDLSS(true);
        UDLSSLibrary::EnableDLSSRR(true);
        if (!UDLSSLibrary::IsDLSSRREnabled()) { Fail(TEXT("Failed to enable DLSS RR.")); return false; }
    }
    else if (Profile.Backend == ESLGMRenderBackend::SVGF)
    {
        Guide = FSceneViewExtensions::NewExtension<FSLGuideProbe>(GetWorld());
        SetVariable(TEXT("r.PathTracing.Denoiser"), TEXT("1"));
        SetVariable(TEXT("r.PathTracing.SpatialDenoiser"), TEXT("1"));
        SetVariable(TEXT("r.PathTracing.SpatialDenoiser.Type"), TEXT("0"));
        SetVariable(TEXT("r.PathTracing.Denoiser.Name"), TEXT("SLGuides"));
    }
    if (CaptureCamera)
    {
        auto& PP = CaptureCamera->GetCameraComponent()->PostProcessSettings;
        PP.bOverride_PathTracingSamplesPerPixel = true;
        PP.PathTracingSamplesPerPixel = ReferenceSamples;
        PP.bOverride_PathTracingMaxBounces = true;
        PP.PathTracingMaxBounces = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.MaxBounces"))->GetInt();
    }
    Status = GetButtonLabel(ActiveEntry).ToString() + TEXT(" enabled");
    return true;
}

bool USLComparisonSubsystem::Execute(const FSLGMToolEntry& Requested, USLGMToolConfig* Config)
{
    if (!Config) { Status = TEXT("No GM configuration asset assigned."); return false; }
    Configuration = Config;
    const FSLGMToolEntry Entry = ResolveEntry(Requested);
    FText Reason;
    if (!CanExecute(Entry, Reason)) { Status = Reason.ToString(); return false; }
    if (Entry.Action == ESLComparisonAction::Restore) { Restore(); return true; }
    if (Entry.Action == ESLComparisonAction::OpenResults) { FPlatformProcess::ExploreFolder(*Output); return true; }
    // A selected preset may replace an in-progress capture; do not lock the GM panel.
    CancelCapture(TEXT("Capture replaced by another selection."));
    CaptureMetadata.Reset();
    PreviewBeforeCapture = ActiveEntry;
    bResumeLiveAfterCapture = bLiveComparison && !Entry.Profile.bKeepSelectedAfterCapture;
    const auto* Method = ActiveEntry.Profile.LabelOptionVariable.IsEmpty() ? nullptr : IConsoleManager::Get().FindConsoleVariable(*ActiveEntry.Profile.LabelOptionVariable);
    MethodBeforeCapture = Method ? Method->GetInt() : 0;
    ActiveEntry = Entry;
    if (Entry.Profile.bAccumulate) return StartCapture();
    return ApplySelectedProfile();
}

void USLComparisonSubsystem::Restore()
{
    bResumeLiveAfterCapture = false;
    DisableLiveComparison();
    CancelCapture(TEXT("Cancelled. Partial results were retained."));
    ReleaseGuide();
    if (bOwned)
    {
        FlushRenderingCommands(); View.Reset();
        UDLSSLibrary::EnableDLSSRR(false); UDLSSLibrary::EnableDLSS(false);
        RestoreVariables();
        UKismetRenderingLibrary::EnablePathTracing(bSavedPathTracing);
        UDLSSLibrary::EnableDLSS(bSavedDLSS); UDLSSLibrary::EnableDLSSRR(bSavedRR);
    }
    SavedVariables.Reset(); bOwned = false;
    Status = TEXT("Original settings restored.");
}

void USLComparisonSubsystem::Fail(const FString& Message)
{
    UE_LOG(LogSL_Test, Error, TEXT("Rendering comparison failed: %s"), *Message);
    const FString Error = Message;
    Restore(); Status = Error;
    if (!CaptureMetadata.IsEmpty()) { WriteReport(); FFileHelper::SaveStringToFile(Error, *FPaths::Combine(Output, TEXT("failed.txt"))); }
}

FText USLComparisonSubsystem::GetButtonLabel(const FSLGMToolEntry& Requested) const
{
    const auto& Entry = ResolveEntry(Requested);
    FFormatNamedArguments Args;
    const auto Count = FText::AsNumber(ReadSamples(Entry));
    Args.Add(TEXT("0"), Count); Args.Add(TEXT("Samples"), Count);
    return FText::Format(Entry.Label, Args);
}

bool USLComparisonSubsystem::RefreshSelectedProfile()
{
    if (!bOwned || !Configuration) return false;
    const auto* Current = Configuration->FindProfile(ActiveEntry.Id);
    if (!Current) return false;
    // Re-read the asset and bound sample CVar, including edits made after selection.
    if (!FSLGMToolEntry::StaticStruct()->CompareScriptStruct(Current, &ActiveEntry, 0)
        || ReadSamples(*Current) != ReferenceSamples)
    {
        return Execute(*Current, Configuration);
    }
    return false;
}

bool USLComparisonSubsystem::SelectPreviewMethod(const FSLGMToolEntry& Entry, int32 Value, FText& Result)
{
    if (!CanSelectPreviewMethod() || !Configuration || Entry.PreviewProfileId.IsNone()) return false;
    if (!Entry.RadioOptions.ContainsByPredicate([Value](const FSLGMRadioOption& O) { return O.Value == Value; })) return false;
    const auto* Raw = Configuration->FindProfile(Entry.PreviewProfileId);
    if (!Raw) { Result = FText::FromString(TEXT("Raw rendering profile is missing.")); return false; }
    // Switch the renderer, not merely the caption: filters must receive raw input.
    if (!Execute(*Raw, Configuration)) { Result = GetStatus(); return false; }
    const bool bApplied = FSLGMCommandService::Apply(Entry, FString::FromInt(Value), Result);
    Status = Result.ToString();
    OnLiveComparisonChanged.Broadcast();
    return bApplied;
}

void USLComparisonSubsystem::Deinitialize() { Restore(); Super::Deinitialize(); }

void USLComparisonSubsystem::Tick(float DeltaTime)
{
    if (RefreshSelectedProfile()) return;
    if (RefreshPreviewMethod()) return;
    TickCapture();
}

bool USLComparisonSubsystem::ApplySelectedProfile()
{
    const bool bRestoreComparison = bLiveComparison;
    if (!Apply()) return false;
    if (bRestoreComparison && !ActivateLiveComparison())
    {
        Fail(Status);
        return false;
    }
    OnLiveComparisonChanged.Broadcast();
    return true;
}

bool USLComparisonSubsystem::RefreshPreviewMethod()
{
    if (!bLiveComparison || !Guide || !Configuration) return false;
    for (const auto& Control : Configuration->Entries)
    {
        if (Control.PreviewProfileId.IsNone() || Control.PreviewProfileId == ActiveEntry.Id) continue;
        const auto* Profile = Configuration->FindProfile(Control.PreviewProfileId);
        const auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Control.ConsoleVariable);
        if (!Profile || !Variable || Variable->GetInt() == Profile->Profile.PassthroughOptionValue) continue;
        FText Result;
        if (SelectPreviewMethod(Control, Variable->GetInt(), Result)) return true;
    }
    return false;
}
