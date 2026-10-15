#include "SLFlythroughPanel.h"
#include "SLFlythroughSubsystem.h"
#include "SLFlythroughMetrics.h"
#include "Debug/GMTools/SLGMToolsStyle.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Styling/CoreStyle.h"
#include "HAL/PlatformProcess.h"

namespace
{

// -----------------------------------------------------------------------
// Inline Bar Chart Widget
// -----------------------------------------------------------------------

/** A simple horizontal bar chart row for metric visualization. */
class SMetricBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMetricBar) {}
		SLATE_ARGUMENT(FString, Label)
		SLATE_ARGUMENT(double, Value)
		SLATE_ARGUMENT(double, MaxValue)
		SLATE_ARGUMENT(FLinearColor, BarColor)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		const float Fraction = Args._MaxValue > 0 ? FMath::Clamp((float)(Args._Value / Args._MaxValue), 0.f, 1.f) : 0.f;
		ChildSlot
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(140)
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(Args._Label))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
			[
				SNew(SBox).HeightOverride(16)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FLinearColor(0.15f, 0.18f, 0.22f))
					[
						SNew(SBox)
						.HAlign(HAlign_Left)
						[
							SNew(SBorder)
							.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
							.BorderBackgroundColor(Args._BarColor)
							.Padding(FMargin(FMath::Max(4.f, Fraction * 200.f), 0, 0, 0))
							[
								SNullWidget::NullWidget
							]
						]
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(SBox).WidthOverride(80)
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(FString::Printf(TEXT("%.2f"), Args._Value)))
				]
			]
		];
	}
};

// -----------------------------------------------------------------------
// Method Selector Button
// -----------------------------------------------------------------------

class SMethodButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMethodButton) {}
		SLATE_ARGUMENT(int32, MethodIndex)
		SLATE_ARGUMENT(TWeakObjectPtr<USLFlythroughSubsystem>, Subsystem)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		MethodIndex = Args._MethodIndex;
		Subsystem = Args._Subsystem;

		const FString& Name = Subsystem.IsValid() ? Subsystem->GetMethods()[MethodIndex].Name : TEXT("???");
		ChildSlot
		[
			SNew(SButton)
			.IsEnabled_Lambda([this] { return Subsystem.IsValid() && !Subsystem->IsBusy(); })
			.ButtonColorAndOpacity_Lambda([this]
			{
				if (!Subsystem.IsValid()) return FLinearColor(0.25f, 0.3f, 0.38f);
				const bool bSelected = Subsystem->GetSelectedMethodIndex() == MethodIndex;
				const bool bHasRun = Subsystem->HasCompletedRun(Subsystem->GetMethods()[MethodIndex].Name);
				if (bSelected)
					return FLinearColor(0.1f, 0.55f, 0.95f, 1.f); // blue = selected
				if (bHasRun)
					return FLinearColor(0.15f, 0.55f, 0.25f, 1.f); // green = has run
				return FLinearColor(0.25f, 0.3f, 0.38f, 1.f); // grey = unrendered
			})
			.ForegroundColor(FLinearColor::White)
			.ContentPadding(FMargin(12, 6))
			.OnClicked_Lambda([this]
			{
				if (Subsystem.IsValid()) Subsystem->SelectMethod(MethodIndex);
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text_Lambda([this]
					{
						if (!Subsystem.IsValid()) return FText::FromString(TEXT("○"));
						return FText::FromString(Subsystem->GetSelectedMethodIndex() == MethodIndex ? TEXT("●") : TEXT("○"));
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(Name))
					.AutoWrapText(true)
				]
			]
		];
	}

private:
	int32 MethodIndex = 0;
	TWeakObjectPtr<USLFlythroughSubsystem> Subsystem;
};

// -----------------------------------------------------------------------
// Chart Section
// -----------------------------------------------------------------------

class SMetricCharts : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMetricCharts) {}
		SLATE_ARGUMENT(TWeakObjectPtr<USLFlythroughSubsystem>, Subsystem)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		Subsystem = Args._Subsystem;
		ChildSlot
		[
			SAssignNew(ChartsBox, SVerticalBox)
		];
		Rebuild();
	}

	void Rebuild()
	{
		if (!ChartsBox.IsValid() || !Subsystem.IsValid()) return;
		ChartsBox->ClearChildren();

		const TArray<FSLMethodSummary>& Summaries = Subsystem->GetSummaries();
		if (Summaries.Num() == 0)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(STextBlock)
				.TextStyle(&FGMToolsStyle::Get().Body)
				.Text(FText::FromString(TEXT("No metrics data. Render methods and compute metrics.")))
			];
			return;
		}

		// Find max values for normalization
		double MaxRenderMs = 1, MaxMSE = 1, MaxPSNR = 1, MaxSSIM = 1, MaxCNR = 1;
		for (const auto& S : Summaries)
		{
			MaxRenderMs = FMath::Max(MaxRenderMs, S.AvgRenderMs);
			MaxMSE = FMath::Max(MaxMSE, S.AvgMSE);
			MaxPSNR = FMath::Max(MaxPSNR, S.AvgPSNR);
			MaxSSIM = FMath::Max(MaxSSIM, S.AvgSSIM);
			MaxCNR = FMath::Max(MaxCNR, FMath::Abs(S.AvgCNR));
		}

		auto AddSection = [this](const FText& Title)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock)
				.TextStyle(&FGMToolsStyle::Get().Body)
				.Text(Title)
				.ColorAndOpacity(FLinearColor(0.6f, 0.8f, 1.0f))
			];
		};

		// Render Time
		AddSection(FText::FromString(TEXT("▸ Avg Render Time (ms) — lower is better")));
		for (const auto& S : Summaries)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SMetricBar).Label(S.Method).Value(S.AvgRenderMs).MaxValue(MaxRenderMs)
				.BarColor(FLinearColor(0.95f, 0.55f, 0.15f))
			];
		}

		// PSNR
		AddSection(FText::FromString(TEXT("▸ Avg PSNR (dB) — higher is better")));
		for (const auto& S : Summaries)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SMetricBar).Label(S.Method).Value(S.AvgPSNR).MaxValue(MaxPSNR)
				.BarColor(FLinearColor(0.3f, 0.75f, 0.4f))
			];
		}

		// SSIM
		AddSection(FText::FromString(TEXT("▸ Avg SSIM — higher is better (max 1.0)")));
		for (const auto& S : Summaries)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SMetricBar).Label(S.Method).Value(S.AvgSSIM).MaxValue(1.0)
				.BarColor(FLinearColor(0.4f, 0.6f, 0.95f))
			];
		}

		// MSE
		AddSection(FText::FromString(TEXT("▸ Avg MSE — lower is better")));
		for (const auto& S : Summaries)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SMetricBar).Label(S.Method).Value(S.AvgMSE).MaxValue(MaxMSE)
				.BarColor(FLinearColor(0.85f, 0.3f, 0.3f))
			];
		}

		// CNR
		AddSection(FText::FromString(TEXT("▸ Avg CNR (ratio) — higher is better")));
		for (const auto& S : Summaries)
		{
			ChartsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SMetricBar).Label(S.Method).Value(S.AvgCNR).MaxValue(MaxCNR)
				.BarColor(FLinearColor(0.7f, 0.4f, 0.9f))
			];
		}
	}

private:
	TWeakObjectPtr<USLFlythroughSubsystem> Subsystem;
	TSharedPtr<SVerticalBox> ChartsBox;
};

// -----------------------------------------------------------------------
// Main Panel
// -----------------------------------------------------------------------

class SFlythroughPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlythroughPanel) {}
		SLATE_ARGUMENT(USLFlythroughSubsystem*, Subsystem)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		Subsystem = Args._Subsystem;

		// Method selector
		if (Subsystem.IsValid()) Subsystem->RefreshSummaries();
		TSharedRef<SVerticalBox> MethodButtons = SNew(SVerticalBox);
		if (Subsystem.IsValid())
		{
			for (int32 i = 0; i < Subsystem->GetMethods().Num(); ++i)
			{
				MethodButtons->AddSlot().AutoHeight().Padding(0, 2)
				[
					SNew(SMethodButton).MethodIndex(i).Subsystem(Subsystem)
				];
			}
		}

		TSharedPtr<SMetricCharts> Charts;

		ChildSlot
		[
			SNew(SVerticalBox)

			// ---- Title ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.TextStyle(&FGMToolsStyle::Get().Body)
				.Text(FText::FromString(TEXT("Flythrough Benchmark")))
				.ColorAndOpacity(FLinearColor(0.6f, 0.8f, 1.0f))
			]

			// ---- Method Selector ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FGMToolsStyle::Get().RowBackground)
				.Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.TextStyle(&FGMToolsStyle::Get().Body)
						.Text(FText::FromString(TEXT("Select Method")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
					[
						MethodButtons
					]
				]
			]

			// ---- Render Flythrough Button ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton)
				.IsEnabled_Lambda([this] { FText R; return Subsystem.IsValid() && Subsystem->CanStartFlythrough(R); })
				.ToolTipText_Lambda([this]
				{
					if (!Subsystem.IsValid()) return FText::GetEmpty();
					FText R;
					Subsystem->CanStartFlythrough(R);
					return R;
				})
				.OnClicked_Lambda([this]
				{
					if (Subsystem.IsValid()) Subsystem->StartFlythrough();
					return FReply::Handled();
				})
				.ContentPadding(FMargin(16, 8))
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(TEXT("▶  Render Flythrough")))
				]
			]

			// ---- Cancel Button ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SButton)
				.IsEnabled_Lambda([this] { return Subsystem.IsValid() && Subsystem->IsBusy(); })
				.OnClicked_Lambda([this]
				{
					if (Subsystem.IsValid()) Subsystem->CancelFlythrough();
					return FReply::Handled();
				})
				.ContentPadding(FMargin(16, 8))
				.ButtonColorAndOpacity(FLinearColor(0.6f, 0.2f, 0.2f))
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(TEXT("✕  Cancel")))
				]
			]

			// ---- Progress Bar ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SProgressBar)
				.Percent_Lambda([this] { return Subsystem.IsValid() ? Subsystem->GetProgress() : 0.f; })
				.FillColorAndOpacity(FLinearColor(0.1f, 0.55f, 0.95f))
				.Visibility_Lambda([this]
				{
					return (Subsystem.IsValid() && Subsystem->IsBusy()) ? EVisibility::Visible : EVisibility::Collapsed;
				})
			]

			// ---- Status Text ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(STextBlock)
				.TextStyle(&FGMToolsStyle::Get().Body)
				.AutoWrapText(true)
				.Text_Lambda([this]
				{
					return Subsystem.IsValid() ? Subsystem->GetStatusText() : FText::GetEmpty();
				})
			]

			// ---- Compute Metrics Button ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(SButton)
				.IsEnabled_Lambda([this] { FText R; return Subsystem.IsValid() && Subsystem->CanComputeMetrics(R); })
				.ToolTipText_Lambda([this]
				{
					if (!Subsystem.IsValid()) return FText::GetEmpty();
					FText R;
					Subsystem->CanComputeMetrics(R);
					return R;
				})
				.OnClicked_Lambda([this]
				{
					if (Subsystem.IsValid())
					{
						Subsystem->ComputeMetrics();
					}
					return FReply::Handled();
				})
				.ContentPadding(FMargin(16, 8))
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(TEXT("📊  Compute Metrics")))
				]
			]

			// ---- Open Results Folder Button ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SButton)
				.OnClicked_Lambda([this]
				{
					if (Subsystem.IsValid())
						FPlatformProcess::ExploreFolder(*Subsystem->GetRendersRoot());
					return FReply::Handled();
				})
				.ContentPadding(FMargin(16, 8))
				[
					SNew(STextBlock)
					.TextStyle(&FGMToolsStyle::Get().Body)
					.Text(FText::FromString(TEXT("📁  Open Results Folder")))
				]
			]

			// ---- Metric Charts ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SAssignNew(Charts, SMetricCharts).Subsystem(Subsystem)
			]
		];

		// Rebuild charts when state changes
		if (Subsystem.IsValid())
		{
			Subsystem->OnStateChanged.AddSPLambda(this, [WeakCharts = TWeakPtr<SMetricCharts>(Charts)]
			{
				if (const auto PinnedCharts = WeakCharts.Pin()) PinnedCharts->Rebuild();
			});
		}
	}

private:
	TWeakObjectPtr<USLFlythroughSubsystem> Subsystem;
};

} // anonymous namespace

// -----------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------

TSharedRef<SWidget> CreateFlythroughPanel(USLFlythroughSubsystem* Subsystem)
{
	return SNew(SFlythroughPanel).Subsystem(Subsystem);
}
