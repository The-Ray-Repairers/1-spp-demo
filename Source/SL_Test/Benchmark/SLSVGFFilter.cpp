#include "SLSVGFFilter.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"
DECLARE_GPU_STAT(SLSVGF);

static TAutoConsoleVariable<int32> CVarIterations(TEXT("SL.SVGF.Iterations"), 4, TEXT("Falcor a-trous iterations (1..10)"), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarFeedback(TEXT("SL.SVGF.FeedbackTap"), 1, TEXT("Falcor feedback iteration; -1 is unfiltered reprojection"), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarAlpha(TEXT("SL.SVGF.Alpha"), .05f, TEXT("Temporal illumination alpha"), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarMomentsAlpha(TEXT("SL.SVGF.MomentsAlpha"), .2f, TEXT("Temporal moments alpha"), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarPhiColor(TEXT("SL.SVGF.PhiColor"), 10.f, TEXT("Falcor luminance edge stopping"), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarPhiNormal(TEXT("SL.SVGF.PhiNormal"), 128.f, TEXT("Falcor normal edge stopping"), ECVF_RenderThreadSafe);

BEGIN_SHADER_PARAMETER_STRUCT(FSLSVGFParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gNormalDepth)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gMotion)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gPositionNormalFwidth)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gColor)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gAlbedo)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gEmission)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gPrevIllum)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gPrevMoments)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gLinearZAndNormal)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gPrevLinearZAndNormal)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gPrevHistoryLength)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gIllumination)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gMoments)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, gHistoryLength)
    SHADER_PARAMETER(float, gAlpha)
    SHADER_PARAMETER(float, gMomentsAlpha)
    SHADER_PARAMETER(float, gPhiColor)
    SHADER_PARAMETER(float, gPhiNormal)
    SHADER_PARAMETER(int32, gStepSize)
    SHADER_PARAMETER(uint32, gResetHistory)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

#define SL_SVGF_SHADER(Name, File) \
class Name : public FGlobalShader { \
    DECLARE_GLOBAL_SHADER(Name); \
    SHADER_USE_PARAMETER_STRUCT(Name, FGlobalShader); \
    using FParameters = FSLSVGFParameters; \
}; \
IMPLEMENT_GLOBAL_SHADER(Name, File, "MainPS", SF_Pixel);

SL_SVGF_SHADER(FSLSVGFPackPS, "/Project/SVGF/Pack.usf")
SL_SVGF_SHADER(FSLSVGFReprojectPS, "/Project/SVGF/Reproject.usf")
SL_SVGF_SHADER(FSLSVGFMomentsPS, "/Project/SVGF/FilterMoments.usf")
SL_SVGF_SHADER(FSLSVGFAtrousPS, "/Project/SVGF/Atrous.usf")
SL_SVGF_SHADER(FSLSVGFFinalPS, "/Project/SVGF/FinalModulate.usf")

template<typename T> static void Draw(FRDGBuilder& Graph, const FSceneView& View,
    FSLSVGFParameters* Params, FIntPoint Size, const TCHAR* Label)
{
    TShaderMapRef<T> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FPixelShaderUtils::AddFullscreenPass(Graph, GetGlobalShaderMap(View.GetFeatureLevel()),
        FRDGEventName(TEXT("%s"), Label), Shader, Params, FIntRect(FIntPoint::ZeroValue, Size));
}

FRDGTextureRef FSLSVGFFilter::AddPasses(FRDGBuilder& Graph, const FSceneView& View, FRDGTextureRef Color,
    FRDGTextureRef NormalDepth, FRDGTextureRef Diffuse, FRDGTextureRef Emission, FRDGTextureRef Motion)
{
    RDG_EVENT_SCOPE_STAT(Graph, SLSVGF, "SLSVGF");
    const TArray<float> Settings = {float(CVarIterations.GetValueOnRenderThread()), float(CVarFeedback.GetValueOnRenderThread()),
        CVarAlpha.GetValueOnRenderThread(), CVarMomentsAlpha.GetValueOnRenderThread(), CVarPhiColor.GetValueOnRenderThread(), CVarPhiNormal.GetValueOnRenderThread()};
    const FIntPoint NewSize = Color->Desc.Extent;
    const bool Reset = Size != NewSize || View.bCameraCut || !Illumination || PreviousFrame + 1 != View.Family->FrameNumber || Settings != PreviousSettings;
    PreviousSettings = Settings;
    Size = NewSize;
    PreviousFrame = View.Family->FrameNumber;
    auto Texture = [&](const TCHAR* Name) {
        return Graph.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_A32B32G32R32F,
            FClearValueBinding::None, TexCreate_RenderTargetable | TexCreate_ShaderResource), Name);
    };
    auto* Defaults = Graph.AllocParameters<FSLSVGFParameters>();
    Defaults->gNormalDepth = NormalDepth;
    Defaults->gMotion = Motion;
    Defaults->gColor = Color;
    Defaults->gAlbedo = Diffuse;
    Defaults->gEmission = Emission;
    Defaults->gAlpha = FMath::Clamp(CVarAlpha.GetValueOnRenderThread(), 0.f, 1.f);
    Defaults->gMomentsAlpha = FMath::Clamp(CVarMomentsAlpha.GetValueOnRenderThread(), 0.f, 1.f);
    Defaults->gPhiColor = FMath::Max(CVarPhiColor.GetValueOnRenderThread(), 1e-6f);
    Defaults->gPhiNormal = FMath::Max(CVarPhiNormal.GetValueOnRenderThread(), 0.f);
    Defaults->gResetHistory = Reset;
    auto Parameters = [&](std::initializer_list<FRDGTextureRef> Targets) {
        auto* P = Graph.AllocParameters<FSLSVGFParameters>();
        *P = *Defaults;
        int32 Index = 0;
        for (auto Target : Targets) P->RenderTargets[Index++] = FRenderTargetBinding(Target, ERenderTargetLoadAction::ENoAction);
        return P;
    };
    auto Packed = Texture(TEXT("SVGF.PackedGuides"));
    auto Fwidth = Texture(TEXT("SVGF.NormalFwidth"));
    Draw<FSLSVGFPackPS>(Graph, View, Parameters({Packed,Fwidth}), Size, TEXT("SVGF.Pack"));
    Defaults->gLinearZAndNormal = Packed;
    Defaults->gPositionNormalFwidth = Fwidth;
    auto Zero = Texture(TEXT("SVGF.ZeroHistory"));
    AddClearRenderTargetPass(Graph, Zero, FLinearColor::Transparent);
    Defaults->gPrevIllum = Reset ? Zero : Graph.RegisterExternalTexture(Illumination);
    Defaults->gPrevMoments = Reset ? Zero : Graph.RegisterExternalTexture(Moments);
    Defaults->gPrevHistoryLength = Reset ? Zero : Graph.RegisterExternalTexture(HistoryLength);
    Defaults->gPrevLinearZAndNormal = Reset ? Zero : Graph.RegisterExternalTexture(PackedGuide);
    auto Reprojected = Texture(TEXT("SVGF.Reprojected"));
    auto NewMoments = Texture(TEXT("SVGF.Moments"));
    auto NewHistory = Texture(TEXT("SVGF.HistoryLength"));
    Draw<FSLSVGFReprojectPS>(Graph, View, Parameters({Reprojected,NewMoments,NewHistory}), Size, TEXT("SVGF.Reproject"));
    Defaults->gIllumination = Reprojected;
    Defaults->gMoments = NewMoments;
    Defaults->gHistoryLength = NewHistory;
    auto Filtered = Texture(TEXT("SVGF.FilteredMoments"));
    Draw<FSLSVGFMomentsPS>(Graph, View, Parameters({Filtered}), Size, TEXT("SVGF.FilterMoments"));
    const int32 Iterations = FMath::Clamp(CVarIterations.GetValueOnRenderThread(), 1, 10);
    const int32 Feedback = FMath::Clamp(CVarFeedback.GetValueOnRenderThread(), -1, Iterations-1);
    FRDGTextureRef FeedbackTexture = Reprojected;
    for (int32 Index = 0; Index < Iterations; ++Index)
    {
        Defaults->gIllumination = Filtered;
        Defaults->gStepSize = 1 << Index;
        Filtered = Texture(TEXT("SVGF.Atrous"));
        Draw<FSLSVGFAtrousPS>(Graph, View, Parameters({Filtered}), Size, TEXT("SVGF.Atrous"));
        if (Index == Feedback) FeedbackTexture = Filtered;
    }
    Defaults->gIllumination = Filtered;
    auto Result = Texture(TEXT("SVGF.Final"));
    Draw<FSLSVGFFinalPS>(Graph, View, Parameters({Result}), Size, TEXT("SVGF.FinalModulate"));
    Graph.QueueTextureExtraction(FeedbackTexture, &Illumination);
    Graph.QueueTextureExtraction(NewMoments, &Moments);
    Graph.QueueTextureExtraction(NewHistory, &HistoryLength);
    Graph.QueueTextureExtraction(Packed, &PackedGuide);
    return Result;
}
