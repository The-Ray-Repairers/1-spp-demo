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
enum class ESLComparisonAction : uint8 { Raw, SVGF, RR, Reference, Restore, OpenResults };

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

UCLASS(BlueprintType)
class SL_TEST_API USLGMToolConfig : public UDataAsset
{
	GENERATED_BODY()
public:
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
