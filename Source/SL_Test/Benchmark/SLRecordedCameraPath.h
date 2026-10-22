#pragma once

#include "CoreMinimal.h"

/** Runtime data, independent of pawns, editor assets, and scene geometry. */
struct FSLRecordedCameraSample
{
	FVector Position = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	float FOV = 90.f;
	float AspectRatio = 16.f / 9.f;
	bool bConstrainAspectRatio = false;
};

struct FSLRecordedCameraPath
{
	FString Id;
	FString Name;
	FString Map;
	int32 FPS = 30;
	TArray<FSLRecordedCameraSample> Frames;

	bool Save(const FString& Filename) const;
	bool Load(const FString& Filename);
	static FSLRecordedCameraSample Interpolate(const FSLRecordedCameraSample& A,
		const FSLRecordedCameraSample& B, double Alpha);
};
