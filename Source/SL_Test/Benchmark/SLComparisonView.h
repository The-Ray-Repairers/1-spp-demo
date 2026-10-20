#pragma once

#include "SceneViewExtension.h"
#include "SceneView.h"
#include "SceneManagement.h"

/** Main-view accumulation tracking; scene captures keep their own sample state. */
class FSLComparisonView : public FWorldSceneViewExtension
{
public:
    FSLComparisonView(const FAutoRegister& Register, UWorld* World) : FWorldSceneViewExtension(Register, World) {}
    bool bReference = false;
    int32 DenoiserOverride = -1;
    TAtomic<uint32> Samples{0};
    /** Offline renders ignore camera motion, so a moved camera must request a reset explicitly. */
    TAtomic<bool> bResetAccumulation{false};
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView& V) override
    {
        if (V.bIsSceneCapture) return;
        if (bReference) V.bIsOfflineRender = true;
        if (bResetAccumulation.Exchange(false)) V.bForcePathTracerReset = true;
        if (DenoiserOverride >= 0) V.FinalPostProcessSettings.PathTracingEnableDenoiser = DenoiserOverride != 0;
    }
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void PreRenderView_RenderThread(FRDGBuilder&, FSceneView& V) override
    { if (!V.bIsSceneCapture && V.State && V.Family->EngineShowFlags.PathTracing) Samples.Store(V.State->GetPathTracingSampleIndex()); }
};

