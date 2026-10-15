#include "SLGMPanel.h"
#include "SLGMToolsStyle.h"
#include "SLGMToolConfig.h"
#include "SLGMCommandService.h"
#include "SL_Test.h"
#include "Benchmark/SLComparisonSubsystem.h"
#include "Benchmark/SLFlythroughSubsystem.h"
#include "Benchmark/SLFlythroughPanel.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
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
				Controls->AddSlot().AutoWidth().Padding(8, 0)[SNew(SButton).Text(Entry.ApplyLabel)
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
					return FText::Format(Entry.ValueFormat, FText::FromString(Value), Error);
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
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(SButton).Text(Entry.ApplyLabel)
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

	class SGMRadioGroupRow : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGMRadioGroupRow) {} SLATE_ARGUMENT(FSLGMToolEntry, Entry) SLATE_ARGUMENT(TWeakObjectPtr<USLComparisonSubsystem>, Comparison) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			Entry = Args._Entry;
            Comparison = Args._Comparison;
			TSharedRef<SVerticalBox> ButtonList = SNew(SVerticalBox);
			for (const FSLGMRadioOption& Opt : Entry.RadioOptions)
			{
				const int32 OptionValue = Opt.Value;
				const FText OptionLabel = Opt.Label;
				ButtonList->AddSlot().AutoHeight().Padding(0, 2)
				[
					SNew(SButton)
					.IsEnabled_Lambda([this] { return CanEdit(); })
					.ButtonColorAndOpacity_Lambda([this, OptionValue] {
						return IsActive(OptionValue)
							? FLinearColor(0.1f, 0.55f, 0.95f, 1.f)   // active: blue
							: FLinearColor(0.25f, 0.3f, 0.38f, 1.f); // inactive: grey
					})
					.ForegroundColor(FLinearColor::White)
					.ContentPadding(FMargin(12, 6))
					.OnClicked_Lambda([this, OptionValue] {
						ApplyOption(OptionValue);
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
						[
							SNew(STextBlock)
							.TextStyle(&FGMToolsStyle::Get().Body)
							.Text_Lambda([this, OptionValue] {
								return FText::FromString(IsActive(OptionValue) ? TEXT("●") : TEXT("○"));
							})
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.TextStyle(&FGMToolsStyle::Get().Body)
							.Text(OptionLabel)
							.AutoWrapText(true)
						]
					]
				];
			}
			ChildSlot
			[
				SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().RowBackground).ForegroundColor(FLinearColor::White).Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Entry.Label).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
					[
						ButtonList
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body)
						.Text_Lambda([this] { return Status; }).AutoWrapText(true)
					]
				]
			];
		}
	private:
		bool CanEdit() const { FString Value; FText Error; return FSLGMCommandService::Read(Entry, Value, Error); }
		bool IsActive(int32 OptionValue) const
		{
            if (!Entry.PreviewProfileId.IsNone() && Comparison.IsValid() && Comparison->OwnsRenderer() && !Comparison->IsProfileSelected(Entry.PreviewProfileId)) return false;
			FString Value; FText Error;
			if (!FSLGMCommandService::Read(Entry, Value, Error)) return false;
			int32 Current = 0;
			return LexTryParseString(Current, *Value) && Current == OptionValue;
		}
		void ApplyOption(int32 OptionValue)
		{
            if (!Entry.PreviewProfileId.IsNone() && Comparison.IsValid() && Comparison->CanSelectPreviewMethod())
            {
                Comparison->SelectPreviewMethod(Entry, OptionValue, Status);
                return;
            }
			FSLGMCommandService::Apply(Entry, FString::FromInt(OptionValue), Status);
		}
		TWeakObjectPtr<USLComparisonSubsystem> Comparison;
		FSLGMToolEntry Entry;
		FText Status;
	};

	class SGMPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGMPanel) {} SLATE_ARGUMENT(USLGMToolConfig*, Configuration) SLATE_ARGUMENT(APlayerController*, Player) SLATE_EVENT(FSimpleDelegate, OnMinimize) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			OnMinimize = Args._OnMinimize;
            TWeakObjectPtr<USLComparisonSubsystem> Comparison = Args._Player->GetWorld()->GetSubsystem<USLComparisonSubsystem>();
            TWeakObjectPtr<USLFlythroughSubsystem> FlythroughSubsystem = Args._Player->GetWorld()->GetSubsystem<USLFlythroughSubsystem>();

			TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
            TWeakObjectPtr<USLGMToolConfig> Config = Args._Configuration;
            if (Config.IsValid())
            {
                FString PreviousGroup;
                for (const FSLGMToolEntry& Entry : Config->Entries)
                {
                    const FString Group = Entry.Group.ToString();
                    if (Group != PreviousGroup && !Group.IsEmpty())
                        Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Entry.Group)];
                    PreviousGroup = Group;
                    if (Entry.Control == ESLGMControlType::RenderingAction)
                    {
                        Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SButton)
                            .IsEnabled_Lambda([Comparison, Entry] { FText Reason; return Comparison.IsValid() && Comparison->CanExecute(Entry, Reason); })
                            .ToolTipText_Lambda([Comparison, Entry] { FText Reason; if (Comparison.IsValid()) Comparison->CanExecute(Entry, Reason); return Reason.IsEmpty() ? Entry.Applicability : Reason; })
                            .OnClicked_Lambda([Comparison, Config, Entry] { if (Comparison.IsValid() && Config.IsValid()) Comparison->Execute(Entry, Config.Get()); return FReply::Handled(); })
                            [SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).AutoWrapText(true)
                                .Text_Lambda([Comparison, Entry] { return Comparison.IsValid() ? Comparison->GetButtonLabel(Entry) : Entry.Label; })]];
                        if (Entry.Id == Config->BaselineProfileId)
                            Rows->AddSlot().AutoHeight().Padding(8, 4)[SNew(SCheckBox)
                                .IsEnabled_Lambda([Comparison] { FText Reason; return Comparison.IsValid() && (Comparison->IsLiveComparisonEnabled() || Comparison->CanEnableLiveComparison(Reason)); })
                                .IsChecked_Lambda([Comparison] { return Comparison.IsValid() && Comparison->IsLiveComparisonEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                .OnCheckStateChanged_Lambda([Comparison, Config, Entry](ECheckBoxState State) {
                                    if (Comparison.IsValid() && Config.IsValid()) Comparison->SetLiveComparisonEnabled(State == ECheckBoxState::Checked, Entry, Config.Get());
                                }).ToolTipText_Lambda([Comparison, Config] {
                                    FText Reason;
                                    if (Comparison.IsValid()) Comparison->CanEnableLiveComparison(Reason);
                                    return Reason.IsEmpty() && Config.IsValid() ? Config->LiveComparison.Hint : Reason;
                                })
                                [SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Config->LiveComparison.ToggleLabel)]];
                    }
                    else if (Entry.Control == ESLGMControlType::RenderingStatus)
                        Rows->AddSlot().AutoHeight().Padding(0, 6)[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).AutoWrapText(true)
                            .Text_Lambda([Comparison, Entry] { const FText Status = Comparison.IsValid() ? Comparison->GetStatus() : FText::GetEmpty(); return Status.IsEmpty() ? Entry.Label : Status; })];
                    else
                    {
                        TSharedPtr<SWidget> Row;
                        if (Entry.Control == ESLGMControlType::CommandBatch) Row = SNew(SGMBatchRow).Entry(Entry).Player(Args._Player);
						else if (Entry.Control == ESLGMControlType::RadioGroup) Row = SNew(SGMRadioGroupRow).Entry(Entry).Comparison(Comparison);
                        else Row = SNew(SGMRow).Entry(Entry);
                        Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SBox)
                            .IsEnabled_Lambda([Comparison, Entry, FlythroughSubsystem] {
                                if (FlythroughSubsystem.IsValid() && FlythroughSubsystem->IsBusy()) return false;
                                return !Comparison.IsValid() || Comparison->IsSampleCountControl(Entry) ||
                                (Entry.Control == ESLGMControlType::RadioGroup && !Entry.PreviewProfileId.IsNone() && Comparison->CanSelectPreviewMethod()) ||
                                (!Comparison->IsBusy() && (!Comparison->OwnsRenderer() || Entry.bEditableDuringPreview)); })[Row.ToSharedRef()]];
                    }
                }
            }

            // ---- Flythrough Benchmark Section ----
            if (USLFlythroughSubsystem* Flythrough = Args._Player->GetWorld()->GetSubsystem<USLFlythroughSubsystem>())
            {
                Rows->AddSlot().AutoHeight().Padding(0, 16, 0, 4)
                [
                    CreateFlythroughPanel(Flythrough)
                ];
            }

			ChildSlot.HAlign(HAlign_Right).VAlign(VAlign_Fill).Padding(16)[SNew(SBox).WidthOverride(460)[SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().PanelBackground).ForegroundColor(FLinearColor::White).Padding(16)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).TextStyle(&FGMToolsStyle::Get().Body).Text(Config.IsValid() ? Config->PanelTitle : FText::GetEmpty())]
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(Config.IsValid() ? Config->MinimizeLabel : FText::GetEmpty()).OnClicked_Lambda([this] { OnMinimize.ExecuteIfBound(); return FReply::Handled(); })]]
				+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[Rows]]
			]]];
		}
		virtual bool SupportsKeyboardFocus() const override { return true; }
	private:
		FSimpleDelegate OnMinimize;
	};
}

TSharedRef<SWidget> CreateGMPanel(USLGMToolConfig* Configuration, APlayerController* Player, FSimpleDelegate OnMinimize)
{
    return SNew(SGMPanel).Configuration(Configuration).Player(Player).OnMinimize(OnMinimize);
}
