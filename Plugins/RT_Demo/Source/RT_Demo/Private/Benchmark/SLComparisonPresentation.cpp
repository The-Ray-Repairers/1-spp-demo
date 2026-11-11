#include "SLComparisonSubsystem.h"
#include "SLComparisonView.h"
#include "SLGuideProbe.h"
#include "SLRawComparisonView.h"
#include "SLPathTracerSubsystem.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/WorldSettings.h"
#include "RenderingThread.h"

bool USLComparisonSubsystem::NeedsIndependentRaw() const
{
    return ActiveEntry.Profile.ComparisonSource == ESLGMComparisonSource::IndependentRaw;
}

bool USLComparisonSubsystem::SetLiveComparisonEnabled(bool bEnabled, const FSLGMToolEntry& Entry, USLGMToolConfig* Config)
{
    if (!bEnabled) { DisableLiveComparison(); return true; }
    FText Reason;
    if (!Config || !CanEnableLiveComparison(Reason)) { Status = Reason.ToString(); return false; }
    // The checkbox controls presentation only. Never replace an already selected renderer.
    if (!bOwned)
    {
        const auto* Raw = Config->FindProfile(Config->DefaultPreviewProfileId);
        if (!Raw || !Execute(*Raw, Config)) return false;
    }
    Configuration = Config;
    if (!bComparisonPositionInitialized) ComparisonPosition = FMath::Clamp(Config->LiveComparison.InitialPosition, 0.f, 1.f);
    if (!ActivateLiveComparison()) return false;
    OnLiveComparisonChanged.Broadcast();
    return true;
}

void USLComparisonSubsystem::DisableLiveComparison()
{
    const bool bWasEnabled = bLiveComparison;
    bLiveComparison = false;
    if (Guide) Guide->SetComparisonPosition(-1.f);
    if (auto* Path = GetWorld()->GetSubsystem<USLPathTracerSubsystem>()) Path->SetFilterComparisonPosition(-1.f);
    StopRawComparison();
    if (bWasEnabled) OnLiveComparisonChanged.Broadcast();
}

void USLComparisonSubsystem::SetComparisonPosition(float Position)
{
    if (!FMath::IsFinite(Position)) return;
    bComparisonPositionInitialized = true;
    ComparisonPosition = FMath::Clamp(Position, 0.f, 1.f);
    if (bLiveComparison && Guide) Guide->SetComparisonPosition(ComparisonPosition);
    if (RawComparison) RawComparison->SetPosition(bLiveComparison && !IsScreenshotPending() ? ComparisonPosition : -1.f);
    if (auto* Path = GetWorld()->GetSubsystem<USLPathTracerSubsystem>())
        Path->SetFilterComparisonPosition(bLiveComparison && !Guide && !RawComparison ? ComparisonPosition : -1.f);
}

FText USLComparisonSubsystem::GetLiveAlgorithmLabel() const
{
    const auto* Method = ActiveEntry.Profile.LabelOptionVariable.IsEmpty() ? nullptr : IConsoleManager::Get().FindConsoleVariable(*ActiveEntry.Profile.LabelOptionVariable);
    if (!Method || Method->GetInt() == ActiveEntry.Profile.PassthroughOptionValue)
    {
        auto Caption = ActiveEntry;
        if (!Caption.Profile.PreviewLabel.IsEmpty()) Caption.Label = Caption.Profile.PreviewLabel;
        FFormatNamedArguments Args;
        const auto Count = FText::AsNumber(ReferenceSamples);
        Args.Add(TEXT("0"), Count); Args.Add(TEXT("Samples"), Count);
        return FText::Format(Caption.Label, Args);
    }
    if (Configuration && Method)
        for (const auto& Entry : Configuration->Entries)
            if (Entry.ConsoleVariable == ActiveEntry.Profile.LabelOptionVariable)
                for (const auto& Option : Entry.RadioOptions)
                    if (Option.Value == Method->GetInt()) return Option.Label;
    return ActiveEntry.Label;
}

bool USLComparisonSubsystem::CanEnableLiveComparison(FText& Reason) const
{
    const auto* Config = Configuration ? Configuration.Get() : GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    const auto* Baseline = Config ? Config->FindProfile(Config->BaselineProfileId) : nullptr;
    const auto* Selected = bOwned ? &ActiveEntry : (Config ? Config->FindProfile(Config->DefaultPreviewProfileId) : nullptr);
    if (!Baseline || !Selected) { Reason = FText::FromString(TEXT("Configure BaselineProfileId and DefaultPreviewProfileId.")); return false; }
    return CanExecute(*Selected, Reason) && CanExecute(*Baseline, Reason);
}

bool USLComparisonSubsystem::StartRawComparison()
{
    if (RawComparison) return true;
    // These are PT-only settings; Lumen/RR continues to use the chosen GM profile.
    const auto* Raw = Configuration->FindProfile(Configuration->BaselineProfileId);
    if (!Raw) { Status = TEXT("Raw rendering profile is missing."); return false; }
    if (ActiveEntry.Profile.Backend == ESLGMRenderBackend::LumenDLSSRR)
    {
        for (const auto& Setting : Raw->RenderSettings)
            if (Setting.Name.StartsWith(TEXT("r.PathTracing."))) SetVariable(Setting.Name, Setting.Value);
        SetVariable(TEXT("r.PathTracing.Denoiser"), TEXT("0"));
    }
    else
    {
        // Permit the reference view and the raw view to choose their own denoising.
        View->DenoiserOverride = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.Denoiser"))->GetInt();
        SetVariable(TEXT("r.PathTracing.Denoiser"), TEXT("-1"));
    }
    RawTarget = NewObject<UTextureRenderTarget2D>(this);
    RawTarget->InitCustomFormat(1, 1, PF_FloatRGBA, true);
    RawTarget->UpdateResourceImmediate(true);
    RawCapture = NewObject<USceneCaptureComponent2D>(GetWorld()->GetWorldSettings());
    RawCapture->TextureTarget = RawTarget;
    RawCapture->CaptureSource = SCS_SceneColorHDRNoAlpha;
    RawCapture->bCaptureEveryFrame = true;
    RawCapture->bCaptureOnMovement = false;
    RawCapture->bAlwaysPersistRenderingState = true;
    RawCapture->SetTickableWhenPaused(true);
    RawCapture->bUseRayTracingIfEnabled = true;
    RawCapture->bMainViewFamily = true;
    RawCapture->bMainViewCamera = true;
    RawCapture->bMainViewResolution = true;
    RawCapture->bIgnoreScreenPercentage = true;
    RawCapture->bInheritMainViewCameraPostProcessSettings = true;
    RawComparison = FSceneViewExtensions::NewExtension<FSLRawComparisonView>(GetWorld(), RawTarget->GameThread_GetRenderTargetResource());
    RawCapture->RegisterComponentWithWorld(GetWorld());
    // OnRegister restores flags from the component archetype. Set the capture's
    // renderer afterwards so it cannot silently fall back to the Lumen viewport.
    RawCapture->ShowFlags.SetPathTracing(true);
    RawCapture->ShowFlags.SetTemporalAA(false);
    RawCapture->ShowFlags.SetMotionBlur(false);
    RawCapture->ShowFlags.SetPostProcessMaterial(false);
    return true;
}

void USLComparisonSubsystem::StopRawComparison()
{
    if (RawCapture) { RawCapture->DestroyComponent(); RawCapture = nullptr; }
    if (RawComparison) { RawComparison->SetPosition(-1.f); FlushRenderingCommands(); RawComparison.Reset(); FlushRenderingCommands(); }
    RawTarget = nullptr;
}

uint32 USLComparisonSubsystem::GetIndependentCompositeCount() const
{
    return RawComparison ? RawComparison->GetCompositeCount() : 0;
}

bool USLComparisonSubsystem::ActivateLiveComparison()
{
    if (NeedsIndependentRaw() && !StartRawComparison()) return false;
    bLiveComparison = true;
    SetComparisonPosition(ComparisonPosition);
    return true;
}
