#pragma once

#include "CoreMinimal.h"

class UWidgetTree;
class UTextBlock;
class UButton;
class UBorder;
class UWidget;
class UProgressBar;
class UEditableTextBox;

/**
 * Black-and-blue lightbike control system. One palette drives every menu and HUD widget.
 * Changing Accent / Bright / Highlight retints the whole UI.
 */
namespace NeonUI
{
	// #000000 / #05070A / #07111C / #0A1624 / #008CFF / #00B7FF / #8DDCFF
	inline const FLinearColor Ink(0.00f, 0.00f, 0.00f, 1.00f);
	inline const FLinearColor Void(0.020f, 0.027f, 0.039f, 0.92f);
	inline const FLinearColor Surface(0.027f, 0.067f, 0.110f, 0.88f);
	inline const FLinearColor Navy(0.039f, 0.086f, 0.141f, 0.94f);
	inline const FLinearColor Line(0.000f, 0.35f, 0.65f, 0.85f);
	inline const FLinearColor Accent(0.000f, 0.549f, 1.000f);
	inline const FLinearColor Bright(0.000f, 0.718f, 1.000f);
	inline const FLinearColor Highlight(0.553f, 0.863f, 1.000f);
	inline const FLinearColor Text(0.92f, 0.96f, 1.00f);
	inline const FLinearColor Dim(0.28f, 0.40f, 0.50f);
	inline const FLinearColor Warning(0.000f, 0.718f, 1.000f); // alerts stay in-system blue
	inline const FLinearColor Panel(0.027f, 0.067f, 0.110f, 0.90f);
	inline const FLinearColor PanelOutline(0.000f, 0.549f, 1.000f, 0.85f);
	inline const FLinearColor ButtonIdle(0.000f, 0.000f, 0.000f, 0.0f);
	inline const FLinearColor ButtonSelected(0.027f, 0.067f, 0.110f, 0.55f);
	inline const FLinearColor ButtonDisabled(0.00f, 0.00f, 0.00f, 0.0f);
	inline const FLinearColor LabelOnSelected(0.553f, 0.863f, 1.000f);

	NEONGRID_API UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, bool bBold = true);
	NEONGRID_API UTextBlock* MakeCaps(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, int32 Tracking = 2);
	NEONGRID_API UTextBlock* MakeDisplay(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, int32 Tracking = 1);
	NEONGRID_API UWidget* MakeRule(UWidgetTree* Tree, const FLinearColor& Color, float Thickness = 1.f);

	/** Angular text row. No fat rounded slab. */
	NEONGRID_API UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel = nullptr);

	/** Dark-blue panel, 1 px electric edge, square corners. */
	NEONGRID_API UBorder* MakePanel(UWidgetTree* Tree, const FMargin& Padding, float Radius = 0.f);

	NEONGRID_API void StyleMeter(UProgressBar* Bar);
	NEONGRID_API void StyleAddressBox(UEditableTextBox* Box);

	NEONGRID_API UWidget* MakeSwatch(UWidgetTree* Tree, const FLinearColor& Color, float Size = 16.f);
	NEONGRID_API void SetSwatchColor(UWidget* Swatch, const FLinearColor& Color);

	NEONGRID_API void SetButtonSelected(UButton* Button, bool bSelected);
	NEONGRID_API void SetButtonEnabled(UButton* Button, UTextBlock* Label, bool bEnabled);
}
