#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Debug/GMTools/SLGMToolConfig.h"
#include "SLComparisonSubsystem.generated.h"

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
    bool IsBusy() const { return bCapturing; }
    bool CanExecute(ESLComparisonAction Action, FText& Reason) const;
    FText GetStatus() const;
    FText GetButtonLabel(const FSLGMToolEntry& Button) const;
    FString GetOutputDirectory() const { return Output; }
private:
    bool Apply(ESLComparisonAction Action);
    bool StartCapture();
    void StartReference();
    void Fail(const FString& Message);
    void ReleaseCamera();
    void ReleaseGuide();
    void SetVariable(const FString& Name, const FString& Value);
    void Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels);
    void WriteReport();
    bool bOwned = false, bPausedBefore = false, bExpectScreenshot = false;
    bool bSavedPathTracing = false, bSavedDLSS = false, bSavedRR = false;
    ESLComparisonAction Active = ESLComparisonAction::Raw;
    bool bCapturing = false;
    TMap<FString, FString> SavedVariables;
    TSharedPtr<class FSLComparisonView, ESPMode::ThreadSafe> View;
    TSharedPtr<class FSLGuideProbe, ESPMode::ThreadSafe> Guide;
    FString Output, Status;
    FIntPoint CaptureSize = FIntPoint::ZeroValue;
    FDelegateHandle ScreenshotHandle;
    int32 ReferenceSamples = 1;
    int32 TargetSamples = 0;
    bool bRequested = false, bImageSaved = false;
    double StageStarted = 0;
    FString CaptureMetadata;
    FSLGMToolEntry ActiveEntry;
    UPROPERTY(Transient) TObjectPtr<USLGMToolConfig> Configuration;
    UPROPERTY(Transient) TObjectPtr<class ACameraActor> CaptureCamera;
    UPROPERTY(Transient) TObjectPtr<AActor> PreviousCamera;
};
