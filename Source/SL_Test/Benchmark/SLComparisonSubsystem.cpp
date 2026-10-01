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
#include "Serialization/JsonSerializer.h"
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

static bool SaveJson(const FString& File, const TSharedRef<FJsonObject>& Json)
{
    FString Text;
    FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
    return FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool USLComparisonSubsystem::CanExecute(ESLComparisonAction Action, FText& Reason) const
{
    auto Reject = [&Reason](const TCHAR* Text) { Reason = FText::FromString(Text); return false; };
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
    const auto* Settings = GetDefault<USLComparisonSettings>();
    const auto* Button = Settings->Buttons.FindByPredicate([Action](const FSLComparisonButton& B) { return B.Action == Action; });
    FString Text;
    TSharedPtr<FJsonObject> NewProfile;
    if (!Button || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(FPaths::ProjectDir(), Button->Profile)) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), NewProfile))
    { Fail(TEXT("Unable to load the rendering profile.")); return false; }
    for (const auto& Pair : NewProfile->GetObjectField(TEXT("cvars"))->Values)
        if (!IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) { Fail(TEXT("Unavailable rendering setting: ") + FString(*Pair.Key)); return false; }
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
    if (Action == ESLComparisonAction::Reference)
    {
        NewProfile->GetObjectField(TEXT("cvars"))->SetNumberField(TEXT("r.PathTracing.SamplesPerPixel"), ReferenceSamples);
    }
    Profile = NewProfile;
    // Snapshot before any scalability CVar can change dependent settings.
    for (const auto& Pair : Profile->GetObjectField(TEXT("cvars"))->Values)
        if (!SavedVariables.Contains(FString(*Pair.Key))) SavedVariables.Add(FString(*Pair.Key), IConsoleManager::Get().FindConsoleVariable(*Pair.Key)->GetString());
    for (const auto& Pair : Profile->GetObjectField(TEXT("cvars"))->Values)
        SetVariable(FString(*Pair.Key), FString::SanitizeFloat(Pair.Value->AsNumber()));
    SetVariable(TEXT("SL.Denoiser.Method"), TEXT("0"));
    SetVariable(TEXT("r.DefaultFeature.AutoExposure"), TEXT("0"));
    SetVariable(TEXT("r.EyeAdaptation.ExposureCompensation"), TEXT("0"));
    SetVariable(TEXT("r.PathTracing.TemporalDenoiser"), TEXT("0"));
    SetVariable(TEXT("r.PathTracing.SpatialDenoiser"), TEXT("0"));
    SetVariable(TEXT("r.PathTracing.Denoiser"), TEXT("0"));
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
        PP.PathTracingSamplesPerPixel = Action == ESLComparisonAction::Reference ? int32(Profile->GetObjectField(TEXT("cvars"))->GetNumberField(TEXT("r.PathTracing.SamplesPerPixel"))) : 1;
        PP.bOverride_PathTracingMaxBounces = true;
        PP.PathTracingMaxBounces = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.MaxBounces"))->GetInt();
    }
    Status = Button->Label.ToString() + TEXT(" enabled");
    return true;
}

bool USLComparisonSubsystem::Execute(ESLComparisonAction Action)
{
    FText Reason;
    if (!CanExecute(Action, Reason)) { Status = Reason.ToString(); return false; }
    if (Action == ESLComparisonAction::Reference) return StartCapture();
    return Apply(Action);
}

bool USLComparisonSubsystem::StartCapture()
{
    if (FScreenshotRequest::IsScreenshotRequested())
    { Status = TEXT("Another screenshot is active. Try again later."); return false; }
    const auto* Samples = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.PathTracer.TargetSPP"));
    if (!Samples || Samples->GetInt() < 1) { Status = TEXT("Set Target Samples Per Pixel to at least 1 and click Apply."); return false; }
    ReferenceSamples = Samples->GetInt();
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Viewport = GetWorld()->GetGameViewport();
    CaptureSize = Viewport->Viewport->GetSizeXY();
    if (CaptureSize.GetMin() <= 0) { Status = TEXT("The viewport has no valid image size."); return false; }
    Output = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Comparisons"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    IFileManager::Get().MakeDirectory(*Output, true);
    const auto POV = PC->PlayerCameraManager->GetCameraCacheView();
    PreviousCamera = PC->GetViewTarget();
    bPausedBefore = PC->IsPaused();
    CaptureCamera = GetWorld()->SpawnActor<ACameraActor>(POV.Location, POV.Rotation);
    if (!CaptureCamera) { Status = TEXT("Unable to create the capture camera."); return false; }
    CaptureCamera->GetCameraComponent()->FieldOfView = POV.FOV;
    CaptureCamera->GetCameraComponent()->PostProcessSettings = POV.PostProcessSettings;
    PC->SetViewTarget(CaptureCamera);
    PC->SetPause(true);
    StageResult.Reset();
    bCapturing = true;
    ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(this, &USLComparisonSubsystem::Screenshot);
    StartReference();
    return IsBusy();
}

void USLComparisonSubsystem::StartReference()
{
    const auto Action = ESLComparisonAction::Reference;
    StageDirectory = FPaths::Combine(Output, StaticEnum<ESLComparisonAction>()->GetNameStringByValue(int64(Action)));
    IFileManager::Get().MakeDirectory(*StageDirectory, true);
    if (!Apply(Action)) return;
    bRequested = false; bImageSaved = false;
    StageStarted = FPlatformTime::Seconds();
    TargetSamples = Action == ESLComparisonAction::Reference ? CaptureCamera->GetCameraComponent()->PostProcessSettings.PathTracingSamplesPerPixel : 1;
    StageResult = MakeShared<FJsonObject>();
    StageResult->SetStringField(TEXT("mode"), StaticEnum<ESLComparisonAction>()->GetNameStringByValue(int64(Action)));
    const auto* Button = GetDefault<USLComparisonSettings>()->Buttons.FindByPredicate([Action](const FSLComparisonButton& B) { return B.Action == Action; });
    StageResult->SetStringField(TEXT("label"), (Button->ResultLabel.IsEmpty() ? Button->Label : Button->ResultLabel).ToString());
    if (Action == ESLComparisonAction::Reference)
        StageResult->SetStringField(TEXT("label"), FString::Printf(TEXT("%s (%d SPP)"), *(Button->ResultLabel.IsEmpty() ? Button->Label : Button->ResultLabel).ToString(), ReferenceSamples));
    StageResult->SetNumberField(TEXT("target_samples"), TargetSamples);
    StageResult->SetStringField(TEXT("purpose"), Button->Purpose.ToString());
    StageResult->SetBoolField(TEXT("end_to_end"), Button->bEndToEnd);
    StageResult->SetObjectField(TEXT("profile"), Profile.ToSharedRef());
    StageResult->SetStringField(TEXT("camera"), CaptureCamera->GetActorTransform().ToString());
    StageResult->SetNumberField(TEXT("fov"), CaptureCamera->GetCameraComponent()->FieldOfView);
    StageResult->SetNumberField(TEXT("width"), CaptureSize.X); StageResult->SetNumberField(TEXT("height"), CaptureSize.Y);
    auto Effective = MakeShared<FJsonObject>();
    for (const auto& Pair : SavedVariables)
        if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) Effective->SetStringField(Pair.Key, Variable->GetString());
    StageResult->SetObjectField(TEXT("effective_cvars"), Effective);
    SaveJson(FPaths::Combine(StageDirectory, TEXT("capture.json")), StageResult.ToSharedRef());
}

void USLComparisonSubsystem::Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
{
    if (!bExpectScreenshot) return;
    bExpectScreenshot = false;
    if (FIntPoint(Width, Height) != CaptureSize) { Fail(TEXT("The viewport size changed. Start a new capture.")); return; }
    TArray64<uint8> PNG;
    FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
    if (!FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(StageDirectory, TEXT("output.png")))) { Fail(TEXT("Failed to save the image.")); return; }
    bImageSaved = true;
}

void USLComparisonSubsystem::WriteReport()
{
    if (!StageResult) return;
    StageResult->SetStringField(TEXT("status"), Status);
    StageResult->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    StageResult->SetStringField(TEXT("gpu"), GRHIAdapterName);
    StageResult->SetStringField(TEXT("map"), GetWorld()->GetOutermost()->GetName());
    StageResult->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
    StageResult->SetNumberField(TEXT("stage_wall_seconds"), FPlatformTime::Seconds() - StageStarted);
    if (View) StageResult->SetNumberField(TEXT("completed_samples"), View->Samples.Load());
    SaveJson(FPaths::Combine(StageDirectory, TEXT("capture.json")), StageResult.ToSharedRef());
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

FText USLComparisonSubsystem::GetButtonLabel(const FSLComparisonButton& Button) const
{
    if (Button.Action != ESLComparisonAction::Reference) return Button.Label;
    const auto* Samples = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.PathTracer.TargetSPP"));
    const int32 Count = IsBusy() ? ReferenceSamples : (Samples ? Samples->GetInt() : 1);
    return FText::FromString(FString::Printf(TEXT("%s (%d SPP)"), *Button.Label.ToString(), Count));
}

FText USLComparisonSubsystem::GetStatus() const
{
    if (IsBusy() && Active == ESLComparisonAction::Reference && View)
        return FText::FromString(FString::Printf(TEXT("Generating reference: %u / %d SPP"), View->Samples.Load(), TargetSamples));
    return FText::FromString(Status.IsEmpty() ? TEXT("Select a method to preview. Saving locks the current camera.") : Status);
}

void USLComparisonSubsystem::Tick(float DeltaTime)
{
    if (!IsBusy()) return;
    if (GetWorld()->GetGameViewport()->Viewport->GetSizeXY() != CaptureSize) { Fail(TEXT("Viewport resized. Capture stopped.")); return; }
    if (FPlatformTime::Seconds()-StageStarted > GetDefault<USLComparisonSettings>()->TimeoutSeconds) { Fail(TEXT("Capture timed out. Results were retained.")); return; }
    if (!bRequested && View->Samples.Load() >= uint32(TargetSamples))
    {
        bRequested = true; bExpectScreenshot = true;
        FScreenshotRequest::RequestScreenshot(false);
    }
    if (bImageSaved)
    {
        Status = TEXT("Reference image saved: ") + StageDirectory;
        WriteReport();
        bCapturing = false;
        const FString CompletedStatus = Status;
        Restore();
        Status = CompletedStatus;
    }
}

void USLComparisonSubsystem::Deinitialize() { Restore(); Super::Deinitialize(); }
