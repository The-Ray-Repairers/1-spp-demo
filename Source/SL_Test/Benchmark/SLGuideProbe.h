#pragma once
#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "SceneView.h"

struct FSLGuideFrame;
/** Single-view auxiliary rendering and SVGF integration. No engine patch required. */
class FSLGuideProbe : public FWorldSceneViewExtension
{
public:
    FSLGuideProbe(const FAutoRegister& Register, UWorld* World);
    virtual ~FSLGuideProbe();
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override;
private:
    FSceneViewStateReference GuideViewState;
    TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> Frame;
};
