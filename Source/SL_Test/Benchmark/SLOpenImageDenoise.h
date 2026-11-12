#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

/** The engine plugin owns the OIDN implementation and its CPU buffer transfers. */
namespace SLOpenImageDenoise
{
inline bool IsAvailable(FText& Reason)
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("OpenImageDenoise"))
        || !IConsoleManager::Get().FindConsoleVariable(TEXT("r.OIDN.UseAuxilaryInputs")))
    {
        Reason = FText::FromString(TEXT("Open Image Denoise requires the enabled OpenImageDenoise engine plugin on Win64 x64. Restart after enabling the plugin."));
        return false;
    }
    Reason = FText::GetEmpty();
    return true;
}

/** The caller supplies its snapshot-and-restore setter. */
template<typename TSetter>
void Apply(TSetter&& Set)
{
    Set(TEXT("r.PathTracing.Denoiser"), TEXT("1"));
    Set(TEXT("r.PathTracing.SpatialDenoiser"), TEXT("1"));
    Set(TEXT("r.PathTracing.SpatialDenoiser.Type"), TEXT("0"));
    Set(TEXT("r.PathTracing.TemporalDenoiser"), TEXT("0"));
    Set(TEXT("r.PathTracing.Denoiser.Name"), TEXT("OIDN"));
    Set(TEXT("r.OIDN.UseAuxilaryInputs"), TEXT("1"));
    Set(TEXT("r.OIDN.DenoiseAuxilaryInputs"), TEXT("1"));
}
}
