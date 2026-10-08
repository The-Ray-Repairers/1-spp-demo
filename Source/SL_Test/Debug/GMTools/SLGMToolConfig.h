#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "SLGMToolConfig.generated.h"

UENUM(BlueprintType)
enum class ESLGMControlType : uint8
{
	Toggle,
	Integer,
	Float,
	CommandBatch,
	/** A set of mutually-exclusive integer options driven by a single CVar. */
	RadioGroup,
	RenderingAction,
	RenderingStatus
};

UENUM(BlueprintType)
enum class ESLComparisonAction : uint8 { Raw, SVGF, RR, Reference, Restore, OpenResults, ApplyProfile };

/** Renderer adapters; presets and comparison behavior are configured separately. */
UENUM(BlueprintType)
enum class ESLGMRenderBackend : uint8 { PathTracing, SVGF, LumenDLSSRR };

UENUM(BlueprintType)
enum class ESLGMComparisonSource : uint8 { SharedInput, IndependentRaw };

USTRUCT(BlueprintType)
struct FSLGMRenderProfile
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") ESLGMRenderBackend Backend = ESLGMRenderBackend::PathTracing;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") ESLGMComparisonSource ComparisonSource = ESLGMComparisonSource::IndependentRaw;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") bool bAccumulate = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile", meta=(ClampMin="1")) int32 SamplesPerPixel = 1;
    /** Optional GM CVar supplying the live sample count; empty uses SamplesPerPixel. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") FString SamplesVariable;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") bool bSaveImage = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") bool bKeepSelectedAfterCapture = true;
    /** Optional caption. Both label fields accept {Samples} or legacy {0}. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") FText PreviewLabel;
    /** Optional radio-control CVar whose selected option supplies the caption. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") FString LabelOptionVariable;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Profile") int32 PassthroughOptionValue = 0;
};

USTRUCT(BlueprintType)
struct FSLGMRenderSetting
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FString Name;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FString Value;
};

/** One labelled option inside a RadioGroup entry. */
USTRUCT(BlueprintType)
struct FSLGMRadioOption
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Option")
	FText Label;
	/** The integer value to write to ConsoleVariable when this option is selected. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Option")
	int32 Value = 0;
};

/** Describes a console variable; this asset never sets startup values. */
USTRUCT(BlueprintType)
struct FSLGMToolEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
	FName Id;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
	FText Group;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
	FText Label;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="Control != ESLGMControlType::CommandBatch", EditConditionHides))
	FString ConsoleVariable;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
	ESLGMControlType Control = ESLGMControlType::Float;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering", meta=(EditCondition="Control == ESLGMControlType::RenderingAction", EditConditionHides))
    ESLComparisonAction Action = ESLComparisonAction::Raw;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering", meta=(EditCondition="Control == ESLGMControlType::RenderingAction", EditConditionHides))
    FSLGMRenderProfile Profile;
    /** Radio selection first activates this profile; empty leaves the renderer unchanged. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering", meta=(EditCondition="Control == ESLGMControlType::RadioGroup", EditConditionHides))
    FName PreviewProfileId;
    /** Applied in array order; values are snapshotted and restored when leaving comparison. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering", meta=(EditCondition="Control == ESLGMControlType::RenderingAction", EditConditionHides))
    TArray<FSLGMRenderSetting> RenderSettings;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
    bool bEditableDuringPreview = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command")
    FText ApplyLabel;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command") FText ValueFormat;
	/** Each array element is one console command, executed in order on click. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="Control == ESLGMControlType::CommandBatch", EditConditionHides))
	TArray<FString> Commands;
	/** Options displayed as radio buttons when Control == RadioGroup. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="Control == ESLGMControlType::RadioGroup", EditConditionHides))
	TArray<FSLGMRadioOption> RadioOptions;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(MultiLine=true))
	FText Applicability;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="Control != ESLGMControlType::CommandBatch", EditConditionHides))
	bool bLimitRange = true;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="bLimitRange && Control != ESLGMControlType::CommandBatch", EditConditionHides))
	double Minimum = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="bLimitRange && Control != ESLGMControlType::CommandBatch", EditConditionHides))
	double Maximum = 1;
};

/** Presentation only; the slider never changes the denoiser's history or input samples. */
USTRUCT(BlueprintType)
struct FSLLiveComparisonStyle
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FText ToggleLabel = NSLOCTEXT("SLComparison", "Toggle", "Live before / after");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FText BeforeLabel = NSLOCTEXT("SLComparison", "Before", "BEFORE  |  Raw path tracing");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FText AfterLabel = NSLOCTEXT("SLComparison", "After", "AFTER  |  SVGF");
    /** Runtime label; algorithm name comes from the selected configured option. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FText AfterLabelFormat = NSLOCTEXT("SLComparison", "AfterAlgorithm", "AFTER  |  {Algorithm}");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FText Hint = NSLOCTEXT("SLComparison", "Hint", "Drag to compare. Arrow keys adjust; Home / End show either side.");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison", meta=(ClampMin="0", ClampMax="1"))
    float InitialPosition = 0.5f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison", meta=(ClampMin="0.001", ClampMax="1"))
    float KeyboardStep = 0.01f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison", meta=(ClampMin="24"))
    float HandleSize = 48.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison", meta=(ClampMin="1"))
    float LineWidth = 2.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Comparison")
    FLinearColor Accent = FLinearColor(0.46f, 0.78f, 0.12f);
};

UCLASS(BlueprintType)
class SL_TEST_API USLGMToolConfig : public UDataAsset
{
	GENERATED_BODY()
public:
    virtual void PostLoad() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
    const FSLGMToolEntry* FindProfile(FName Id) const;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FName BaselineProfileId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FName DefaultPreviewProfileId;
    UPROPERTY() int32 RenderingProfileVersion = 0;
    /** Optional designer-authored presentation; native UMG layout is the fallback. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering")
    TSubclassOf<class USLLiveComparisonUserWidget> LiveComparisonWidgetClass;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FSLLiveComparisonStyle LiveComparison;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Panel") FText PanelTitle;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Panel") FText MinimizeLabel;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Panel") FText RestorePanelLabel;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering") FString ReferenceSamplesVariable;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Rendering", meta=(ClampMin="1")) float CaptureTimeoutSeconds = 600.f;
	/** Array order defines group order and row order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="GM Tools")
	TArray<FSLGMToolEntry> Entries;
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="GM Tools"))
class SL_TEST_API USLGMToolSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	UPROPERTY(Config, EditAnywhere, Category="Panel")
	TSoftObjectPtr<USLGMToolConfig> Configuration;
};
