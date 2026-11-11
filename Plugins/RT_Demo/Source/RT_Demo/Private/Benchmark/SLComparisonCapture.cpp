#include "SLComparisonSubsystem.h"
#include "SLComparisonView.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

bool USLComparisonSubsystem::StartCapture()
{
    if (FScreenshotRequest::IsScreenshotRequested())
    { Status = TEXT("Another screenshot is active. Try again later."); return false; }
    ReferenceSamples = ReadSamples(ActiveEntry);
    if (ReferenceSamples < 1) { Status = TEXT("The configured sample count must be at least 1."); return false; }
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Viewport = GetWorld()->GetGameViewport();
    CaptureSize = Viewport->Viewport->GetSizeXY();
    if (CaptureSize.GetMin() <= 0) { Status = TEXT("The viewport has no valid image size."); return false; }
    if (ActiveEntry.Profile.bSaveImage)
    {
        const FString FolderName = FString::Printf(TEXT("%s_%dSPP"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H-%M-%S")), ReferenceSamples);
        const FString BaseDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("References"), FolderName));
        Output = BaseDirectory;
        for (int32 Suffix = 2; IFileManager::Get().DirectoryExists(*Output) || IFileManager::Get().FileExists(*Output); ++Suffix)
            Output = FString::Printf(TEXT("%s_%d"), *BaseDirectory, Suffix);
        if (!IFileManager::Get().MakeDirectory(*Output, true))
        { Status = TEXT("Unable to create the reference output folder."); return false; }
    }
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
    CaptureState = ECaptureState::Accumulating;
    if (ActiveEntry.Profile.bSaveImage)
        ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(this, &USLComparisonSubsystem::Screenshot);
    StartReference();
    return IsBusy();
}

void USLComparisonSubsystem::StartReference()
{
    if (!ApplySelectedProfile()) return;
    StageStarted = FPlatformTime::Seconds();
    TargetSamples = ReferenceSamples;
    if (!ActiveEntry.Profile.bSaveImage) return;
    CaptureMetadata = FString::Printf(TEXT("Label: %s\nTarget SPP: %d\nCamera: %s\nFOV: %g\nResolution: %d x %d\n"),
        *ActiveEntry.Label.ToString(), TargetSamples, *CaptureCamera->GetActorTransform().ToString(),
        CaptureCamera->GetCameraComponent()->FieldOfView, CaptureSize.X, CaptureSize.Y);
    for (const auto& Pair : SavedVariables)
        if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) CaptureMetadata += Pair.Key + TEXT("=") + Variable->GetString() + TEXT("\n");
    WriteReport();
}

void USLComparisonSubsystem::Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
{
    if (!IsScreenshotPending()) return;
    if (FIntPoint(Width, Height) != CaptureSize) { Fail(TEXT("The viewport size changed. Start a new capture.")); return; }
    TArray64<uint8> PNG;
    FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
    if (!FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(Output, TEXT("output.png")))) { Fail(TEXT("Failed to save the image.")); return; }
    CaptureState = ECaptureState::ImageSaved;
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
    ScreenshotHandle.Reset();
    if (CaptureCamera)
    {
        if (auto* PC = GetWorld()->GetFirstPlayerController()) { if (PreviousCamera) PC->SetViewTarget(PreviousCamera); PC->SetPause(bPausedBefore); }
        CaptureCamera->Destroy(); CaptureCamera = nullptr; PreviousCamera = nullptr;
    }
}

FText USLComparisonSubsystem::GetStatus() const
{
    if (IsBusy() && View)
        return FText::FromString(FString::Printf(TEXT("Generating reference: %u / %d SPP"), View->Samples.Load(), TargetSamples));
    return FText::FromString(Status);
}

uint32 USLComparisonSubsystem::GetCompletedSamples() const
{
    return View ? View->Samples.Load() : 0;
}

void USLComparisonSubsystem::TickCapture()
{
    if (!IsBusy()) return;
    if (GetWorld()->GetGameViewport()->Viewport->GetSizeXY() != CaptureSize) { Fail(TEXT("Viewport resized. Capture stopped.")); return; }
    if (FPlatformTime::Seconds()-StageStarted > Configuration->CaptureTimeoutSeconds) { Fail(TEXT("Capture timed out. Results were retained.")); return; }
    if (CaptureState == ECaptureState::Accumulating && View->Samples.Load() >= uint32(TargetSamples))
    {
        if (!ActiveEntry.Profile.bSaveImage)
        {
            CaptureState = ECaptureState::Idle;
            Status = TEXT("Reference ready for comparison.");
            OnLiveComparisonChanged.Broadcast();
            return;
        }
        CaptureState = ECaptureState::AwaitingScreenshot;
        // Save the right-side image without the raw overlay. The selected
        // profile determines whether the completed result stays in the viewport.
        SetComparisonPosition(ComparisonPosition);
        FScreenshotRequest::RequestScreenshot(false);
    }
    if (CaptureState == ECaptureState::ImageSaved)
    {
        Status = TEXT("Reference image saved: ") + Output;
        WriteReport();
        CaptureState = ECaptureState::Idle;
        const FString CompletedStatus = Status;
        if (ActiveEntry.Profile.bKeepSelectedAfterCapture)
        {
            SetComparisonPosition(ComparisonPosition);
            OnLiveComparisonChanged.Broadcast();
            return;
        }
        const bool bResume = bResumeLiveAfterCapture && bLiveComparison;
        const FSLGMToolEntry Previous = PreviewBeforeCapture;
        const int32 PreviousMethod = MethodBeforeCapture;
        Restore();
        if (bResume && Execute(Previous, Configuration))
        {
            if (!Previous.Profile.LabelOptionVariable.IsEmpty())
                if (const auto* Method = Configuration->Entries.FindByPredicate([&Previous](const FSLGMToolEntry& E) { return E.ConsoleVariable == Previous.Profile.LabelOptionVariable; }))
                {
                    FText Result;
                    SelectPreviewMethod(*Method, PreviousMethod, Result);
                }
            SetLiveComparisonEnabled(true, Previous, Configuration);
        }
        Status = CompletedStatus;
    }
}

void USLComparisonSubsystem::CancelCapture(const FString& Reason)
{
    if (IsBusy())
    {
        Status = Reason;
        WriteReport();
    }
    if (IsScreenshotPending()) FScreenshotRequest::Reset();
    CaptureState = ECaptureState::Idle;
    ReleaseCamera();
}
