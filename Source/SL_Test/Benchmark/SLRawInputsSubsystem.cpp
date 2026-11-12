#include "SLRawInputsSubsystem.h"
#include "SLComparisonSubsystem.h"
#include "SLFlythroughSubsystem.h"
#include "SLDenoiser.h"
#include "SL_Test.h"
#include "DLSSLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PostProcess/PostProcessMaterial.h"
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "UnrealClient.h"

class FSLRawInputsView : public FWorldSceneViewExtension
{
public:
    FSLRawInputsView(const FAutoRegister& Register, UWorld* World) : FWorldSceneViewExtension(Register, World) {}
    TAtomic<uint32> Frames{0};
    virtual int32 GetPriority() const override { return -100; }
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView& V) override
    { if (!V.bIsSceneCapture) V.bIsOfflineRender = false; }
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& V,
        FAfterPassCallbackDelegateArray& Callbacks, bool) override
    {
        if (Pass == EPostProcessingPass::MotionBlur && !V.bIsSceneCapture && V.Family->bIsMainViewFamily)
            Callbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSLRawInputsView::Draw));
    }
private:
    FScreenPassTexture Draw(FRDGBuilder& Graph, const FSceneView& V, const FPostProcessMaterialInputs& Inputs)
    {
        FSLDenoiserContext Context;
        Context.SceneColor = FScreenPassTexture(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
        Context.SceneTextures = Inputs.SceneTextures;
        FSLDenoiserNeuralPlaceholder Grid(0);
        auto Output = Grid.Apply(Graph, V, Context);
        ++Frames;
        return Output;
    }
};

static FAutoConsoleCommandWithWorldAndArgs CmdRawInputs(TEXT("raw-inputs"),
    TEXT("raw-inputs [cancel]: save the current camera's diagnostic input grid to Saved/RawInputs. Restores renderer/camera afterward."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        auto* Capture = World ? World->GetSubsystem<USLRawInputsSubsystem>() : nullptr;
        if (!Capture) { UE_LOG(LogSL_Test, Warning, TEXT("raw-inputs: start Play or Standalone Game first.")); return; }
        if (Args.IsEmpty()) Capture->Start();
        else if (Args.Num() == 1 && Args[0].Equals(TEXT("cancel"), ESearchCase::IgnoreCase)) Capture->Cancel();
        else UE_LOG(LogSL_Test, Warning, TEXT("Usage: raw-inputs [cancel]"));
    }));

TStatId USLRawInputsSubsystem::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(USLRawInputsSubsystem, STATGROUP_Tickables); }

bool USLRawInputsSubsystem::Start()
{
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* VP = GetWorld()->GetGameViewport();
    auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>();
    auto* Flythrough = GetWorld()->GetSubsystem<USLFlythroughSubsystem>();
    if (bActive || !PC || !PC->PlayerCameraManager || !VP || !VP->Viewport
        || !GetWorld()->GetGameInstance() || GetWorld()->GetGameInstance()->GetLocalPlayers().Num() != 1
        || (Comparison && Comparison->IsBusy()) || (Flythrough && !Flythrough->CanChangePath())
        || FScreenshotRequest::IsScreenshotRequested())
    {
        UE_LOG(LogSL_Test, Warning, TEXT("raw-inputs: requires one playable viewport and no active capture, recording, preview, or batch."));
        return false;
    }
    Size = VP->Viewport->GetSizeXY();
    if (Size.GetMin() <= 0) return false;
    const auto POV = PC->PlayerCameraManager->GetCameraCacheView();
    Camera = GetWorld()->SpawnActor<ACameraActor>(POV.Location, POV.Rotation);
    if (!Camera) return false;
    auto* Component = Camera->GetCameraComponent();
    Component->FieldOfView = POV.FOV;
    Component->AspectRatio = POV.AspectRatio;
    Component->bConstrainAspectRatio = POV.bConstrainAspectRatio;
    Component->ProjectionMode = POV.ProjectionMode;
    Component->OrthoWidth = POV.OrthoWidth;
    Component->PostProcessSettings = POV.PostProcessSettings;
    PreviousCamera = PC->GetViewTarget();
    bPaused = PC->IsPaused();
    bPathTracing = VP->EngineShowFlags.PathTracing;
    bDLSS = UDLSSLibrary::IsDLSSEnabled(); bRR = UDLSSLibrary::IsDLSSRREnabled();
    bLive = Comparison && Comparison->IsLiveComparisonEnabled();
    if (bLive) Comparison->DisableLiveComparison();
    Saved.Reset();
    for (const auto& Setting : TArray<TPair<FString, FString>>{
        {TEXT("SL.Denoiser.Method"), TEXT("0")}, {TEXT("SL.NoiseFallback.Scale"), TEXT("0")},
        {TEXT("r.ScreenPercentage"), TEXT("100")}, {TEXT("r.DynamicRes.OperationMode"), TEXT("0")},
        {TEXT("r.ScreenshotDelegate"), TEXT("1")}, {TEXT("r.MotionBlurQuality"), TEXT("0")}})
        if (auto* Var = IConsoleManager::Get().FindConsoleVariable(*Setting.Key))
        { Saved.Add(Setting.Key, Var->GetString()); Var->Set(*Setting.Value, ECVF_SetByConsole); }
    UDLSSLibrary::EnableDLSSRR(false); UDLSSLibrary::EnableDLSS(false);
    UKismetRenderingLibrary::EnablePathTracing(false);
    PC->SetViewTarget(Camera); PC->SetPause(true); PC->PlayerCameraManager->UpdateCamera(0.f);
    bActive = true; bPending = false; Started = FPlatformTime::Seconds();
    Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RawInputs"),
        FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H-%M-%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    if (!IFileManager::Get().MakeDirectory(*Folder, true)) { Finish(true); return false; }
    View = FSceneViewExtensions::NewExtension<FSLRawInputsView>(GetWorld());
    ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(this, &USLRawInputsSubsystem::Screenshot);
    UE_LOG(LogSL_Test, Display, TEXT("raw-inputs: capturing to %s"), *Folder);
    return true;
}

void USLRawInputsSubsystem::Tick(float)
{
    const auto* VP = GetWorld()->GetGameViewport();
    if (!VP || !VP->Viewport || VP->Viewport->GetSizeXY() != Size || FPlatformTime::Seconds() - Started > 60)
    { UE_LOG(LogSL_Test, Warning, TEXT("raw-inputs: cancelled after timeout or viewport change.")); Cancel(); return; }
    if (!bPending && View && View->Frames.Load() >= 2)
    { bPending = true; FScreenshotRequest::RequestScreenshot(false); }
}

void USLRawInputsSubsystem::Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
{
    if (!bPending) return;
    TArray64<uint8> PNG;
    if (FIntPoint(Width, Height) == Size) FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
    const bool bSaved = !PNG.IsEmpty() && FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(Folder, TEXT("raw-inputs.png")));
    const FString Notes = FString::Printf(TEXT("raw-inputs: deferred-rendered diagnostic view (not a denoising benchmark).\nCamera: %s\nFOV: %g\nResolution: %d x %d\nGrid: scene color, albedo, world normals, depth, motion vectors, placeholder composite.\nNo roughness tile. Missing renderer buffers use fallback textures. Camera/gameplay held for capture; motion vectors may be zero.\n"),
        *Camera->GetActorTransform().ToString(), Camera->GetCameraComponent()->FieldOfView, Width, Height);
    if (bSaved) FFileHelper::SaveStringToFile(Notes, *FPaths::Combine(Folder, TEXT("capture.txt")));
    UE_LOG(LogSL_Test, Display, TEXT("raw-inputs: %s %s"), bSaved ? TEXT("saved") : TEXT("failed to save"), *Folder);
    Finish(true);
}

void USLRawInputsSubsystem::Finish(bool bResumeComparison)
{
    if (!bActive) return;
    UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle); ScreenshotHandle.Reset();
    if (bPending) FScreenshotRequest::Reset();
    FlushRenderingCommands(); View.Reset(); FlushRenderingCommands();
    if (auto* PC = GetWorld()->GetFirstPlayerController())
    { if (PreviousCamera) PC->SetViewTarget(PreviousCamera); PC->SetPause(bPaused); }
    if (Camera) Camera->Destroy(); Camera = nullptr; PreviousCamera = nullptr;
    UKismetRenderingLibrary::EnablePathTracing(bPathTracing);
    UDLSSLibrary::EnableDLSS(bDLSS); UDLSSLibrary::EnableDLSSRR(bRR);
    for (const auto& Pair : Saved)
        if (auto* Var = IConsoleManager::Get().FindConsoleVariable(*Pair.Key)) Var->Set(*Pair.Value, ECVF_SetByConsole);
    Saved.Reset(); bActive = false; bPending = false;
    if (bResumeComparison && bLive)
        if (auto* Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>())
            if (auto* Config = GetDefault<USLGMToolSettings>()->Configuration.LoadSynchronous())
                if (const auto* Entry = Config->FindProfile(Config->BaselineProfileId)) Comparison->SetLiveComparisonEnabled(true, *Entry, Config);
    bLive = false;
}
void USLRawInputsSubsystem::Cancel() { Finish(true); }
void USLRawInputsSubsystem::Deinitialize() { Finish(false); Super::Deinitialize(); }
