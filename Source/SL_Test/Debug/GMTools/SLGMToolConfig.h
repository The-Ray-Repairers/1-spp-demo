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
	CommandBatch
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
	/** Each array element is one console command, executed in order on click. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Command", meta=(EditCondition="Control == ESLGMControlType::CommandBatch", EditConditionHides))
	TArray<FString> Commands;
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
