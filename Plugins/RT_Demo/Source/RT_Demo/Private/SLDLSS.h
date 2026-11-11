#pragma once

#include "CoreMinimal.h"

#if RT_DEMO_WITH_DLSS
#include "DLSSLibrary.h"
#endif

/**
 * NVIDIA DLSS is an optional dependency of RT_Demo (see RT_Demo.Build.cs).
 * Built without the DLSS plugin, these report "unsupported / disabled" and
 * the setters do nothing, so the Lumen + DLSS RR profile is simply rejected.
 */
namespace SLDLSS
{
#if RT_DEMO_WITH_DLSS
	constexpr bool bCompiledIn = true;
	inline bool IsSupported() { return UDLSSLibrary::IsDLSSSupported(); }
	inline bool IsRRSupported() { return UDLSSLibrary::IsDLSSRRSupported(); }
	inline bool IsEnabled() { return UDLSSLibrary::IsDLSSEnabled(); }
	inline bool IsRREnabled() { return UDLSSLibrary::IsDLSSRREnabled(); }
	inline void Enable(bool bEnabled) { UDLSSLibrary::EnableDLSS(bEnabled); }
	inline void EnableRR(bool bEnabled) { UDLSSLibrary::EnableDLSSRR(bEnabled); }
#else
	constexpr bool bCompiledIn = false;
	inline bool IsSupported() { return false; }
	inline bool IsRRSupported() { return false; }
	inline bool IsEnabled() { return false; }
	inline bool IsRREnabled() { return false; }
	inline void Enable(bool) {}
	inline void EnableRR(bool) {}
#endif
}
