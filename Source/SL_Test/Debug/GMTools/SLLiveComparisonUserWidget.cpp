#include "SLLiveComparisonUserWidget.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "SceneView.h"
#include "UnrealClient.h"

void USLLiveComparisonUserWidget::BuildDefaultLayout(UWidgetTree* Tree)
{
    if (!Tree || Tree->RootWidget) return;
    auto* Canvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ComparisonCanvas"));
    Tree->RootWidget = Canvas;
    Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    auto* Line = Tree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Divider"));
    Line->SetVisibility(ESlateVisibility::HitTestInvisible);
    auto* LineSlot = Canvas->AddChildToCanvas(Line);
    LineSlot->SetAnchors(FAnchors(0.5f, 0, 0.5f, 1));
    LineSlot->SetOffsets(FMargin(-1, 0, 2, 0));
    auto* Handle = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DragHandle"));
    Handle->SetCursor(EMouseCursor::ResizeLeftRight);
    FSlateBrush Brush;
    Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
    Brush.OutlineSettings.CornerRadii = FVector4(24, 24, 24, 24);
    Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
    Handle->SetBrush(Brush);
    Handle->SetHorizontalAlignment(HAlign_Center);
    Handle->SetVerticalAlignment(VAlign_Center);
    auto* HandleSlot = Canvas->AddChildToCanvas(Handle);
    HandleSlot->SetAnchors(FAnchors(0.5f, 0.5f));
    HandleSlot->SetAlignment(FVector2D(0.5f, 0.5f));
    HandleSlot->SetSize(FVector2D(48, 48));
    HandleSlot->SetZOrder(1);
    auto* Glyph = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HandleGlyph"));
    Glyph->SetText(FText::FromString(TEXT("\u2039  \u203a")));
    Glyph->SetVisibility(ESlateVisibility::HitTestInvisible);
    Handle->AddChild(Glyph);
    for (const FName Name : {FName(TEXT("BeforeText")), FName(TEXT("AfterText"))})
    {
        auto* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
        Label->SetVisibility(ESlateVisibility::HitTestInvisible);
        Label->SetClipping(EWidgetClipping::ClipToBounds);
        Label->SetShadowOffset(FVector2D(1, 1));
        Label->SetShadowColorAndOpacity(FLinearColor::Black);
        auto* Slot = Canvas->AddChildToCanvas(Label);
        const bool bBefore = Name == TEXT("BeforeText");
        Slot->SetAnchors(FAnchors(bBefore ? 0.f : 0.5f, 1));
        Slot->SetOffsets(FMargin(12, -48, 310, 36));
    }
}

TSharedRef<SWidget> USLLiveComparisonUserWidget::RebuildWidget()
{
    BuildDefaultLayout(WidgetTree);
    // Bind the same contract for the native fallback and Blueprint-created trees.
    ComparisonCanvas = Cast<UCanvasPanel>(WidgetTree->FindWidget(TEXT("ComparisonCanvas")));
    Divider = Cast<UImage>(WidgetTree->FindWidget(TEXT("Divider")));
    DragHandle = Cast<UBorder>(WidgetTree->FindWidget(TEXT("DragHandle")));
    BeforeText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("BeforeText")));
    AfterText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("AfterText")));
    return Super::RebuildWidget();
}

void USLLiveComparisonUserWidget::NativePreConstruct()
{
    SetIsFocusable(true);
    if (Divider) Divider->SetColorAndOpacity(Style.Accent);
    if (DragHandle) { DragHandle->SetBrushColor(Style.Accent); DragHandle->SetToolTipText(Style.Hint); }
    if (BeforeText) BeforeText->SetText(Style.BeforeLabel);
    if (AfterText) { AfterText->SetText(Style.AfterLabel); AfterText->SetColorAndOpacity(Style.Accent); }
    // Blueprint PreConstruct can customize the final presentation.
    Super::NativePreConstruct();
}

void USLLiveComparisonUserWidget::NativeConstruct()
{
    Super::NativeConstruct();
    Comparison = GetWorld()->GetSubsystem<USLComparisonSubsystem>();
    if (Comparison.IsValid())
    {
        Comparison->OnLiveComparisonChanged.Remove(StateHandle);
        StateHandle = Comparison->OnLiveComparisonChanged.AddUObject(this, &ThisClass::RefreshVisibility);
    }
    RefreshVisibility();
}

void USLLiveComparisonUserWidget::NativeDestruct()
{
    EndDrag();
    if (Comparison.IsValid()) Comparison->OnLiveComparisonChanged.Remove(StateHandle);
    StateHandle.Reset();
    Comparison.Reset();
    Super::NativeDestruct();
}

void USLLiveComparisonUserWidget::RefreshVisibility()
{
    const bool bActive = Comparison.IsValid() && Comparison->IsLiveComparisonEnabled();
    if (!bActive)
    {
        EndDrag();
        if (GetCachedWidget().IsValid() && GetCachedWidget()->HasMouseCapture())
            FSlateApplication::Get().ReleaseAllPointerCapture();
    }
    SetVisibility(bActive ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    if (bActive) RefreshLabels();
}

float USLLiveComparisonUserWidget::GetComparisonPosition() const
{
    return Comparison.IsValid() ? Comparison->GetComparisonPosition() : Style.InitialPosition;
}

void USLLiveComparisonUserWidget::SetComparisonPosition(float Position)
{
    if (Comparison.IsValid()) Comparison->SetComparisonPosition(Position);
}

FBox2D USLLiveComparisonUserWidget::ImageBounds() const
{
    if (const ULocalPlayer* Local = GetOwningLocalPlayer())
        if (Local->ViewportClient && Local->ViewportClient->Viewport)
        {
            FViewport* Viewport = Local->ViewportClient->Viewport;
            FSceneViewProjectionData Projection;
            const FIntPoint Size = Viewport->GetSizeXY();
            if (Size.GetMin() > 0 && Local->GetProjectionData(Viewport, Projection))
            {
                const FIntRect Rect = Projection.GetConstrainedViewRect();
                return FBox2D(FVector2D(double(Rect.Min.X) / Size.X, double(Rect.Min.Y) / Size.Y),
                    FVector2D(double(Rect.Max.X) / Size.X, double(Rect.Max.Y) / Size.Y));
            }
        }
    return FBox2D(FVector2D::ZeroVector, FVector2D(1, 1));
}

void USLLiveComparisonUserWidget::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
    Super::NativeTick(Geometry, DeltaTime);
    RefreshLabels();
    UpdateLayout(Geometry);
}

void USLLiveComparisonUserWidget::RefreshLabels()
{
    if (AfterText && Comparison.IsValid() && Comparison->IsLiveComparisonEnabled())
    {
        FFormatNamedArguments Arguments;
        Arguments.Add(TEXT("Algorithm"), Comparison->GetLiveAlgorithmLabel());
        const FText Label = FText::Format(Style.AfterLabelFormat, Arguments);
        if (!AfterText->GetText().EqualTo(Label)) AfterText->SetText(Label);
    }
}

void USLLiveComparisonUserWidget::UpdateLayout(const FGeometry& Geometry)
{
    const FBox2D Bounds = ImageBounds();
    const FVector2D Min = Bounds.Min * Geometry.GetLocalSize();
    const FVector2D Max = Bounds.Max * Geometry.GetLocalSize();
    const float X = FMath::Lerp(Min.X, Max.X, GetComparisonPosition());
    auto Layout = [](UWidget* Widget, FVector2D Origin, FVector2D Size) {
        if (Widget) if (auto* Slot = Cast<UCanvasPanelSlot>(Widget->Slot))
        {
            Slot->SetAnchors(FAnchors(0, 0));
            Slot->SetAlignment(FVector2D::ZeroVector);
            Slot->SetPosition(Origin);
            Slot->SetSize(Size);
        }
    };
    const float LineWidth = FMath::Max(1.f, Style.LineWidth);
    Layout(Divider, FVector2D(FMath::Clamp(X - LineWidth / 2, float(Min.X), float(FMath::Max(Min.X, Max.X - LineWidth))), Min.Y),
        FVector2D(LineWidth, Max.Y - Min.Y));
    const float HandleSize = FMath::Max(24.f, Style.HandleSize);
    const float Half = FMath::Min(HandleSize / 2, float((Max.X - Min.X) / 2));
    Layout(DragHandle, FVector2D(FMath::Clamp(X, float(Min.X) + Half, float(Max.X) - Half) - HandleSize / 2,
        (Min.Y + Max.Y - HandleSize) / 2), FVector2D(HandleSize));
    Layout(BeforeText, FVector2D(Min.X + 12, Max.Y - 48), FVector2D(FMath::Max(0.f, X - float(Min.X) - 24), 36));
    Layout(AfterText, FVector2D(X + 12, Max.Y - 48), FVector2D(FMath::Max(0.f, float(Max.X) - X - 24), 36));
}

FReply USLLiveComparisonUserWidget::BeginDrag(const FPointerEvent& Event)
{
    if (bDragging || !Comparison.IsValid() || !Comparison->IsLiveComparisonEnabled() || !DragHandle ||
        !DragHandle->GetCachedGeometry().IsUnderLocation(Event.GetScreenSpacePosition())) return FReply::Unhandled();
    const FGeometry& Geometry = GetCachedGeometry();
    const FBox2D Bounds = ImageBounds();
    DragOffset = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()).X -
        (Bounds.Min.X + GetComparisonPosition() * Bounds.GetSize().X) * Geometry.GetLocalSize().X;
    bDragging = true;
    PointerIndex = Event.GetPointerIndex();
    DragPlayer = GetOwningPlayer();
    if (DragPlayer.IsValid()) DragPlayer->SetIgnoreLookInput(true);
    return FReply::Handled().CaptureMouse(TakeWidget()).SetUserFocus(TakeWidget(), EFocusCause::Mouse);
}

void USLLiveComparisonUserWidget::EndDrag()
{
    if (bDragging && DragPlayer.IsValid()) DragPlayer->SetIgnoreLookInput(false);
    bDragging = false;
    DragPlayer.Reset();
}

FReply USLLiveComparisonUserWidget::NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    return Event.GetEffectingButton() == EKeys::LeftMouseButton ? BeginDrag(Event) : Super::NativeOnMouseButtonDown(Geometry, Event);
}
FReply USLLiveComparisonUserWidget::NativeOnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!bDragging || Event.GetPointerIndex() != PointerIndex) return Super::NativeOnMouseMove(Geometry, Event);
    const FBox2D Bounds = ImageBounds();
    const double Width = Bounds.GetSize().X * Geometry.GetLocalSize().X;
    if (Width > 0) SetComparisonPosition((Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()).X -
        Bounds.Min.X * Geometry.GetLocalSize().X - DragOffset) / Width);
    return FReply::Handled();
}
FReply USLLiveComparisonUserWidget::NativeOnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!bDragging || Event.GetEffectingButton() != EKeys::LeftMouseButton) return Super::NativeOnMouseButtonUp(Geometry, Event);
    EndDrag();
    return FReply::Handled().ReleaseMouseCapture();
}
FReply USLLiveComparisonUserWidget::NativeOnTouchStarted(const FGeometry&, const FPointerEvent& Event) { return BeginDrag(Event); }
FReply USLLiveComparisonUserWidget::NativeOnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) { return NativeOnMouseMove(Geometry, Event); }
FReply USLLiveComparisonUserWidget::NativeOnTouchEnded(const FGeometry&, const FPointerEvent& Event)
{
    if (!bDragging || Event.GetPointerIndex() != PointerIndex) return FReply::Unhandled();
    EndDrag();
    return FReply::Handled().ReleaseMouseCapture();
}
void USLLiveComparisonUserWidget::NativeOnMouseCaptureLost(const FCaptureLostEvent& Event) { EndDrag(); Super::NativeOnMouseCaptureLost(Event); }
FReply USLLiveComparisonUserWidget::NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    float Position = GetComparisonPosition();
    const FKey Key = Event.GetKey();
    if (Key == EKeys::Left) Position -= Style.KeyboardStep;
    else if (Key == EKeys::Right) Position += Style.KeyboardStep;
    else if (Key == EKeys::Home) Position = 0;
    else if (Key == EKeys::End) Position = 1;
    else return Super::NativeOnKeyDown(Geometry, Event);
    SetComparisonPosition(Position);
    return FReply::Handled();
}
