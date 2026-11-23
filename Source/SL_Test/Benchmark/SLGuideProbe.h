#pragma once
#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "SceneView.h"

struct FSLGuideFrame;
class FRDGBuilder;
class FRDGTexture;

/** First-hit guides from the SLGuides depth + base pass. All are view-rect sized with their origin at (0, 0). */
struct FSLGuideTextures
{
    FRDGTexture* NormalDepth = nullptr;   // xyz world-space shading normal, w scene depth (cm)
    FRDGTexture* Diffuse = nullptr;       // rgb diffuse colour = base colour * (1 - metallic), a shading model id
    FRDGTexture* Motion = nullptr;        // xy previous minus current UV, z 1 where the base pass wrote velocity
    FRDGTexture* BaseColor = nullptr;     // rgb base colour, a metallic
};

/**
 * Renders the main view's depth and base pass once more each frame, as a custom render pass, and exports
 * first-hit guides from it. The path tracer skips the base pass, so this is how path traced frames get
 * G-Buffer data. Used by the Ray Repair neural denoisers (SL.Denoiser.Method 5-7); SVGF uses FSLGuideProbe.
 */
class FSLGuideCapture
{
public:
    explicit FSLGuideCapture(ERHIFeatureLevel::Type FeatureLevel);
    ~FSLGuideCapture();
    /** Game thread, from BeginRenderViewFamily: render the guides for this family's single path traced view. */
    void AddToViewFamily(FSceneViewFamily& Family);
    /** Render thread: the guides rendered earlier in this graph for this frame, or false. */
    bool GetTextures(const FRDGBuilder& GraphBuilder, uint32 FrameNumber, FSLGuideTextures& Out) const;
private:
    FSceneViewStateReference ViewState;
    TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> Frame;
};

/** Single-view auxiliary rendering and SVGF integration. No engine patch required. */
class FSLGuideProbe : public FWorldSceneViewExtension
{
public:
    FSLGuideProbe(const FAutoRegister& Register, UWorld* World);
    virtual ~FSLGuideProbe();
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override;
    /** Negative disables the wipe; otherwise the raw image occupies [0, Position). */
    void SetComparisonPosition(float Position);
    bool HasMissingInputs() const;
    uint32 GetFilteredFrames() const;
    /** Call after flushing rendering, before warming a new benchmark pose. */
    void ResetFrameCounters();
private:
    FSceneViewStateReference GuideViewState;
    TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> Frame;
};
