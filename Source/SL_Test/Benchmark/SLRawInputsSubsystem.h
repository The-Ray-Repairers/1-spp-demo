#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SLRawInputsSubsystem.generated.h"

/** One-shot diagnostic viewport capture, independent of the benchmark method registry. */
UCLASS()
class USLRawInputsSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    bool Start();
    void Cancel();
    bool IsActive() const { return bActive; }
    virtual bool IsTickable() const override { return bActive; }
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual TStatId GetStatId() const override;
    virtual void Tick(float DeltaTime) override;
    virtual void Deinitialize() override;
private:
    void Finish(bool bResumeComparison);
    void Screenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels);
    bool bActive = false, bPending = false, bPaused = false;
    bool bPathTracing = false, bDLSS = false, bRR = false, bLive = false;
    double Started = 0;
    FIntPoint Size = FIntPoint::ZeroValue;
    FString Folder;
    TMap<FString, FString> Saved;
    FDelegateHandle ScreenshotHandle;
    UPROPERTY(Transient) TObjectPtr<class ACameraActor> Camera;
    UPROPERTY(Transient) TObjectPtr<class AActor> PreviousCamera;
    TSharedPtr<class FSLRawInputsView, ESPMode::ThreadSafe> View;
};
