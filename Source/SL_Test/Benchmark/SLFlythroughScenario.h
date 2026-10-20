#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SLFlythroughScenario.generated.h"

/** Place one per benchmark level. The sequence may animate only the designated camera. */
UCLASS(Blueprintable)
class ASLFlythroughScenario : public AActor
{
	GENERATED_BODY()

public:
	ASLFlythroughScenario();

	/** Stable folder name shared by all methods captured in this scene. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Flythrough")
	FName ScenarioId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Flythrough")
	TObjectPtr<class ULevelSequence> Sequence;

	/** A placed CameraActor or CineCameraActor. Keep it unanimated outside the sequence. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Flythrough")
	TObjectPtr<class ACameraActor> Camera;

	/** Tag the camera actor binding with this name in the Level Sequence. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Flythrough")
	FName CameraBindingTag = TEXT("BenchmarkCamera");
};
