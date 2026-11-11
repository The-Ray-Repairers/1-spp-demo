#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Debug/GMTools/SLGMToolConfig.h"
#include "SLComparisonSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FSLLiveComparisonChanged);

/** Interactive rendering ownership and same-camera capture, used directly by the GM panel. */
UCLASS()
class USLComparisonSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override { return bOwned; }
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(USLComparisonSubsystem, STATGROUP_Tickables); }
    virtual void Deinitialize() override;
    bool Execute(const FSLGMToolEntry& Entry, USLGMToolConfig* Config);
    void Restore();
    bool OwnsRenderer() const { return bOwned; }
    bool IsBusy() const { return CaptureState != ECaptureState::Idle; }
    bool CanExecute(ESLComparisonAction Action, FText& Reason) const;
    bool CanExecute(const FSLGMToolEntry& Entry, FText& Reason) const;
    bool IsProfileSelected(FName Id) const { return bOwned && ActiveEntry.Id == Id; }
    bool IsSampleCountControl(const FSLGMToolEntry& Entry) const { return bOwned && !Entry.ConsoleVariable.IsEmpty() && Entry.ConsoleVariable == ActiveEntry.Profile.SamplesVariable; }
    const FSLGMToolEntry& ResolveEntry(const FSLGMToolEntry& Entry) const;
    FText GetStatus() const;
    FText GetButtonLabel(const FSLGMToolEntry& Button) const;
    FString GetOutputDirectory() const { return Output; }
    bool SetLiveComparisonEnabled(bool bEnabled, const FSLGMToolEntry& Entry, USLGMToolConfig* Config);
    bool CanEnableLiveComparison(FText& Reason) const;
    void DisableLiveComparison();
    bool IsLiveComparisonEnabled() const { return bLiveComparison; }
    FSLLiveComparisonChanged OnLiveComparisonChanged;
    float GetComparisonPosition() const { return ComparisonPosition; }
    void SetComparisonPosition(float Position);
    FText GetLiveAlgorithmLabel() const;
    bool IsSVGFPreview() const { return bOwned && ActiveEntry.Profile.Backend == ESLGMRenderBackend::SVGF; }
    bool IsFilterPreview() const { return bOwned && !ActiveEntry.Profile.LabelOptionVariable.IsEmpty(); }
    bool CanSelectPreviewMethod() const { return bOwned; }
    bool SelectPreviewMethod(const FSLGMToolEntry& Entry, int32 Value, FText& Result);
    uint32 GetIndependentCompositeCount() const;
    uint32 GetCompletedSamples() const;
    class UTextureRenderTarget2D* GetRawComparisonTarget() const { return RawTarget; }
private:
    enum class ECaptureState : uint8 { Idle, Accumulating, AwaitingScreenshot, ImageSaved };
    bool IsScreenshotPending() const { return CaptureState == ECaptureState::AwaitingScreenshot; }
    void TickCapture();
    void CancelCapture(const FString& Reason);
    bool ApplySelectedProfile();
    bool ActivateLiveComparison();
    bool RefreshPreviewMethod();
    bool StartRawComparison();
    void StopRawComparison();
    bool Apply();
    bool NeedsIndependentRaw() const;
    int32 ReadSamples(const FSLGMToolEntry& Entry) const;
    bool RefreshSelectedProfile();
    bool StartCapture();
    void StartReference();
    void Fail(const FString& Message);
    void ReleaseCamera();
    void ReleaseGuide();
    void SetVariable(const FString& Name, const FString& Value);
    void RestoreVariables();
    void Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels);
    void WriteReport();
    bool bOwned = false, bPausedBefore = false;
    bool bSavedPathTracing = false, bSavedDLSS = false, bSavedRR = false;
    ECaptureState CaptureState = ECaptureState::Idle;
    bool bLiveComparison = false;
    bool bComparisonPositionInitialized = false;
    float ComparisonPosition = 0.5f;
    TMap<FString, FString> SavedVariables;
    TSharedPtr<class FSLComparisonView, ESPMode::ThreadSafe> View;
    TSharedPtr<class FSLGuideProbe, ESPMode::ThreadSafe> Guide;
    TSharedPtr<class FSLRawComparisonView, ESPMode::ThreadSafe> RawComparison;
    UPROPERTY(Transient) TObjectPtr<class USceneCaptureComponent2D> RawCapture;
    UPROPERTY(Transient) TObjectPtr<class UTextureRenderTarget2D> RawTarget;
    FString Output, Status;
    FIntPoint CaptureSize = FIntPoint::ZeroValue;
    FDelegateHandle ScreenshotHandle;
    int32 ReferenceSamples = 1;
    int32 TargetSamples = 0;
    double StageStarted = 0;
    FString CaptureMetadata;
    FSLGMToolEntry ActiveEntry;
    FSLGMToolEntry PreviewBeforeCapture;
    bool bResumeLiveAfterCapture = false;
    int32 MethodBeforeCapture = 0;
    UPROPERTY(Transient) TObjectPtr<USLGMToolConfig> Configuration;
    UPROPERTY(Transient) TObjectPtr<class ACameraActor> CaptureCamera;
    UPROPERTY(Transient) TObjectPtr<AActor> PreviousCamera;
};
