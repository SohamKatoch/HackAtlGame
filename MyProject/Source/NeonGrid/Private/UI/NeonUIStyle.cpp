#include "UI/NeonUIStyle.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"

namespace NeonUI
{
	namespace
	{
		constexpr float ButtonRadius = 0.f;

		FButtonStyle MakeButtonStyle(const FLinearColor& Fill, const FLinearColor& Outline, float Edge = 1.f)
		{
			FButtonStyle Style;
			Style.SetNormal(FSlateRoundedBoxBrush(Fill, ButtonRadius, Outline, Edge));
			Style.SetHovered(FSlateRoundedBoxBrush(Surface, ButtonRadius, Bright, 1.25f));
			Style.SetPressed(FSlateRoundedBoxBrush(Navy, ButtonRadius, Highlight, 1.25f));
			Style.SetDisabled(FSlateRoundedBoxBrush(Ink, ButtonRadius, FLinearColor(Line.R, Line.G, Line.B, 0.25f), 1.f));
			Style.SetNormalPadding(FMargin(0.f));
			Style.SetPressedPadding(FMargin(0.f));
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
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), Size);
		Font.LetterSpacing = Size >= 40 ? 2 : 0;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(ETextJustify::Center);
		// A one-pixel dark shadow keeps light text legible over the bright arena and the glass panels.
		Block->SetShadowOffset(FVector2D(1.f, 1.f));
		Block->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.75f));
		return Block;
	}

	UTextBlock* MakeCaps(UWidgetTree* Tree, const FText& InText, int32 Size, const FLinearColor& Color, int32 Tracking)
	{
		UTextBlock* Block = MakeText(Tree, InText, Size, Color, true);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("BoldCondensed"), Size);
		Font.LetterSpacing = Tracking;
		Block->SetFont(Font);
		return Block;
	}

	UTextBlock* MakeDisplay(UWidgetTree* Tree, const FText& InText, int32 Size, const FLinearColor& Color, int32 Tracking)
	{
		UTextBlock* Block = MakeText(Tree, InText, Size, Color, true);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Black"), Size);
		Font.LetterSpacing = Tracking;
		Block->SetFont(Font);
		return Block;
	}

	UWidget* MakeRule(UWidgetTree* Tree, const FLinearColor& Color, float Thickness)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetHeightOverride(Thickness);
		UBorder* Fill = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Fill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 0.f));
		Fill->SetBrushColor(Color);
		Box->AddChild(Fill);
		return Box;
	}

	UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel)
	{
		UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass());
		Button->SetStyle(MakeButtonStyle(ButtonIdle, Line, 1.f));
		Button->SetBackgroundColor(FLinearColor::White);

		UTextBlock* LabelText = MakeCaps(Tree, Label, Size, NeonUI::Text, 4);
		LabelText->SetJustification(ETextJustify::Left);

		UBorder* Pad = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Pad->SetBrushColor(FLinearColor::Transparent);
		Pad->SetPadding(FMargin(14.f, 8.f));
		Pad->SetHorizontalAlignment(HAlign_Left);
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
		(void)Radius;
		UBorder* Border = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Border->SetBrush(FSlateRoundedBoxBrush(Panel, 0.f, PanelOutline, 1.f));
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
		Fill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 0.f, FLinearColor(0.f, 0.f, 0.f, 0.85f), 1.f));
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
		Button->SetStyle(bSelected ? MakeButtonStyle(ButtonSelected, Bright, 1.25f) : MakeButtonStyle(ButtonIdle, Line, 1.f));
		if (UTextBlock* Label = FindButtonLabel(Button))
		{
			Label->SetColorAndOpacity(FSlateColor(bSelected ? LabelOnSelected : NeonUI::Text));
			Label->SetShadowColorAndOpacity(bSelected ? FLinearColor::Transparent : FLinearColor(0.f, 0.f, 0.f, 0.75f));
		}
	}

	void StyleMeter(UProgressBar* Bar)
	{
		if (!Bar)
		{
			return;
		}
		FProgressBarStyle Style = Bar->GetWidgetStyle();
		Style.SetBackgroundImage(FSlateRoundedBoxBrush(Navy, 0.f, Line, 1.f));
		Style.SetFillImage(FSlateRoundedBoxBrush(Accent, 0.f));
		Bar->SetWidgetStyle(Style);
	}

	void StyleAddressBox(UEditableTextBox* Box)
	{
		if (!Box)
		{
			return;
		}
		FEditableTextBoxStyle Style = Box->WidgetStyle;
		const FLinearColor Fill(0.00f, 0.00f, 0.00f, 0.95f);
		Style.SetBackgroundImageNormal(FSlateRoundedBoxBrush(Fill, 4.f, PanelOutline, 1.5f));
		Style.SetBackgroundImageHovered(FSlateRoundedBoxBrush(Fill, 4.f, Accent, 2.f));
		Style.SetBackgroundImageFocused(FSlateRoundedBoxBrush(Fill, 4.f, Accent, 2.f));
		Style.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(ButtonDisabled, 4.f, PanelOutline, 1.f));
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("BoldCondensed"), 20);
		Font.LetterSpacing = 3;
		Style.SetFont(Font);
		Style.SetForegroundColor(Text);
		Box->WidgetStyle = Style;
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
