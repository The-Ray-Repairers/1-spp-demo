#pragma once

#include "CoreMinimal.h"

/** Authored camera route for the default benchmark scene (Unreal units = cm). */
namespace SLFlythroughCameraPath
{
	inline bool IsRoomMap(const FString& MapName)
	{
		// GetMapName includes a UEDPIE_<n>_ prefix during play in editor.
		return MapName == TEXT("LVL_RT_Room")
			|| (MapName.StartsWith(TEXT("UEDPIE_")) && MapName.EndsWith(TEXT("_LVL_RT_Room")));
	}

	constexpr float RoomFieldOfView = 80.0f;

	inline FTransform EvaluateRoom(double NormalizedTime)
	{
		// Stay in the clear aisle in front of the sphere/cylinder. The room's
		// inner walls are x = +/-700, y = -690/700; the floor is z = 50.
		// This loop keeps at least 100 cm from walls and 225 cm above the floor.
		const double Angle = NormalizedTime * 2.0 * PI;
		const FVector Position(-150.0 + 450.0 * FMath::Sin(Angle),
			200.0 + 200.0 * FMath::Cos(Angle), 300.0 + 25.0 * FMath::Sin(Angle));
		const FVector Subject(-225.0, -450.0, 220.0);
		return FTransform((Subject - Position).Rotation(), Position);
	}
}
