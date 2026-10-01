#include "SLComparisonSubsystem.h"
#include "SLGuideProbe.h"
#include "DLSSLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "SceneViewExtension.h"
#include "SceneManagement.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "RenderingThread.h"
#include "UnrealClient.h"
#include "HAL/FileManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameInstance.h"
#include "RHI.h"
#include "RenderUtils.h"

class FSLComparisonView : public FWorldSceneViewExtension
{
public:
    FSLComparisonView(const FAutoRegister& Register, UWorld* World) : FWorldSceneViewExtension(Register, World) {}
    bool bReference = false;
    TAtomic<uint32> Samples{0};
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView& V) override { if (bReference) V.bIsOfflineRender = true; }
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void PreRenderView_RenderThread(FRDGBuilder&, FSceneView& V) override
    { if (V.State && V.Family->EngineShowFlags.PathTracing) Samples.Store(V.State->GetPathTracingSampleIndex()); }
};

bool USLComparisonSubsystem::CanExecute(ESLComparisonAction Action, FText& Reason) const
{
    auto Reject = [&Reason](const TCHAR* Text) { Reason = FText::FromString(Text); return false; };
    if (Action == ESLComparisonAction::Restore) return OwnsRenderer();
    if (Action == ESLComparisonAction::OpenResults) return !Output.IsEmpty();
    if (IsBusy()) return Reject(TEXT("Capture in progress. Wait or cancel."));
    if (!GetWorld()->GetFirstPlayerController() || !GetWorld()->GetGameViewport()) return Reject(TEXT("Start the game first."));
    if (!GetWorld()->GetGameViewport()->Viewport) return Reject(TEXT("The game viewport is not ready."));
    if (Action != ESLComparisonAction::RR && !IsRayTracingEnabled()) return Reject(TEXT("Hardware ray tracing is not enabled."));
    if (GetWorld()->GetGameInstance()->GetLocalPlayers().Num() != 1) return Reject(TEXT("Comparison requires one local player and one viewport."));
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && Context.World() != GetWorld() && Context.World()->IsGameWorld())
            return Reject(TEXT("Close other game instances before comparing."));
    if (Action == ESLComparisonAction::SVGF)
    {
        if (IConsoleManager::Get().FindConsoleVariable(TEXT("r.VelocityOutputPass"))->GetInt() != 1)
            return Reject(TEXT("Restart the editor to apply the SVGF startup settings."));
        if (IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate.ProjectGBufferFormat"))->GetInt() != 0)
            return Reject(TEXT("The current GBuffer format is not supported by this SVGF implementation."));
    }
    if ((Action == ESLComparisonAction::RR) &&
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

bool USLComparisonSubsystem::Apply(ESLComparisonAction Action)
{
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
    for (const auto& Pair : SavedVariables)
        if (auto* V = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) V->Set(*Pair.Value, ECVF_SetByConsole);
    // Snapshot all configured values before scalability can change dependent CVars.
    for (const auto& Setting : ActiveEntry.RenderSettings)
        if (!SavedVariables.Contains(Setting.Name)) SavedVariables.Add(Setting.Name, IConsoleManager::Get().FindConsoleVariable(*Setting.Name)->GetString());
    for (const auto& Setting : ActiveEntry.RenderSettings) SetVariable(Setting.Name, Setting.Value);
    if (Action == ESLComparisonAction::Reference) SetVariable(TEXT("r.PathTracing.SamplesPerPixel"), FString::FromInt(ReferenceSamples));
    Active = Action;
    View->bReference = Action == ESLComparisonAction::Reference;
    View->Samples.Store(0);
    UKismetRenderingLibrary::EnablePathTracing(Action != ESLComparisonAction::RR);
    if (Action == ESLComparisonAction::RR)
    {
        UDLSSLibrary::EnableDLSS(true);
        UDLSSLibrary::EnableDLSSRR(true);
        if (!UDLSSLibrary::IsDLSSRREnabled()) { Fail(TEXT("Failed to enable DLSS RR.")); return false; }
    }
    else if (Action == ESLComparisonAction::SVGF)
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
        PP.PathTracingSamplesPerPixel = Action == ESLComparisonAction::Reference ? ReferenceSamples : 1;
        PP.bOverride_PathTracingMaxBounces = true;
        PP.PathTracingMaxBounces = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.MaxBounces"))->GetInt();
    }
    Status = GetButtonLabel(ActiveEntry).ToString() + TEXT(" enabled");
    return true;
}

bool USLComparisonSubsystem::Execute(const FSLGMToolEntry& Entry, USLGMToolConfig* Config)
{
    const auto Action = Entry.Action;
    FText Reason;
    if (!CanExecute(Action, Reason)) { Status = Reason.ToString(); return false; }
    if (Action == ESLComparisonAction::Restore) { Restore(); return true; }
    if (Action == ESLComparisonAction::OpenResults) { FPlatformProcess::ExploreFolder(*Output); return true; }
    if (!Config) { Status = TEXT("No GM configuration asset assigned."); return false; }
    Configuration = Config;
    ActiveEntry = Entry;
    if (Action == ESLComparisonAction::Reference) return StartCapture();
    return Apply(Action);
}

bool USLComparisonSubsystem::StartCapture()
{
    if (FScreenshotRequest::IsScreenshotRequested())
    { Status = TEXT("Another screenshot is active. Try again later."); return false; }
    const auto* Samples = IConsoleManager::Get().FindConsoleVariable(*Configuration->ReferenceSamplesVariable);
    if (!Samples || Samples->GetInt() < 1) { Status = TEXT("Set Target Samples Per Pixel to at least 1 and click Apply."); return false; }
    ReferenceSamples = Samples->GetInt();
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Viewport = GetWorld()->GetGameViewport();
    CaptureSize = Viewport->Viewport->GetSizeXY();
    if (CaptureSize.GetMin() <= 0) { Status = TEXT("The viewport has no valid image size."); return false; }
    const FString FolderName = FString::Printf(TEXT("%s_%dSPP"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H-%M-%S")), ReferenceSamples);
    const FString BaseDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("References"), FolderName));
    Output = BaseDirectory;
    for (int32 Suffix = 2; IFileManager::Get().DirectoryExists(*Output) || IFileManager::Get().FileExists(*Output); ++Suffix)
        Output = FString::Printf(TEXT("%s_%d"), *BaseDirectory, Suffix);
    if (!IFileManager::Get().MakeDirectory(*Output, true))
    { Status = TEXT("Unable to create the reference output folder."); return false; }
    const auto POV = PC->PlayerCameraManager->GetCameraCacheView();
    PreviousCamera = PC->GetViewTarget();
    bPausedBefore = PC->IsPaused();
    CaptureCamera = GetWorld()->SpawnActor<ACameraActor>(POV.Location, POV.Rotation);
    if (!CaptureCamera) { Status = TEXT("Unable to create the capture camera."); return false; }
    CaptureCamera->GetCameraComponent()->FieldOfView = POV.FOV;
    CaptureCamera->GetCameraComponent()->PostProcessSettings = POV.PostProcessSettings;
    PC->SetViewTarget(CaptureCamera);
    PC->SetPause(true);
    CaptureMetadata.Reset();
    bCapturing = true;
    ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(this, &USLComparisonSubsystem::Screenshot);
    StartReference();
    return IsBusy();
}

void USLComparisonSubsystem::StartReference()
{
    const auto Action = ESLComparisonAction::Reference;
    if (!Apply(Action)) return;
    bRequested = false; bImageSaved = false;
    StageStarted = FPlatformTime::Seconds();
    TargetSamples = Action == ESLComparisonAction::Reference ? CaptureCamera->GetCameraComponent()->PostProcessSettings.PathTracingSamplesPerPixel : 1;
    CaptureMetadata = FString::Printf(TEXT("Label: %s\nTarget SPP: %d\nCamera: %s\nFOV: %g\nResolution: %d x %d\n"),
        *ActiveEntry.Label.ToString(), TargetSamples, *CaptureCamera->GetActorTransform().ToString(),
        CaptureCamera->GetCameraComponent()->FieldOfView, CaptureSize.X, CaptureSize.Y);
    for (const auto& Pair : SavedVariables)
        if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) CaptureMetadata += Pair.Key + TEXT("=") + Variable->GetString() + TEXT("\n");
    WriteReport();
}

void USLComparisonSubsystem::Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
{
    if (!bExpectScreenshot) return;
    bExpectScreenshot = false;
    if (FIntPoint(Width, Height) != CaptureSize) { Fail(TEXT("The viewport size changed. Start a new capture.")); return; }
    TArray64<uint8> PNG;
    FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
    if (!FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(Output, TEXT("output.png")))) { Fail(TEXT("Failed to save the image.")); return; }
    bImageSaved = true;
}

void USLComparisonSubsystem::WriteReport()
{
    if (CaptureMetadata.IsEmpty()) return;
    const FString Text = CaptureMetadata + FString::Printf(TEXT("Status: %s\nEngine: %s\nGPU: %s\nMap: %s\nUTC: %s\nElapsed seconds: %.3f\nCompleted SPP: %u\n"),
        *Status, *FEngineVersion::Current().ToString(), *GRHIAdapterName, *GetWorld()->GetOutermost()->GetName(),
        *FDateTime::UtcNow().ToIso8601(), FPlatformTime::Seconds() - StageStarted, View ? View->Samples.Load() : 0);
    FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Output, TEXT("capture.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void USLComparisonSubsystem::ReleaseCamera()
{
    UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle);
    if (CaptureCamera)
    {
        if (auto* PC = GetWorld()->GetFirstPlayerController()) { if (PreviousCamera) PC->SetViewTarget(PreviousCamera); PC->SetPause(bPausedBefore); }
        CaptureCamera->Destroy(); CaptureCamera = nullptr; PreviousCamera = nullptr;
    }
}

void USLComparisonSubsystem::Restore()
{
    if (IsBusy() && !Output.IsEmpty()) { Status = TEXT("Cancelled. Partial results were retained."); WriteReport(); }
    if (bExpectScreenshot) FScreenshotRequest::Reset();
    bCapturing = false; bExpectScreenshot = false;
    ReleaseCamera(); ReleaseGuide();
    if (bOwned)
    {
        FlushRenderingCommands(); View.Reset();
        UDLSSLibrary::EnableDLSSRR(false); UDLSSLibrary::EnableDLSS(false);
        for (const auto& Pair : SavedVariables) if (auto* V = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) V->Set(*Pair.Value, ECVF_SetByConsole);
        UKismetRenderingLibrary::EnablePathTracing(bSavedPathTracing);
        UDLSSLibrary::EnableDLSS(bSavedDLSS); UDLSSLibrary::EnableDLSSRR(bSavedRR);
    }
    SavedVariables.Reset(); bOwned = false;
    Status = TEXT("Original settings restored.");
}

void USLComparisonSubsystem::Fail(const FString& Message)
{
    const FString Error = Message;
    Restore(); Status = Error;
    if (!Output.IsEmpty()) { WriteReport(); FFileHelper::SaveStringToFile(Error, *FPaths::Combine(Output, TEXT("failed.txt"))); }
}

FText USLComparisonSubsystem::GetButtonLabel(const FSLGMToolEntry& Button) const
{
    if (Button.Action != ESLComparisonAction::Reference) return Button.Label;
    const auto* Config = Configuration ? Configuration.Get() : GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous();
    const auto* Samples = Config ? IConsoleManager::Get().FindConsoleVariable(*Config->ReferenceSamplesVariable) : nullptr;
    const int32 Count = IsBusy() ? ReferenceSamples : (Samples ? Samples->GetInt() : 1);
    return FText::Format(Button.Label, FText::AsNumber(Count));
}

FText USLComparisonSubsystem::GetStatus() const
{
    if (IsBusy() && Active == ESLComparisonAction::Reference && View)
        return FText::FromString(FString::Printf(TEXT("Generating reference: %u / %d SPP"), View->Samples.Load(), TargetSamples));
    return FText::FromString(Status);
}

void USLComparisonSubsystem::Tick(float DeltaTime)
{
    if (!IsBusy()) return;
    if (GetWorld()->GetGameViewport()->Viewport->GetSizeXY() != CaptureSize) { Fail(TEXT("Viewport resized. Capture stopped.")); return; }
    if (FPlatformTime::Seconds()-StageStarted > Configuration->CaptureTimeoutSeconds) { Fail(TEXT("Capture timed out. Results were retained.")); return; }
    if (!bRequested && View->Samples.Load() >= uint32(TargetSamples))
    {
        bRequested = true; bExpectScreenshot = true;
        FScreenshotRequest::RequestScreenshot(false);
    }
    if (bImageSaved)
    {
        Status = TEXT("Reference image saved: ") + Output;
        WriteReport();
        bCapturing = false;
        const FString CompletedStatus = Status;
        Restore();
        Status = CompletedStatus;
    }
}

void USLComparisonSubsystem::Deinitialize() { Restore(); Super::Deinitialize(); }
