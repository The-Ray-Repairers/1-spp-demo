#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SLGMToolsComponent.generated.h"

class APlayerController;
class USLGMToolConfig;
class SWidget;

/**
 * Owns the panel lifetime and the local player's temporary UI input capture.
 *
 * No setup is required: the `gm` console command attaches one to the local
 * player controller on first use. Add it to a player controller yourself only
 * to drive the panel from Blueprint or C++.
 */
UCLASS(BlueprintType, meta=(BlueprintSpawnableComponent))
class RT_DEMO_API USLGMToolsComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	/** Returns the controller's GM tools component, creating a transient one if it has none. */
	UFUNCTION(BlueprintCallable, Category="GM Tools")
	static USLGMToolsComponent* GetOrCreate(APlayerController* PlayerController);
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
