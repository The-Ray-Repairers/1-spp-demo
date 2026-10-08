#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "SLGMToolConfig.h"
#include "SLLiveComparisonUserWidget.generated.h"

class UCanvasPanel;
class UBorder;
class UImage;
class UTextBlock;
class USLComparisonSubsystem;

/** UMG presentation for the live GPU wipe; Blueprint children own the visual hierarchy. */
UCLASS(Blueprintable)
class SL_TEST_API USLLiveComparisonUserWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Comparison", meta=(ExposeOnSpawn=true))
    FSLLiveComparisonStyle Style;

    UFUNCTION(BlueprintCallable, Category="Comparison") void SetComparisonPosition(float Position);
    UFUNCTION(BlueprintPure, Category="Comparison") float GetComparisonPosition() const;

    /** Native fallback, also used by the editor authoring command to seed an editable WBP. */
    static void BuildDefaultLayout(UWidgetTree* Tree);
protected:
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget), Category="Comparison") TObjectPtr<UCanvasPanel> ComparisonCanvas;
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget), Category="Comparison") TObjectPtr<UImage> Divider;
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget), Category="Comparison") TObjectPtr<UBorder> DragHandle;
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget), Category="Comparison") TObjectPtr<UTextBlock> BeforeText;
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget), Category="Comparison") TObjectPtr<UTextBlock> AfterText;

    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativePreConstruct() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;
    virtual FReply NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply NativeOnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply NativeOnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply NativeOnTouchStarted(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply NativeOnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply NativeOnTouchEnded(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& Event) override;
    virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
    void RefreshVisibility();
    void RefreshLabels();
    void UpdateLayout(const FGeometry& Geometry);
    FBox2D ImageBounds() const;
    FReply BeginDrag(const FPointerEvent& Event);
    void EndDrag();
    TWeakObjectPtr<USLComparisonSubsystem> Comparison;
    TWeakObjectPtr<APlayerController> DragPlayer;
    FDelegateHandle StateHandle;
    bool bDragging = false;
    uint32 PointerIndex = 0;
    double DragOffset = 0;
};
