#pragma once

#include "SLGMToolConfig.h"

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
        {TEXT("Ours"), TEXT("Ours"), ESLComparisonAction::ApplyProfile, TEXT("Ours is not implemented in this project.")},
        {TEXT("KPN_Direct"), TEXT("KPN/Direct"), ESLComparisonAction::ApplyProfile, TEXT("KPN/Direct is not implemented in this project.")},
        {TEXT("OpenImageDenoise"), TEXT("Open Image Denoise"), ESLComparisonAction::ApplyProfile, TEXT("")}
    };
    return Methods;
}
}
