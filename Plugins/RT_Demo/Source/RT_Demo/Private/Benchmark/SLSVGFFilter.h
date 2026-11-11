#pragma once
#include "CoreMinimal.h"
#include "PooledRenderTarget.h"
#include "RenderGraphFwd.h"
class FSceneView;

/** Falcor SVGF filter with UE raster input adaptation; history is independent of PT resets. */
class FSLSVGFFilter
{
public:
    FRDGTextureRef AddPasses(FRDGBuilder& Graph, const FSceneView& View, FRDGTextureRef Color,
        FRDGTextureRef NormalDepth, FRDGTextureRef Diffuse, FRDGTextureRef Emission, FRDGTextureRef Motion);
private:
    TRefCountPtr<IPooledRenderTarget> Illumination, Moments, HistoryLength, PackedGuide;
    FIntPoint Size = FIntPoint::ZeroValue;
    uint32 PreviousFrame = MAX_uint32;
    TArray<float> PreviousSettings;
};
