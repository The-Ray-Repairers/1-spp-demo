#pragma once

#include "SceneViewExtension.h"
#include "ScreenPass.h"

class FTextureRenderTargetResource;

/** A separate 1-SPP view for comparisons whose right side is not the same PT sample. */
class FSLRawComparisonView : public FWorldSceneViewExtension
{
public:
    FSLRawComparisonView(const FAutoRegister& Register, UWorld* World, FTextureRenderTargetResource* Target);
    void SetPosition(float Value) { Position.Store(Value); }
    uint32 GetCompositeCount() const { return CompositeCount.Load(); }
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void PreRenderView_RenderThread(FRDGBuilder&, FSceneView& View) override;
    virtual void PostRenderView_RenderThread(FRDGBuilder&, FSceneView& View) override;
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
        FAfterPassCallbackDelegateArray& Callbacks, bool bEnabled) override;
private:
    FScreenPassTexture Composite(FRDGBuilder& Graph, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
    FTextureRenderTargetResource* Target;
    TAtomic<float> Position{-1.f};
    TAtomic<uint32> CompositeCount{0};
    float RawPreExposure = 1.f;
    bool bLoggedRaw = false;
};
