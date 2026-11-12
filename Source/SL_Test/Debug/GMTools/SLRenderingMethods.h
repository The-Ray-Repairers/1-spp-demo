#pragma once

#include "SLGMToolConfig.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"

/** Shared order and captions for every GM method selector. IDs are safe directory names. */
namespace SLRenderingMethods
{
struct FMethod
{
    const TCHAR* Id;
    const TCHAR* Label;
    ESLComparisonAction Action;
    const TCHAR* UnavailableReason;
};

inline int32 GetDenoiserMethod(FName Id)
{
    if (Id == TEXT("Ours")) return 5;
    if (Id == TEXT("KPN_Direct")) return 6;
    return 0;
}

inline bool CheckModelAvailable(int32 Method, FText& Reason)
{
    Reason = FText::GetEmpty();
    if (Method != 5 && Method != 6) return true;
    const auto* Model = IConsoleManager::Get().FindConsoleVariable(TEXT("SL.Denoiser.RayRepair.Model"));
    const FString Path = Method == 6 ? TEXT("/Game/ML/nb_direct") : (Model ? Model->GetString() : FString());
    if (!Path.IsEmpty() && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path))) return true;
    Reason = FText::FromString(TEXT("Neural model asset is missing: ") + Path);
    return false;
}

inline ESLGMRenderBackend GetBackend(const FMethod& Method)
{
    if (FString(Method.Id) == TEXT("OpenImageDenoise")) return ESLGMRenderBackend::OpenImageDenoise;
    if (Method.Action == ESLComparisonAction::SVGF) return ESLGMRenderBackend::SVGF;
    if (Method.Action == ESLComparisonAction::RR) return ESLGMRenderBackend::LumenDLSSRR;
    return ESLGMRenderBackend::PathTracing;
}

inline const TArray<FMethod>& Get()
{
    static const TArray<FMethod> Methods = {
        {TEXT("1SPP_Noisy"), TEXT("1spp"), ESLComparisonAction::Raw, TEXT("")},
        {TEXT("512SPP_GT"), TEXT("512spp"), ESLComparisonAction::Reference, TEXT("")},
        {TEXT("SVGF"), TEXT("SVGF"), ESLComparisonAction::SVGF, TEXT("")},
        {TEXT("DLSS"), TEXT("DLSS"), ESLComparisonAction::RR, TEXT("")},
        {TEXT("Ours"), TEXT("Ours"), ESLComparisonAction::ApplyProfile, TEXT("")},
        {TEXT("KPN_Direct"), TEXT("KPN/Direct"), ESLComparisonAction::ApplyProfile, TEXT("")},
        {TEXT("OpenImageDenoise"), TEXT("Open Image Denoise"), ESLComparisonAction::ApplyProfile, TEXT("")}
    };
    return Methods;
}
}
