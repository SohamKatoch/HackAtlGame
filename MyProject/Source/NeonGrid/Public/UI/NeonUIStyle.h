#pragma once

#include "CoreMinimal.h"

class UWidgetTree;
class UTextBlock;
class UButton;
class UWidget;

/**
 * Small helpers for building the code-defined menu and HUD widgets. Presentation only; a Blueprint
 * subclass of either widget can replace the whole tree (Docs/TECHNICAL_SPEC.md section 2).
 */
namespace NeonUI
{
	// Palette (guide section 64: dark arena, neon accents, subtle holographic UI).
	inline const FLinearColor Accent(0.10f, 0.90f, 1.00f);
	inline const FLinearColor Text(0.92f, 0.96f, 1.00f);
	inline const FLinearColor Dim(0.45f, 0.50f, 0.58f);
	inline const FLinearColor Panel(0.02f, 0.03f, 0.06f, 0.82f);
	inline const FLinearColor ButtonIdle(0.06f, 0.09f, 0.16f, 0.95f);
	inline const FLinearColor ButtonSelected(0.10f, 0.90f, 1.00f, 0.85f);
	inline const FLinearColor ButtonDisabled(0.04f, 0.05f, 0.08f, 0.6f);

	NEONGRID_API UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, bool bBold = true);

	/** Button with a centered text label. Caller binds OnClicked. */
	NEONGRID_API UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel = nullptr);

	NEONGRID_API void SetButtonSelected(UButton* Button, bool bSelected);
	NEONGRID_API void SetButtonEnabled(UButton* Button, UTextBlock* Label, bool bEnabled);
}
