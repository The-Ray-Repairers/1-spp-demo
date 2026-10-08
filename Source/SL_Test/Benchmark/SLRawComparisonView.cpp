#include "SLRawComparisonView.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "Engine/TextureRenderTarget.h"
#include "SL_Test.h"
#include "SceneRendering.h"

class FSLRawComparisonPS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FSLRawComparisonPS);
    SHADER_USE_PARAMETER_STRUCT(FSLRawComparisonPS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RawTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CurrentTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, RawSampler)
        SHADER_PARAMETER(FVector2f, ViewMin)
        SHADER_PARAMETER(FVector2f, ViewSize)
        SHADER_PARAMETER(float, Split)
        SHADER_PARAMETER(float, ExposureRatio)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSLRawComparisonPS, "/Project/SLRawComparison.usf", "MainPS", SF_Pixel);

FSLRawComparisonView::FSLRawComparisonView(const FAutoRegister& Register, UWorld* World, FTextureRenderTargetResource* InTarget)
    : FWorldSceneViewExtension(Register, World), Target(InTarget) {}

void FSLRawComparisonView::SetupView(FSceneViewFamily& Family, FSceneView& View)
{
    if (Family.RenderTarget != Target) return;
    // Offline makes the per-view sample count authoritative instead of the right-side CVar.
    // Explicit reset keeps this view at one new sample even with a stationary camera.
    View.bIsOfflineRender = true;
    View.bForcePathTracerReset = true;
    View.FinalPostProcessSettings.PathTracingSamplesPerPixel = 1;
    View.FinalPostProcessSettings.PathTracingEnableDenoiser = false;
    View.AntiAliasingMethod = AAM_None;
}

void FSLRawComparisonView::PreRenderView_RenderThread(FRDGBuilder&, FSceneView& View)
{
    if (View.Family->RenderTarget == Target) RawPreExposure = static_cast<const FViewInfo&>(View).PreExposure;
}

void FSLRawComparisonView::PostRenderView_RenderThread(FRDGBuilder&, FSceneView& View)
{
    if (View.Family->RenderTarget == Target && !bLoggedRaw)
    {
        UE_LOG(LogSL_Test, Display, TEXT("Raw comparison capture: PT=%d RT=%d samples=%u targetSPP=%d rect=%s exposure=%g"),
            int(View.Family->EngineShowFlags.PathTracing), int(View.bSceneCaptureUsesRayTracing),
            View.State ? View.State->GetPathTracingSampleIndex() : 0,
            View.FinalPostProcessSettings.PathTracingSamplesPerPixel, *View.UnscaledViewRect.ToString(), RawPreExposure);
        bLoggedRaw = true;
    }
}

void FSLRawComparisonView::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
    FAfterPassCallbackDelegateArray& Callbacks, bool bEnabled)
{
    if (Pass == EPostProcessingPass::MotionBlur && !View.bIsSceneCapture && Position.Load() >= 0.f)
        Callbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSLRawComparisonView::Composite));
}

FScreenPassTexture FSLRawComparisonView::Composite(FRDGBuilder& Graph, const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs)
{
    const FScreenPassTexture Current(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!Current.IsValid() || !Target->GetRenderTargetTexture()) return Current;
    auto Raw = Graph.RegisterExternalTexture(CreateRenderTarget(Target->GetRenderTargetTexture(), TEXT("SLComparison.Raw")));
    auto Output = Inputs.OverrideOutput;
    if (!Output.IsValid()) Output = FScreenPassRenderTarget::CreateFromInput(
        Graph, Current, ERenderTargetLoadAction::ENoAction, TEXT("SLComparison.Composite"));
    auto* Parameters = Graph.AllocParameters<FSLRawComparisonPS::FParameters>();
    Parameters->RawTexture = Raw;
    Parameters->CurrentTexture = Current.Texture;
    Parameters->RawSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ViewMin = FVector2f(Current.ViewRect.Min);
    Parameters->ViewSize = FVector2f(Current.ViewRect.Size());
    Parameters->Split = Position.Load();
    Parameters->ExposureRatio = static_cast<const FViewInfo&>(View).PreExposure / FMath::Max(RawPreExposure, UE_SMALL_NUMBER);
    Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();
    TShaderMapRef<FSLRawComparisonPS> Shader(GetGlobalShaderMap(View.FeatureLevel));
    FPixelShaderUtils::AddFullscreenPass(Graph, GetGlobalShaderMap(View.FeatureLevel),
        RDG_EVENT_NAME("SLComparison Raw 1SPP / Current"), Shader, Parameters, Output.ViewRect);
    if (CompositeCount.Load() == 0)
        UE_LOG(LogSL_Test, Display, TEXT("Live comparison: compositing independent Raw 1 SPP capture with current renderer."));
    CompositeCount.Store(CompositeCount.Load() + 1);
    return Output;
}
