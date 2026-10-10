#include "UI/NeonUIStyle.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"

namespace NeonUI
{
	UTextBlock* MakeText(UWidgetTree* Tree, const FText& InText, int32 Size, const FLinearColor& Color, bool bBold)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Block->SetText(InText);
		Block->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(ETextJustify::Center);
		Block->SetShadowOffset(FVector2D(2.f, 2.f));
		Block->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));
		return Block;
	}

	UButton* MakeButton(UWidgetTree* Tree, const FText& Label, int32 Size, UTextBlock** OutLabel)
	{
		UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass());
		Button->SetBackgroundColor(ButtonIdle);

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

	void SetButtonSelected(UButton* Button, bool bSelected)
	{
		if (Button)
		{
			Button->SetBackgroundColor(bSelected ? ButtonSelected : ButtonIdle);
		}
	}

	void SetButtonEnabled(UButton* Button, UTextBlock* Label, bool bEnabled)
	{
		if (Button)
		{
			Button->SetIsEnabled(bEnabled);
			Button->SetBackgroundColor(bEnabled ? ButtonIdle : ButtonDisabled);
		}
		if (Label)
		{
			Label->SetColorAndOpacity(FSlateColor(bEnabled ? NeonUI::Text : NeonUI::Dim));
		}
	}
}
