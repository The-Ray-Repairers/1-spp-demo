#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/DeveloperSettings.h"
#include "SLComparisonSubsystem.generated.h"

UENUM()
enum class ESLComparisonAction : uint8 { Raw, SVGF, RR, Reference };

USTRUCT()
struct FSLComparisonButton
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) ESLComparisonAction Action = ESLComparisonAction::Raw;
    UPROPERTY(EditAnywhere) FText Label;
    UPROPERTY(EditAnywhere) FText ResultLabel;
    UPROPERTY(EditAnywhere) FText Purpose;
    UPROPERTY(EditAnywhere) bool bEndToEnd = false;
    /** Rendering profile path relative to the project directory. */
    UPROPERTY(EditAnywhere) FString Profile;
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Rendering Comparison"))
class USLComparisonSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    virtual FName GetCategoryName() const override { return TEXT("Game"); }
    UPROPERTY(Config, EditAnywhere) TArray<FSLComparisonButton> Buttons;
    UPROPERTY(Config, EditAnywhere, meta=(ClampMin="1")) float TimeoutSeconds = 600.f;
};

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
    bool Execute(ESLComparisonAction Action);
    void Restore();
    bool OwnsRenderer() const { return bOwned; }
    bool IsBusy() const { return bCapturing; }
    bool CanExecute(ESLComparisonAction Action, FText& Reason) const;
    FText GetStatus() const;
    FText GetButtonLabel(const FSLComparisonButton& Button) const;
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
    TSharedPtr<class FJsonObject> Profile;
    FString Output, StageDirectory, Status;
    FIntPoint CaptureSize = FIntPoint::ZeroValue;
    FDelegateHandle ScreenshotHandle;
    int32 ReferenceSamples = 1;
    int32 TargetSamples = 0;
    bool bRequested = false, bImageSaved = false;
    double StageStarted = 0;
    TSharedPtr<FJsonObject> StageResult;
    UPROPERTY(Transient) TObjectPtr<class ACameraActor> CaptureCamera;
    UPROPERTY(Transient) TObjectPtr<AActor> PreviousCamera;
};
