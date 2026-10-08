#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SLGMToolsComponent.generated.h"

class USLGMToolConfig;
class SWidget;

/** Owns the panel lifetime and the local player's temporary UI input capture. */
UCLASS(BlueprintType, meta=(BlueprintSpawnableComponent))
class SL_TEST_API USLGMToolsComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="GM Tools")
	void OpenPanel();
	UFUNCTION(BlueprintCallable, Category="GM Tools")
	void TogglePanel();
	UFUNCTION(BlueprintCallable, Category="GM Tools")
	void MinimizePanel();
	UFUNCTION(BlueprintCallable, Category="GM Tools")
	void ClosePanel();
	UFUNCTION(BlueprintPure, Category="GM Tools")
	bool IsPanelOpen() const { return Panel.IsValid() && !bMinimized; }
	UFUNCTION(BlueprintPure, Category="GM Tools")
	bool IsPanelMinimized() const { return bMinimized; }
protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	UPROPERTY(Transient)
	TObjectPtr<USLGMToolConfig> Configuration;
	TSharedPtr<SWidget> Panel;
	TSharedPtr<SWidget> MinimizedButton;
	UPROPERTY(Transient)
	TObjectPtr<class USLLiveComparisonUserWidget> LiveComparison;
	bool bPreviousCursor = false;
	bool bMinimized = false;
	bool bInputCaptured = false;
};
