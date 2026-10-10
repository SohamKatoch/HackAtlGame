#include "UI/NeonUIStyle.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"

namespace NeonUI
{
	namespace
	{
		constexpr float ButtonRadius = 6.f;

		FButtonStyle MakeButtonStyle(const FLinearColor& Fill, const FLinearColor& Outline)
		{
			FLinearColor Hover = Fill;
			Hover.R = FMath::Min(Hover.R + 0.06f, 1.f);
			Hover.G = FMath::Min(Hover.G + 0.08f, 1.f);
			Hover.B = FMath::Min(Hover.B + 0.10f, 1.f);

			FButtonStyle Style;
			Style.SetNormal(FSlateRoundedBoxBrush(Fill, ButtonRadius, Outline, 1.5f));
			Style.SetHovered(FSlateRoundedBoxBrush(Hover, ButtonRadius, Accent, 1.5f));
			Style.SetPressed(FSlateRoundedBoxBrush(Accent * 0.75f, ButtonRadius, Accent, 1.5f));
			Style.SetDisabled(FSlateRoundedBoxBrush(ButtonDisabled, ButtonRadius, FLinearColor(Dim.R, Dim.G, Dim.B, 0.4f), 1.f));
			Style.SetNormalPadding(FMargin(0.f));
			Style.SetPressedPadding(FMargin(0.f, 1.f, 0.f, -1.f));
			return Style;
		}

		UTextBlock* FindButtonLabel(UButton* Button)
		{
			UWidget* Child = Button ? Button->GetChildAt(0) : nullptr;
			if (UBorder* Pad = Cast<UBorder>(Child))
			{
				Child = Pad->GetChildAt(0);
			}
			return Cast<UTextBlock>(Child);
		}
	}

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& InText, int32 Size, const FLinearColor& Color, bool bBold)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Block->SetText(InText);
		Block->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(ETextJustify::Center);
		Block->SetShadowOffset(FVector2D(1.5f, 1.5f));
		Block->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));
		return Block;
	}

	UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel)
	{
		UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass());
		Button->SetStyle(MakeButtonStyle(ButtonIdle, PanelOutline));
		Button->SetBackgroundColor(FLinearColor::White); // tint on top of the style brushes

		UTextBlock* LabelText = MakeText(Tree, Label, Size, NeonUI::Text);

		// Padding around the label (UButton's own padding lives in its style; a transparent border is simpler).
		UBorder* Pad = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Pad->SetBrushColor(FLinearColor::Transparent);
		Pad->SetPadding(FMargin(28.f, 10.f));
		Pad->SetHorizontalAlignment(HAlign_Center);
		Pad->SetVerticalAlignment(VAlign_Center);
		Pad->AddChild(LabelText);
		Button->AddChild(Pad);

		if (OutLabel)
		{
			*OutLabel = LabelText;
		}
		return Button;
	}

	UBorder* MakePanel(UWidgetTree* Tree, const FMargin& Padding, float Radius)
	{
		UBorder* Border = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Border->SetBrush(FSlateRoundedBoxBrush(Panel, Radius, PanelOutline, 1.f));
		Border->SetBrushColor(FLinearColor::White);
		Border->SetPadding(Padding);
		return Border;
	}

	UWidget* MakeSwatch(UWidgetTree* Tree, const FLinearColor& Color, float Size)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Size);
		Box->SetHeightOverride(Size);
		UBorder* Fill = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Fill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 3.f, FLinearColor(0.f, 0.f, 0.f, 0.6f), 1.f));
		Fill->SetBrushColor(Color);
		Box->AddChild(Fill);
		return Box;
	}

	void SetSwatchColor(UWidget* Swatch, const FLinearColor& Color)
	{
		if (USizeBox* Box = Cast<USizeBox>(Swatch))
		{
			if (UBorder* Fill = Cast<UBorder>(Box->GetChildAt(0)))
			{
				Fill->SetBrushColor(Color);
			}
		}
	}

	void SetButtonSelected(UButton* Button, bool bSelected)
	{
		if (!Button)
		{
			return;
		}
		Button->SetStyle(bSelected ? MakeButtonStyle(ButtonSelected, Accent) : MakeButtonStyle(ButtonIdle, PanelOutline));
		if (UTextBlock* Label = FindButtonLabel(Button))
		{
			Label->SetColorAndOpacity(FSlateColor(bSelected ? LabelOnSelected : NeonUI::Text));
			Label->SetShadowColorAndOpacity(bSelected ? FLinearColor::Transparent : FLinearColor(0.f, 0.f, 0.f, 0.8f));
		}
	}

	void SetButtonEnabled(UButton* Button, UTextBlock* Label, bool bEnabled)
	{
		if (Button)
		{
			Button->SetIsEnabled(bEnabled);
		}
		if (Label)
		{
			Label->SetColorAndOpacity(FSlateColor(bEnabled ? NeonUI::Text : NeonUI::Dim));
		}
	}
}
