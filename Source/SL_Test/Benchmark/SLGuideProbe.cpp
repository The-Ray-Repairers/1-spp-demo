#include "SLGuideProbe.h"
#include "SLSVGFFilter.h"
#include "SL_Test.h"
#include "SceneInterface.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneRenderTargetParameters.h"
#include "CustomRenderPassSceneCapture.h"
#include "Rendering/CustomRenderPass.h"
#include "PathTracingDenoiser.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"

class FSLGuidePS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FSLGuidePS);
    SHADER_USE_PARAMETER_STRUCT(FSLGuidePS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSLGuidePS, "/Project/SLGuideProbe.usf", "MainPS", SF_Pixel);

struct FSLGuideFrame
{
    FRDGBuilder* Graph = nullptr;
    uint32 Number = 0;
    TArray<FRDGTextureRef> Textures;
};

class FSLGuidePass final : public FCustomRenderPassBase
{
public:
    IMPLEMENT_CUSTOM_RENDER_PASS(FSLGuidePass);
    FSLGuidePass(FIntPoint Size, TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> InFrame)
        : FCustomRenderPassBase(TEXT("SLGuides"), ERenderMode::DepthAndBasePass,
            ERenderOutput::SceneColorAndDepth, Size), Frame(MoveTemp(InFrame))
    {
        auto Data = MakeUnique<FSceneCaptureCustomRenderPassUserData>();
        Data->bMainViewFamily = true;
        Data->bMainViewResolution = true;
        Data->bMainViewCamera = true;
        SetUserData(MoveTemp(Data));
    }
protected:
    virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
    {
        RenderTargetTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(RenderTargetSize,
            PF_A32B32G32R32F, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("SLGuides.Capture"));
    }
    virtual void OnPostRender(FRDGBuilder& GraphBuilder) override
    {
        check(Views.Num() == 1);
        const FViewInfo& View = *Views[0];
        Frame->Graph = &GraphBuilder;
        Frame->Number = View.Family->FrameNumber;
        Frame->Textures.Reset();
        const TCHAR* Names[] = {TEXT("SLGuides.NormalDepth"), TEXT("SLGuides.Diffuse"),
            TEXT("SLGuides.Emission"), TEXT("SLGuides.Motion")};
        auto* Params = GraphBuilder.AllocParameters<FSLGuidePS::FParameters>();
        Params->View = View.ViewUniformBuffer;
        Params->SceneTextures = CreateSceneTextureUniformBuffer(GraphBuilder, &View.GetSceneTextures(), View.GetFeatureLevel(), ESceneTextureSetupMode::All);
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
        {
            auto Texture = GraphBuilder.CreateTexture(RenderTargetTexture->Desc, Names[Index]);
            Frame->Textures.Add(Texture);
            Params->RenderTargets[Index] = FRenderTargetBinding(Texture, ERenderTargetLoadAction::ENoAction);
        }
        TShaderMapRef<FSLGuidePS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GetGlobalShaderMap(View.GetFeatureLevel()),
            RDG_EVENT_NAME("SLGuides.Export"), Shader, Params, View.ViewRect);
    }
private:
    TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> Frame;
};

class FSLGuideDenoiser final : public UE::Renderer::Private::IPathTracingDenoiser
{
public:
    FSLGuideDenoiser(TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> InFrame) : Frame(MoveTemp(InFrame)) {}
    virtual void AddPasses(FRDGBuilder& Graph, const FSceneView& View, const FInputs& Inputs) const override
    {
        const bool bSameFrame = Frame->Graph == &Graph && Frame->Number == View.Family->FrameNumber && Frame->Textures.Num() == 4
            && Frame->Textures[0]->Desc.Extent == Inputs.ColorTex->Desc.Extent;
        if (!bSameFrame)
        {
            UE_LOG(LogSL_Test, Error, TEXT("SLGuides: missing same-frame inputs"));
        }
        auto Result = Inputs.ColorTex;
        if (bSameFrame)
            Result = Filter.AddPasses(Graph, View, Inputs.ColorTex, Frame->Textures[0], Frame->Textures[1], Frame->Textures[2], Frame->Textures[3]);
        AddCopyTexturePass(Graph, Result, Inputs.OutputTex);

    }
private:
    TSharedPtr<FSLGuideFrame, ESPMode::ThreadSafe> Frame;
    mutable FSLSVGFFilter Filter;
};

FSLGuideProbe::FSLGuideProbe(const FAutoRegister& Register, UWorld* World)
    : FWorldSceneViewExtension(Register, World), Frame(MakeShared<FSLGuideFrame, ESPMode::ThreadSafe>())
{
    GuideViewState.Allocate(World->GetFeatureLevel());
    RegisterSpatialDenoiser(MakeUnique<FSLGuideDenoiser>(Frame), TEXT("SLGuides"));
}

FSLGuideProbe::~FSLGuideProbe()
{
    UnregisterDenoiser(TEXT("SLGuides"));
    GuideViewState.Destroy();
}


void FSLGuideProbe::BeginRenderViewFamily(FSceneViewFamily& Family)
{
    if (!Family.bIsMainViewFamily || !Family.EngineShowFlags.PathTracing || Family.Views.Num() != 1) return;
    const FSceneView& View = *Family.Views[0];
    FSceneInterface::FCustomRenderPassRendererInput Input;
    Input.ViewLocation = View.ViewMatrices.GetViewOrigin();
    Input.ViewRotationMatrix = View.ViewMatrices.GetWorldToView().RemoveTranslation();
    Input.ProjectionMatrix = View.ViewMatrices.GetViewToClip();
    Input.ViewStateInterface = GuideViewState.GetReference();
    Input.EngineShowFlags.SetUnlitViewmode(false);
    Input.CustomRenderPass = new FSLGuidePass(View.UnscaledViewRect.Size(), Frame);
    Family.Scene->AddCustomRenderPass(&Family, Input);
}
