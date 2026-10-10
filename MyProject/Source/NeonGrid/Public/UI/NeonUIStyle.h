#pragma once

#include "CoreMinimal.h"

class UWidgetTree;
class UTextBlock;
class UButton;
class UBorder;
class UWidget;

/**
 * Small helpers for building the code-defined menu and HUD widgets. Presentation only; a Blueprint
 * subclass of either widget can replace the whole tree (Docs/TECHNICAL_SPEC.md section 2).
 *
 * Buttons and panels use rounded-box Slate brushes with a thin outline so the UI reads as holographic
 * panels over the arena rather than flat rectangles.
 */
namespace NeonUI
{
	// Palette (guide section 64: dark arena, neon accents, subtle holographic UI).
	inline const FLinearColor Accent(0.10f, 0.90f, 1.00f);
	inline const FLinearColor Warning(1.00f, 0.45f, 0.10f);
	inline const FLinearColor Text(0.92f, 0.96f, 1.00f);
	inline const FLinearColor Dim(0.45f, 0.50f, 0.58f);
	inline const FLinearColor Panel(0.02f, 0.03f, 0.06f, 0.78f);
	inline const FLinearColor PanelOutline(0.10f, 0.90f, 1.00f, 0.35f);
	inline const FLinearColor ButtonIdle(0.06f, 0.09f, 0.16f, 0.95f);
	inline const FLinearColor ButtonSelected(0.10f, 0.90f, 1.00f, 0.90f);
	inline const FLinearColor ButtonDisabled(0.04f, 0.05f, 0.08f, 0.6f);
	inline const FLinearColor LabelOnSelected(0.02f, 0.05f, 0.08f);

	NEONGRID_API UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, bool bBold = true);

	/** Button with a centered text label. Caller binds OnClicked. */
	NEONGRID_API UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel = nullptr);

	/** Rounded translucent panel with an accent outline. Caller adds one child. */
	NEONGRID_API UBorder* MakePanel(UWidgetTree* Tree, const FMargin& Padding, float Radius = 10.f);

	/** Small filled square used as a player color swatch. */
	NEONGRID_API UWidget* MakeSwatch(UWidgetTree* Tree, const FLinearColor& Color, float Size = 16.f);
	NEONGRID_API void SetSwatchColor(UWidget* Swatch, const FLinearColor& Color);

	NEONGRID_API void SetButtonSelected(UButton* Button, bool bSelected);
	NEONGRID_API void SetButtonEnabled(UButton* Button, UTextBlock* Label, bool bEnabled);
}
