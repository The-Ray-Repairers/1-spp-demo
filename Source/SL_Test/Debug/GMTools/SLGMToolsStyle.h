#pragma once

#include "CoreMinimal.h"
#include "Styling/CoreStyle.h"

/** Shared runtime palette; opaque fill brushes prevent the scene bleeding through. */
struct FGMToolsStyle
{
	FLinearColor PanelBackground = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("171C24FF")));
	FLinearColor RowBackground = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("252D39FF")));
	FTextBlockStyle Body = FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12))
		.SetColorAndOpacity(FLinearColor::White);

	static const FGMToolsStyle& Get()
	{
		static const FGMToolsStyle Style;
		return Style;
	}
};
