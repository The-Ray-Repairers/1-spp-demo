#include "SLGMToolsComponent.h"
#include "SLGMToolConfig.h"
#include "SLGMCommandService.h"
#include "SL_Test.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SViewport.h"
#include "Styling/CoreStyle.h"

namespace
{
	/** Shared runtime palette; opaque fill brushes prevent the scene bleeding through. */
	struct FGMToolsStyle
	{
		FLinearColor PanelBackground = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("171C24FF")));
		FLinearColor RowBackground = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("252D39FF")));
		FTextBlockStyle Body = FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
			.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12))
			.SetColorAndOpacity(FLinearColor::White);

		static const FGMToolsStyle& Get()
		{
			static const FGMToolsStyle Style;
			return Style;
		}
	};

	class SGMRow : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGMRow) {} SLATE_ARGUMENT(FSLGMToolEntry, Entry) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			Entry = Args._Entry;
			FString Current;
			FText Error;
			FSLGMCommandService::Read(Entry, Current, Error);
			TSharedRef<SHorizontalBox> Controls = SNew(SHorizontalBox);
			if (Entry.Control == ESLGMControlType::Toggle)
			{
				Controls->AddSlot().AutoWidth()[SNew(SCheckBox)
					.IsEnabled_Lambda([this] { return CanEdit(); })
					.IsChecked_Lambda([this] {
						FString Value; FText Error;
						FSLGMCommandService::Read(Entry, Value, Error);
						return Value == TEXT("1") ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Apply(State == ECheckBoxState::Checked ? TEXT("1") : TEXT("0")); })];
			}
			else
			{
				Controls->AddSlot().FillWidth(1)[SAssignNew(Input, SEditableTextBox)
					.Font(FGMToolsStyle::Get().Body.Font).ForegroundColor(FLinearColor::White)
					.Text(FText::FromString(Current))
					.IsEnabled_Lambda([this] { return CanEdit(); })
					.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type Type) {
						if (Type == ETextCommit::OnEnter) Apply(Text.ToString());
					})];
				Controls->AddSlot().AutoWidth().Padding(8, 0)[SNew(SButton).Text(FText::FromString(TEXT("Apply")))
					.IsEnabled_Lambda([this] { return CanEdit(); })
					.OnClicked_Lambda([this] { Apply(Input->GetText().ToString()); return FReply::Handled(); })];
			}
			ChildSlot[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().RowBackground).ForegroundColor(FLinearColor::White).Padding(12)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Entry.Label)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)[Controls]
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text_Lambda([this] {
					FString Value; FText Error;
					FSLGMCommandService::Read(Entry, Value, Error);
					return FText::FromString(FString::Printf(TEXT("Current value: %s%s%s"), *Value, Error.IsEmpty() ? TEXT("") : TEXT(" · "), *Error.ToString()));
				}).AutoWrapText(true)]
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text_Lambda([this] { return Status; }).AutoWrapText(true)]
			]];
		}
	private:
		bool CanEdit() const { FString Value; FText Error; return FSLGMCommandService::Read(Entry, Value, Error); }
		void Apply(const FString& Value)
		{
			if (FSLGMCommandService::Apply(Entry, Value, Status) && Input.IsValid())
			{
				FString Actual; FText Error;
				FSLGMCommandService::Read(Entry, Actual, Error);
				Input->SetText(FText::FromString(Actual));
			}
		}
		FSLGMToolEntry Entry;
		FText Status;
		TSharedPtr<SEditableTextBox> Input;
	};

	class SGMBatchRow : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGMBatchRow) {} SLATE_ARGUMENT(FSLGMToolEntry, Entry) SLATE_ARGUMENT(TWeakObjectPtr<APlayerController>, Player) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			Entry = Args._Entry;
			Player = Args._Player;
			ChildSlot[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().RowBackground).Padding(12)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Entry.Label).AutoWrapText(true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(SButton).Text(FText::FromString(TEXT("Run")))
					.OnClicked_Lambda([this]
					{
						// Exec commands can remove the viewport UI. Keep the callback's owner alive.
						const TSharedRef<SGMBatchRow> KeepAlive = SharedThis(this);
						FText Report;
						const bool bSuccess = FSLGMCommandService::ExecuteBatch(Entry, Player.Get(), Report);
						UE_LOG(LogSL_Test, Log, TEXT("GM Tools: %s\n%s"), *Entry.Label.ToString(), *Report.ToString());
						Status = FText::FromString(bSuccess ? TEXT("Commands submitted.") : TEXT("Stopped. See Output Log for details."));
						return FReply::Handled();
					})]
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body)
					.Text_Lambda([this] { return Status; }).AutoWrapText(true)]
			]];
		}
	private:
		FSLGMToolEntry Entry;
		TWeakObjectPtr<APlayerController> Player;
		FText Status;
	};

	class SGMPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGMPanel) {} SLATE_ARGUMENT(USLGMToolConfig*, Configuration) SLATE_ARGUMENT(APlayerController*, Player) SLATE_EVENT(FSimpleDelegate, OnMinimize) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			OnMinimize = Args._OnMinimize;
			TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
			if (Args._Configuration)
			{
				TArray<FString> Groups;
				for (const FSLGMToolEntry& Entry : Args._Configuration->Entries) Groups.AddUnique(Entry.Group.ToString());
				for (const FString& Group : Groups)
				{
					Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(FText::FromString(Group))];
					for (const FSLGMToolEntry& Entry : Args._Configuration->Entries)
					{
						if (Entry.Group.ToString() != Group) continue;
						if (Entry.Control == ESLGMControlType::CommandBatch)
							Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SGMBatchRow).Entry(Entry).Player(Args._Player)];
						else
							Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SGMRow).Entry(Entry)];
					}
				}
			}
			if (!Args._Configuration || Args._Configuration->Entries.IsEmpty())
				Rows->AddSlot().AutoHeight()[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(FText::FromString(TEXT("Assign a configuration asset with at least one entry in Project Settings > Game > GM Tools."))).AutoWrapText(true)];
			ChildSlot.HAlign(HAlign_Right).VAlign(VAlign_Fill).Padding(16)[SNew(SBox).WidthOverride(460)[SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().PanelBackground).ForegroundColor(FLinearColor::White).Padding(16)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(FText::FromString(TEXT("GM Tools | Rendering")))]
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Minimize"))).OnClicked_Lambda([this] { OnMinimize.ExecuteIfBound(); return FReply::Handled(); })]]
				+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[Rows]]
			]]];
		}
		virtual bool SupportsKeyboardFocus() const override { return true; }
	private:
		FSimpleDelegate OnMinimize;
	};
}

void USLGMToolsComponent::TogglePanel()
{
	if (IsPanelOpen()) MinimizePanel();
	else OpenPanel();
}

void USLGMToolsComponent::OpenPanel()
{
	if (IsPanelOpen()) return;
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC || !PC->IsLocalController() || !PC->GetLocalPlayer() || !PC->GetLocalPlayer()->ViewportClient) return;
	ULocalPlayer* Player = PC->GetLocalPlayer();
	if (!Panel.IsValid())
	{
		const USLGMToolSettings* Settings = GetDefault<USLGMToolSettings>();
		Configuration = Settings->Configuration.LoadSynchronous();
		Panel = SNew(SGMPanel).Configuration(Configuration).Player(PC)
			.OnMinimize(FSimpleDelegate::CreateUObject(this, &USLGMToolsComponent::MinimizePanel));
		Player->ViewportClient->AddViewportWidgetForPlayer(Player, Panel.ToSharedRef(), 100);
		// Snapshot once per panel lifetime, not on restore from the minimized state.
		bPreviousCursor = PC->bShowMouseCursor;
	}
	if (MinimizedButton.IsValid())
	{
		Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef());
		MinimizedButton.Reset();
	}
	bMinimized = false;
	Panel->SetVisibility(EVisibility::SelfHitTestInvisible);
	PC->SetIgnoreMoveInput(true);
	PC->SetIgnoreLookInput(true);
	bInputCaptured = true;
	PC->FlushPressedKeys();
	PC->bShowMouseCursor = true;
	FInputModeUIOnly Mode;
	Mode.SetWidgetToFocus(Panel);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
}

void USLGMToolsComponent::MinimizePanel()
{
	if (!IsPanelOpen()) return;
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC || !PC->GetLocalPlayer() || !PC->GetLocalPlayer()->ViewportClient) return;
	ULocalPlayer* Player = PC->GetLocalPlayer();
	// Keep the existing panel so pending edits, status, and scroll position survive.
	Panel->SetVisibility(EVisibility::Collapsed);
	bMinimized = true;
	MinimizedButton = SNew(SBox).HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(16)
		.Visibility(EVisibility::SelfHitTestInvisible)
		[SNew(SButton).ContentPadding(FMargin(12, 8)).IsFocusable(false)
			.ButtonColorAndOpacity(FGMToolsStyle::Get().PanelBackground)
			.OnClicked(FOnClicked::CreateWeakLambda(this, [this]
			{
				OpenPanel();
				return FReply::Handled();
			}))
			[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(FText::FromString(TEXT("GM")))]];
	Player->ViewportClient->AddViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef(), 100);
	if (bInputCaptured)
	{
		PC->SetIgnoreMoveInput(false);
		PC->SetIgnoreLookInput(false);
		bInputCaptured = false;
	}
	PC->FlushPressedKeys();
	// Leave the launcher clickable, while returning keyboard input to the viewport.
	PC->bShowMouseCursor = true;
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(Player->ViewportClient->GetGameViewportWidget());
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
}

void USLGMToolsComponent::ClosePanel()
{
	if (!Panel.IsValid()) return;
	if (APlayerController* PC = Cast<APlayerController>(GetOwner()))
	{
		if (ULocalPlayer* Player = PC->GetLocalPlayer())
		{
			if (Player->ViewportClient)
			{
				Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, Panel.ToSharedRef());
				if (MinimizedButton.IsValid())
					Player->ViewportClient->RemoveViewportWidgetForPlayer(Player, MinimizedButton.ToSharedRef());
			}
		}
		if (bInputCaptured)
		{
			PC->SetIgnoreMoveInput(false);
			PC->SetIgnoreLookInput(false);
		}
		PC->bShowMouseCursor = bPreviousCursor;
		PC->SetInputMode(FInputModeGameOnly());
		PC->FlushPressedKeys();
	}
	bInputCaptured = false;
	bMinimized = false;
	MinimizedButton.Reset();
	Panel.Reset();
	Configuration = nullptr;
}

void USLGMToolsComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	ClosePanel();
	Super::EndPlay(Reason);
}
