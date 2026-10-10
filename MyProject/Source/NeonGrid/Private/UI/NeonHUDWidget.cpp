#include "UI/NeonHUDWidget.h"

#include "Arena/NeonArena.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Core/NeonGameState.h"
#include "Core/NeonPlayerController.h"
#include "Core/NeonPlayerState.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Trail/NeonCollisionSystem.h"
#include "UI/NeonUIStyle.h"
#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehiclePawn.h"

#define LOCTEXT_NAMESPACE "NeonGrid"

TSharedRef<SWidget> UNeonHUDWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}

	TSharedRef<SWidget> Result = Super::RebuildWidget();

	if (!bBoundButtons)
	{
		if (RematchButton)
		{
			RematchButton->OnClicked.AddDynamic(this, &UNeonHUDWidget::OnRematchClicked);
		}
		if (MainMenuButton)
		{
			MainMenuButton->OnClicked.AddDynamic(this, &UNeonHUDWidget::OnMainMenuClicked);
		}
		bBoundButtons = true;
	}
	return Result;
}

void UNeonHUDWidget::BuildTree()
{
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Canvas;

	auto Place = [&](UWidget* W, const FVector2D& Anchor, const FVector2D& Offset)
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(W);
		S->SetAnchors(FAnchors(Anchor.X, Anchor.Y));
		S->SetAlignment(Anchor);
		S->SetPosition(Offset);
		S->SetAutoSize(true);
		return S;
	};

	// Shared pieces of the reference look: a dark glass fill behind a 2 px cyan edge, and condensed caps.
	const FLinearColor Glass = NeonUI::Void;
	const FLinearColor Ink = NeonUI::Ink;

	auto Frame = [&](const FMargin& Inset, float Radius, float EdgeWidth = 1.f, const FLinearColor* FillOverride = nullptr) -> UBorder*
	{
		UBorder* B = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		B->SetBrush(FSlateRoundedBoxBrush(FillOverride ? *FillOverride : Glass, 0.f, NeonUI::Accent, EdgeWidth));
		B->SetBrushColor(FLinearColor::White);
		B->SetPadding(Inset);
		(void)Radius;
		return B;
	};

	auto Face = [&](UTextBlock* T, const TCHAR* Typeface, int32 Size, int32 Spacing)
	{
		FSlateFontInfo F = FCoreStyle::GetDefaultFontStyle(Typeface, Size);
		F.LetterSpacing = Spacing;
		T->SetFont(F);
	};

	auto Sized = [&](float W, float H, UWidget* Child) -> USizeBox*
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(W);
		Box->SetHeightOverride(H);
		if (Child)
		{
			Box->AddChild(Child);
		}
		return Box;
	};

	// ---- Roster (top left): color chip with a dark core, condensed name, colored score line. ----
	{
		UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		StandingsPanel = Rows;
		Place(Rows, FVector2D(0.f, 0.f), FVector2D(30.f, 22.f));

		constexpr float ChipSize = 30.f;
		constexpr float LineWidth = 210.f;

		for (int32 i = 0; i < 4; ++i)
		{
			UVerticalBox* Entry = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			Rows->AddChildToVerticalBox(Entry)->SetPadding(FMargin(0.f, i == 0 ? 0.f : 9.f, 0.f, 0.f));

			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			Entry->AddChildToVerticalBox(Row);

			// Chip: SizeBox -> colored rounded border -> dark inner square. SetSwatchColor tints child 0.
			UBorder* ChipFill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			ChipFill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 6.f, FLinearColor(0.f, 0.f, 0.f, 0.6f), 1.f));
			ChipFill->SetBrushColor(NeonUI::Text);
			ChipFill->SetPadding(FMargin(8.f));
			UBorder* ChipCore = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			ChipCore->SetBrush(FSlateRoundedBoxBrush(Ink, 3.f));
			ChipCore->SetBrushColor(FLinearColor::White);
			ChipFill->AddChild(ChipCore);
			UWidget* Swatch = Sized(ChipSize, ChipSize, ChipFill);
			{
				UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Swatch);
				S->SetVerticalAlignment(VAlign_Center);
				S->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
			}

			UTextBlock* Name = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 16, NeonUI::Highlight, true);
			Face(Name, TEXT("BoldCondensed"), 16, 2);
			Name->SetJustification(ETextJustify::Left);
			Name->SetMinDesiredWidth(LineWidth);
			{
				UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Name);
				S->SetVerticalAlignment(VAlign_Center);
			}

			// Score line: a faint track in the player color with the bright fill on top of it.
			UOverlay* LineStack = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
			UBorder* Track = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Track->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 0.f));
			Track->SetBrushColor(FLinearColor(1.f, 1.f, 1.f, 0.22f));
			LineStack->AddChildToOverlay(Track);
			UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
			Bar->SetPercent(0.f);
			{
				FProgressBarStyle Style = Bar->GetWidgetStyle();
				Style.SetBackgroundImage(FSlateRoundedBoxBrush(FLinearColor::Transparent, 0.f));
				Style.SetFillImage(FSlateRoundedBoxBrush(FLinearColor::White, 0.f));
				Bar->SetWidgetStyle(Style);
			}
			LineStack->AddChildToOverlay(Bar);
			{
				UVerticalBoxSlot* S = Entry->AddChildToVerticalBox(Sized(LineWidth, 4.f, LineStack));
				S->SetPadding(FMargin(ChipSize + 10.f, 4.f, 0.f, 0.f));
				S->SetHorizontalAlignment(HAlign_Left);
			}

			StandingRows.Add(Entry);
			StandingSwatches.Add(Swatch);
			StandingNames.Add(Name);
			StandingBars.Add(Bar);
		}
	}

	// ---- Top center: framed speed readout, whole number large and the fraction small beside it. ----
	{
		UVerticalBox* Top = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Place(Top, FVector2D(0.5f, 0.f), FVector2D(0.f, 16.f));

		UBorder* SpeedFrame = Frame(FMargin(26.f, 2.f, 26.f, 6.f), 6.f);
		Top->AddChildToVerticalBox(SpeedFrame)->SetHorizontalAlignment(HAlign_Center);
		SpeedFrame->SetVisibility(ESlateVisibility::Collapsed);

		UHorizontalBox* Digits = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		SpeedFrame->AddChild(Digits);

		SpeedText = NeonUI::MakeText(WidgetTree, FText::FromString(TEXT("0")), 56, NeonUI::Highlight);
		Face(SpeedText, TEXT("Black"), 50, 1);
		SpeedText->SetJustification(ETextJustify::Right);
		SpeedText->SetMinDesiredWidth(84.f);
		Digits->AddChildToHorizontalBox(SpeedText)->SetVerticalAlignment(VAlign_Bottom);

		SpeedFracText = NeonUI::MakeText(WidgetTree, FText::FromString(TEXT(".00")), 24, NeonUI::Highlight);
		Face(SpeedFracText, TEXT("Black"), 26, 0);
		SpeedFracText->SetJustification(ETextJustify::Left);
		{
			UHorizontalBoxSlot* S = Digits->AddChildToHorizontalBox(SpeedFracText);
			S->SetVerticalAlignment(VAlign_Bottom);
			S->SetPadding(FMargin(2.f, 0.f, 0.f, 9.f));
		}

		UTextBlock* Caption = NeonUI::MakeText(WidgetTree, LOCTEXT("SpeedCaption", "KM/H"), 14, NeonUI::Accent);
		Face(Caption, TEXT("BoldCondensed"), 15, 2);
		Caption->SetVisibility(ESlateVisibility::Collapsed);
		{
			UVerticalBoxSlot* S = Top->AddChildToVerticalBox(Caption);
			S->SetHorizontalAlignment(HAlign_Center);
			S->SetPadding(FMargin(0.f, 5.f, 0.f, 0.f));
		}

	}

	// Perimeter warning sits under the round mark so the two never share a line.
	{
		CollapseText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 14, NeonUI::Warning);
		Face(CollapseText, TEXT("BoldCondensed"), 14, 2);
		Place(CollapseText, FVector2D(0.5f, 0.f), FVector2D(0.f, 72.f));
		CollapseText->SetVisibility(ESlateVisibility::Collapsed);
	}

	// ---- Top right: square arena map with one dot per bike. ----
	{
		constexpr float MapSize = 176.f;
		constexpr float MapPad = 6.f;
		UBorder* MapFrame = Frame(FMargin(MapPad), 3.f);
		MapPanel = MapFrame;
		UCanvasPanelSlot* FrameSlot = Place(MapFrame, FVector2D(1.f, 0.f), FVector2D(-30.f, 16.f));
		FrameSlot->SetAutoSize(false);
		FrameSlot->SetSize(FVector2D(MapSize + MapPad * 2.f, MapSize + MapPad * 2.f));

		UCanvasPanel* Map = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		MapFrame->AddChild(Map);

		// Arena outline, resized every tick to the arena's real aspect so the dots land where the bikes are.
		{
			UBorder* Field = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Field->SetBrush(FSlateRoundedBoxBrush(FLinearColor(NeonUI::Accent.R, NeonUI::Accent.G, NeonUI::Accent.B, 0.08f), 1.f, FLinearColor(NeonUI::Accent.R, NeonUI::Accent.G, NeonUI::Accent.B, 0.55f), 1.f));
			Field->SetBrushColor(FLinearColor::White);
			MapField = Field;
			UCanvasPanelSlot* S = Map->AddChildToCanvas(Field);
			S->SetAnchors(FAnchors(0.f, 0.f));
			S->SetAlignment(FVector2D(0.f, 0.f));
			S->SetAutoSize(false);
			S->SetPosition(FVector2D::ZeroVector);
			S->SetSize(FVector2D(MapSize, MapSize));
		}

		for (int32 i = 0; i < 4; ++i)
		{
			USizeBox* Dot = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			Dot->SetWidthOverride(9.f);
			Dot->SetHeightOverride(9.f);
			UBorder* Fill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Fill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 2.f, FLinearColor(0.f, 0.f, 0.f, 0.7f), 1.f));
			Fill->SetBrushColor(NeonUI::Accent);
			Dot->AddChild(Fill);

			UCanvasPanelSlot* S = Map->AddChildToCanvas(Dot);
			S->SetAnchors(FAnchors(0.f, 0.f));
			S->SetAlignment(FVector2D(0.5f, 0.5f));
			S->SetAutoSize(true);
			S->SetPosition(FVector2D(MapSize * 0.5f, MapSize * 0.5f));
			Dot->SetVisibility(ESlateVisibility::Collapsed);

			MapDots.Add(Dot);
			MapDotSlots.Add(S);
		}
	}

	// ---- Center stack: boot title, big line, small line, results ----
	UVerticalBox* Center = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Place(Center, FVector2D(0.5f, 0.42f), FVector2D::ZeroVector);

	BootTitle = NeonUI::MakeText(WidgetTree, LOCTEXT("BootTitle", "SYSTEM READY"), 36, NeonUI::Highlight);
	Center->AddChildToVerticalBox(BootTitle)->SetHorizontalAlignment(HAlign_Center);
	BootTitle->SetVisibility(ESlateVisibility::Collapsed);

	BootSub = NeonUI::MakeText(WidgetTree, LOCTEXT("BootSub", "LIGHTBIKE"), 14, NeonUI::Accent);
	{
		UVerticalBoxSlot* S = Center->AddChildToVerticalBox(BootSub);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 6.f, 0.f, 18.f));
	}
	BootSub->SetVisibility(ESlateVisibility::Collapsed);

	CenterBig = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 64, NeonUI::Accent);
	CenterBig->SetAutoWrapText(true);
	CenterBig->SetWrapTextAt(640.f);
	Center->AddChildToVerticalBox(CenterBig)->SetHorizontalAlignment(HAlign_Center);

	// Persistent round mark under the speed frame. Small, so it stays readable and out of the arena.
	{
		UVerticalBox* RoundStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		RoundPanel = RoundStack;
		Place(RoundStack, FVector2D(0.5f, 0.f), FVector2D(0.f, 22.f));

		RoundLabel = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 18, NeonUI::Accent);
		Face(RoundLabel, TEXT("BoldCondensed"), 18, 4);
		RoundStack->AddChildToVerticalBox(RoundLabel)->SetHorizontalAlignment(HAlign_Center);

		UBorder* TickFill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		TickFill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 0.f));
		TickFill->SetBrushColor(NeonUI::Accent);
		USizeBox* TickBox = Sized(28.f, 2.f, TickFill);
		RoundTick = TickBox;
		{
			UVerticalBoxSlot* S = RoundStack->AddChildToVerticalBox(TickBox);
			S->SetHorizontalAlignment(HAlign_Center);
			S->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		}
		RoundStack->SetVisibility(ESlateVisibility::Collapsed);
	}

	CenterSmall = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 18, NeonUI::Text, false);
	{
		UVerticalBoxSlot* S = Center->AddChildToVerticalBox(CenterSmall);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	{
		UBorder* PanelBorder = NeonUI::MakePanel(WidgetTree, FMargin(36.f, 18.f));
		ResultsPanel = PanelBorder;
		{
			UVerticalBoxSlot* S = Center->AddChildToVerticalBox(PanelBorder);
			S->SetHorizontalAlignment(HAlign_Center);
			S->SetPadding(FMargin(0.f, 28.f, 0.f, 0.f));
		}
		ResultsRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		PanelBorder->AddChild(ResultsRows);
		for (int32 i = 0; i < 4; ++i)
		{
			UTextBlock* Row = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 26, NeonUI::Text);
			Row->SetJustification(ETextJustify::Left);
			ResultsRows->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, i == 0 ? 0.f : 6.f, 0.f, 0.f));
			ResultsTexts.Add(Row);
		}
		PanelBorder->SetVisibility(ESlateVisibility::Collapsed);
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	ResultsButtons = Buttons;
	{
		UVerticalBoxSlot* S = Center->AddChildToVerticalBox(Buttons);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 28.f, 0.f, 0.f));
	}

	RematchButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Rematch", "RE-GRID"), 22);
	NeonUI::SetButtonSelected(RematchButton, true);
	Buttons->AddChildToHorizontalBox(RematchButton)->SetPadding(FMargin(12.f, 0.f));

	MainMenuButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("MainMenu", "ABORT"), 22);
	Buttons->AddChildToHorizontalBox(MainMenuButton)->SetPadding(FMargin(12.f, 0.f));

	Buttons->SetVisibility(ESlateVisibility::Collapsed);

	// ---- Powerup slot: bottom center stack of name, hint, timer bar ----
	{
		UBorder* PanelBorder = NeonUI::MakePanel(WidgetTree, FMargin(22.f, 12.f));
		PowerupPanel = PanelBorder;
		Place(PanelBorder, FVector2D(0.5f, 1.f), FVector2D(0.f, -150.f));

		// Fixed width so the bar has a size even when the texts are short.
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(340.f);
		PanelBorder->AddChild(Box);

		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Box->AddChild(Stack);

		PowerupName = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 32, NeonUI::Text);
		Stack->AddChildToVerticalBox(PowerupName)->SetHorizontalAlignment(HAlign_Center);

		PowerupHint = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 15, NeonUI::Dim);
		{
			UVerticalBoxSlot* S = Stack->AddChildToVerticalBox(PowerupHint);
			S->SetHorizontalAlignment(HAlign_Center);
			S->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
		}

		PowerupBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
		PowerupBar->SetPercent(0.f);
		NeonUI::StyleMeter(PowerupBar);
		PowerupBar->SetFillColorAndOpacity(NeonUI::Accent);
		{
			UVerticalBoxSlot* S = Stack->AddChildToVerticalBox(PowerupBar);
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
		}
		PowerupBar->SetVisibility(ESlateVisibility::Collapsed);

		PanelBorder->SetVisibility(ESlateVisibility::Collapsed);
	}

	// ---- Bottom gauge: one wide frame. Boost readout left, chevrons, diamond mark, chevrons, jump readout right. ----
	{
		constexpr float GaugeWidth = 1080.f;
		const FLinearColor GaugeGlass(0.00f, 0.00f, 0.00f, 0.82f);

		UBorder* GaugeFrame = Frame(FMargin(22.f, 10.f, 22.f, 12.f), 26.f, 2.f, &GaugeGlass);
		USizeBox* GaugeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		GaugeBox->SetWidthOverride(GaugeWidth);
		GaugeBox->AddChild(GaugeFrame);
		Place(GaugeBox, FVector2D(0.5f, 1.f), FVector2D(0.f, -22.f));

		UHorizontalBox* Gauge = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		GaugeFrame->AddChild(Gauge);

		// Meter slab: a framed bar that fills toward the center mark as the move recharges.
		auto Meter = [&](UProgressBar*& OutBar, bool bFromRight) -> UWidget*
		{
			UBorder* Slab = Frame(FMargin(2.f), 4.f, 1.5f, &Ink);
			OutBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
			OutBar->SetPercent(1.f);
			{
				FProgressBarStyle Style = OutBar->GetWidgetStyle();
				Style.SetBackgroundImage(FSlateRoundedBoxBrush(FLinearColor::Transparent, 0.f));
				Style.SetFillImage(FSlateRoundedBoxBrush(NeonUI::Accent, 2.f));
				OutBar->SetWidgetStyle(Style);
			}
			OutBar->SetFillColorAndOpacity(NeonUI::Accent);
			if (bFromRight)
			{
				OutBar->SetBarFillType(EProgressBarFillType::RightToLeft);
			}
			Slab->AddChild(OutBar);
			return Sized(200.f, 18.f, Slab);
		};

		auto KeyHint = [&](const FText& Key) -> UTextBlock*
		{
			UTextBlock* T = NeonUI::MakeText(WidgetTree, Key, 12, NeonUI::Dim);
			Face(T, TEXT("BoldCondensed"), 12, 2);
			return T;
		};

		auto Chevrons = [&](const TCHAR* Glyphs, bool bAlignRight) -> UWidget*
		{
			UTextBlock* T = NeonUI::MakeText(WidgetTree, FText::FromString(Glyphs), 30, NeonUI::Accent);
			Face(T, TEXT("Black"), 30, 6);
			T->SetJustification(bAlignRight ? ETextJustify::Right : ETextJustify::Left);
			return T;
		};

		// Boost column.
		{
			UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			UHorizontalBox* LabelRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			SprintLabel = NeonUI::MakeText(WidgetTree, LOCTEXT("Sprint", "BOOST"), 18, NeonUI::Accent);
			Face(SprintLabel, TEXT("BoldCondensed"), 20, 3);
			SprintLabel->SetJustification(ETextJustify::Left);
			LabelRow->AddChildToHorizontalBox(SprintLabel)->SetVerticalAlignment(VAlign_Bottom);
			{
				UHorizontalBoxSlot* S = LabelRow->AddChildToHorizontalBox(KeyHint(LOCTEXT("SprintKey", "SHIFT")));
				S->SetVerticalAlignment(VAlign_Bottom);
				S->SetPadding(FMargin(10.f, 0.f, 0.f, 3.f));
			}
			Col->AddChildToVerticalBox(LabelRow)->SetHorizontalAlignment(HAlign_Left);

			UProgressBar* Bar = nullptr;
			UWidget* Box = Meter(Bar, false);
			{
				UVerticalBoxSlot* S = Col->AddChildToVerticalBox(Box);
				S->SetHorizontalAlignment(HAlign_Left);
				S->SetPadding(FMargin(0.f, 5.f, 0.f, 0.f));
			}
			SprintBar = Bar;
			Gauge->AddChildToHorizontalBox(Col)->SetVerticalAlignment(VAlign_Bottom);
		}

		// Chevrons running in toward the mark.
		{
			UHorizontalBoxSlot* S = Gauge->AddChildToHorizontalBox(Chevrons(TEXT("\u203A\u203A\u203A"), true));
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 10.f, 18.f, 0.f));
		}

		// Diamond mark: a rotated square frame with a white diamond inside.
		{
			UOverlay* Mark = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());

			UBorder* Diamond = Frame(FMargin(0.f), 7.f, 2.f);
			Diamond->SetRenderTransformAngle(45.f);
			{
				UOverlaySlot* S = Mark->AddChildToOverlay(Sized(46.f, 46.f, Diamond));
				S->SetHorizontalAlignment(HAlign_Center);
				S->SetVerticalAlignment(VAlign_Center);
			}

			UBorder* Core = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Core->SetBrush(FSlateRoundedBoxBrush(NeonUI::Accent, 2.f));
			Core->SetBrushColor(FLinearColor::White);
			Core->SetRenderTransformAngle(45.f);
			{
				UOverlaySlot* S = Mark->AddChildToOverlay(Sized(20.f, 20.f, Core));
				S->SetHorizontalAlignment(HAlign_Center);
				S->SetVerticalAlignment(VAlign_Center);
			}

			UHorizontalBoxSlot* S = Gauge->AddChildToHorizontalBox(Sized(72.f, 72.f, Mark));
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
		}

		{
			UHorizontalBoxSlot* S = Gauge->AddChildToHorizontalBox(Chevrons(TEXT("\u2039\u2039\u2039"), false));
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(18.f, 10.f, 0.f, 0.f));
		}

		// Jump column, mirrored.
		{
			UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			JumpPanel = Col;
			UHorizontalBox* LabelRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			{
				UHorizontalBoxSlot* S = LabelRow->AddChildToHorizontalBox(KeyHint(LOCTEXT("JumpKey", "CTRL")));
				S->SetVerticalAlignment(VAlign_Bottom);
				S->SetPadding(FMargin(0.f, 0.f, 10.f, 3.f));
			}
			JumpLabel = NeonUI::MakeText(WidgetTree, LOCTEXT("Jump", "JUMP"), 18, NeonUI::Accent);
			Face(JumpLabel, TEXT("BoldCondensed"), 20, 3);
			JumpLabel->SetJustification(ETextJustify::Right);
			LabelRow->AddChildToHorizontalBox(JumpLabel)->SetVerticalAlignment(VAlign_Bottom);
			Col->AddChildToVerticalBox(LabelRow)->SetHorizontalAlignment(HAlign_Right);

			UProgressBar* Bar = nullptr;
			UWidget* Box = Meter(Bar, true);
			{
				UVerticalBoxSlot* S = Col->AddChildToVerticalBox(Box);
				S->SetHorizontalAlignment(HAlign_Right);
				S->SetPadding(FMargin(0.f, 5.f, 0.f, 0.f));
			}
			JumpBar = Bar;
			Gauge->AddChildToHorizontalBox(Col)->SetVerticalAlignment(VAlign_Bottom);
		}

		GaugeBox->SetVisibility(ESlateVisibility::Collapsed);
		SprintPanel = GaugeBox;
	}
}

// ---------------------------------------------------------------------------

void UNeonHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const UWorld* World = GetWorld();
	const ANeonGameState* GS = World ? World->GetGameState<ANeonGameState>() : nullptr;
	if (!GS)
	{
		return;
	}

	const APlayerController* PC = GetOwningPlayer();
	const ANeonPlayerState* LocalPS = PC ? PC->GetPlayerState<ANeonPlayerState>() : nullptr;

	if (GS->GetPhase() != LastPhase)
	{
		LastPhase = GS->GetPhase();
		TimeInPhase = 0.f;
	}
	else
	{
		TimeInPhase += InDeltaTime;
	}
	Pulse += InDeltaTime;

	UpdateStandings(*GS, LocalPS);
	UpdateTopCenter(*GS);
	UpdateCenter(*GS, LocalPS);
	UpdatePowerupPanel(*GS);
	UpdateRideHUD(*GS);
	UpdateMinimap(*GS);
	UpdateBoot(*GS);
}

const UNeonPowerupComponent* UNeonHUDWidget::FindLocalPowerups() const
{
	const APlayerController* PC = GetOwningPlayer();
	const ANeonVehiclePawn* Vehicle = PC ? Cast<ANeonVehiclePawn>(PC->GetPawn()) : nullptr;
	return Vehicle ? Vehicle->GetPowerups() : nullptr;
}

const UNeonMovementComponent* UNeonHUDWidget::FindLocalMovement() const
{
	const APlayerController* PC = GetOwningPlayer();
	const ANeonVehiclePawn* Vehicle = PC ? Cast<ANeonVehiclePawn>(PC->GetPawn()) : nullptr;
	return Vehicle ? Vehicle->GetMovement() : nullptr;
}

void UNeonHUDWidget::UpdateRideHUD(const ANeonGameState& GS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	const bool bInRound = Phase == ENeonMatchPhase::Countdown || Phase == ENeonMatchPhase::Playing;
	const UNeonMovementComponent* Movement = FindLocalMovement();
	const bool bShow = bInRound && Movement && Movement->IsAlive();

	auto ShowPanel = [](UWidget* Panel, bool bVisible)
	{
		if (Panel)
		{
			Panel->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
	};
	ShowPanel(SprintPanel, bShow);
	ShowPanel(JumpPanel, bInRound);

	if (SpeedText && SpeedFracText)
	{
		const float Kmh = (bInRound && Movement) ? Movement->GetCurrentSpeed() * 0.036f : 0.f;
		const int32 Whole = FMath::FloorToInt(Kmh);
		const int32 Frac = FMath::Clamp(FMath::RoundToInt((Kmh - Whole) * 100.f), 0, 99);
		SpeedText->SetText(FText::AsNumber(Whole));
		SpeedFracText->SetText(FText::FromString(FString::Printf(TEXT(".%02d"), Frac)));
		const FLinearColor SpeedColor = FLinearColor::LerpUsingHSV(NeonUI::Text, NeonUI::Highlight, FMath::Clamp(Kmh / 180.f, 0.f, 1.f));
		SpeedText->SetColorAndOpacity(FSlateColor(SpeedColor));
		SpeedFracText->SetColorAndOpacity(FSlateColor(SpeedColor));
	}

	if (!bShow || !Movement)
	{
		return;
	}

	auto Tint = [](const FLinearColor& C, float A) { return FLinearColor(C.R, C.G, C.B, A); };

	// Boost: navy track, electric fill. Brighter when full or burning. Dimmer when empty.
	{
		const float Meter = Movement->GetSprintMeter();
		const bool bBurning = Movement->IsSprinting();
		const bool bReady = Meter >= 0.98f;
		if (SprintBar)
		{
			SprintBar->SetPercent(Meter);
			SprintBar->SetFillColorAndOpacity(bBurning || bReady ? NeonUI::Bright : Tint(NeonUI::Accent, FMath::Lerp(0.35f, 1.f, Meter)));
		}
		if (SprintLabel)
		{
			SprintLabel->SetColorAndOpacity(FSlateColor(bReady || bBurning ? NeonUI::Highlight : NeonUI::Accent));
		}
	}

	// Jump: full cyan when a jump is available, dimmer while it recharges or while airborne.
	{
		const float Meter = Movement->GetJumpMeter();
		const bool bReady = Movement->CanJump();
		if (JumpBar)
		{
			JumpBar->SetPercent(Meter);
			JumpBar->SetFillColorAndOpacity(Tint(NeonUI::Accent, bReady ? 1.f : 0.55f));
		}
		if (JumpLabel)
		{
			JumpLabel->SetColorAndOpacity(FSlateColor(bReady ? NeonUI::Accent : NeonUI::Dim));
		}
	}
}

void UNeonHUDWidget::UpdateMinimap(const ANeonGameState& GS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	const bool bShow = Phase == ENeonMatchPhase::Countdown || Phase == ENeonMatchPhase::Playing || Phase == ENeonMatchPhase::RoundResults;
	if (MapPanel)
	{
		MapPanel->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (!bShow)
	{
		return;
	}

	const ANeonArena* Arena = FindArena();
	const UWorld* World = GetWorld();
	const UNeonCollisionSystem* Collision = World ? World->GetSubsystem<UNeonCollisionSystem>() : nullptr;
	if (!Arena || !Collision)
	{
		return;
	}

	const FBox2D Bounds = Arena->GetBounds();
	const FVector2D Span = Bounds.GetSize();
	constexpr float MapSize = 176.f; // matches the frame built in BuildTree

	// Letterbox the arena inside the square map so a wide arena stays wide.
	const float Scale = (Span.X > 1.f && Span.Y > 1.f) ? FMath::Min(MapSize / Span.X, MapSize / Span.Y) : 0.f;
	const FVector2D Field(Scale > 0.f ? Span.X * Scale : MapSize, Scale > 0.f ? Span.Y * Scale : MapSize);
	const FVector2D FieldOrigin((MapSize - Field.X) * 0.5f, (MapSize - Field.Y) * 0.5f);
	if (MapField)
	{
		if (UCanvasPanelSlot* FieldSlot = Cast<UCanvasPanelSlot>(MapField->Slot))
		{
			FieldSlot->SetPosition(FieldOrigin);
			FieldSlot->SetSize(Field);
		}
	}

	for (UWidget* Dot : MapDots)
	{
		if (Dot)
		{
			Dot->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
	{
		const UNeonMovementComponent* M = Weak.Get();
		if (!M)
		{
			continue;
		}
		const int32 PlayerSlot = M->GetPlayerSlot();
		if (!MapDots.IsValidIndex(PlayerSlot) || !MapDotSlots.IsValidIndex(PlayerSlot))
		{
			continue;
		}
		const float U = Span.X > 1.f ? (M->GetSimPosition().X - Bounds.Min.X) / Span.X : 0.5f;
		const float V = Span.Y > 1.f ? (M->GetSimPosition().Y - Bounds.Min.Y) / Span.Y : 0.5f;
		if (UCanvasPanelSlot* DotSlot = MapDotSlots[PlayerSlot])
		{
			DotSlot->SetPosition(FieldOrigin + FVector2D(FMath::Clamp(U, 0.f, 1.f) * Field.X, (1.f - FMath::Clamp(V, 0.f, 1.f)) * Field.Y));
		}
		if (UWidget* Dot = MapDots[PlayerSlot])
		{
			Dot->SetVisibility(M->IsAlive() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (USizeBox* Box = Cast<USizeBox>(Dot))
			{
				if (UBorder* Fill = Cast<UBorder>(Box->GetChildAt(0)))
				{
					Fill->SetBrushColor(ColorForSlot(GS, PlayerSlot));
				}
			}
		}
	}
}

void UNeonHUDWidget::UpdateBoot(const ANeonGameState& GS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	const bool bCountdown = Phase == ENeonMatchPhase::Countdown;
	const bool bFade = Phase == ENeonMatchPhase::Playing && TimeInPhase < 1.15f;
	const bool bShow = bCountdown || bFade;
	const float Opacity = bCountdown ? (0.82f + 0.18f * FMath::Sin(Pulse * 5.f)) : FMath::Clamp(1.f - TimeInPhase / 1.15f, 0.f, 1.f);
	const float Scale = bCountdown ? (1.04f + 0.06f * FMath::Sin(Pulse * 3.f)) : 1.f;

	auto Apply = [&](UWidget* W)
	{
		if (!W)
		{
			return;
		}
		W->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		W->SetRenderOpacity(Opacity);
		W->SetRenderScale(FVector2D(Scale));
	};
	Apply(BootTitle);
	Apply(BootSub);

	const float Bracket = (bCountdown || Phase == ENeonMatchPhase::Playing)
		? (0.35f + 0.25f * (0.5f + 0.5f * FMath::Sin(Pulse * 4.f)))
		: 0.f;
	for (UWidget* Tick : CornerBrackets)
	{
		if (Tick)
		{
			Tick->SetRenderOpacity(Bracket);
			Tick->SetVisibility(Bracket > 0.02f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
	}
}

const ANeonArena* UNeonHUDWidget::FindArena() const
{
	return ANeonArena::FindArena(GetWorld());
}

FText UNeonHUDWidget::NameForSlot(const ANeonGameState& GS, int32 SlotIndex) const
{
	if (const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex))
	{
		return FText::FromString(PS->GetPlayerName());
	}
	return FText::Format(LOCTEXT("SlotName", "P{0}"), SlotIndex + 1);
}

FLinearColor UNeonHUDWidget::ColorForSlot(const ANeonGameState& GS, int32 SlotIndex) const
{
	if (const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex))
	{
		return PS->GetColor();
	}
	return NeonUI::Text;
}

// ---------------------------------------------------------------------------
// Standings / top bar
// ---------------------------------------------------------------------------

void UNeonHUDWidget::UpdateStandings(const ANeonGameState& GS, const ANeonPlayerState* LocalPS)
{
	const bool bInRound = GS.GetPhase() == ENeonMatchPhase::Playing || GS.GetPhase() == ENeonMatchPhase::RoundResults;
	int32 Shown = 0;

	for (int32 SlotIndex = 0; SlotIndex < StandingRows.Num(); ++SlotIndex)
	{
		UWidget* Row = StandingRows[SlotIndex];
		const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex);
		if (!Row)
		{
			continue;
		}
		if (!PS)
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		++Shown;

		const bool bOut = bInRound && !PS->IsAlive();
		const FLinearColor PlayerColor = PS->GetColor();
		const FLinearColor Chip = bOut ? NeonUI::Dim : PlayerColor;

		if (StandingSwatches.IsValidIndex(SlotIndex))
		{
			NeonUI::SetSwatchColor(StandingSwatches[SlotIndex], Chip);
		}
		if (UTextBlock* Name = StandingNames.IsValidIndex(SlotIndex) ? StandingNames[SlotIndex].Get() : nullptr)
		{
			Name->SetText(FText::FromString(FString::Printf(TEXT("[ P%d ]  %s"), SlotIndex + 1, *PS->GetPlayerName().ToUpper())));
			Name->SetColorAndOpacity(FSlateColor(bOut ? NeonUI::Dim : PlayerColor));
		}
		if (UProgressBar* Bar = StandingBars.IsValidIndex(SlotIndex) ? StandingBars[SlotIndex].Get() : nullptr)
		{
			const float Limit = FMath::Max(GS.GetScoreLimit(), 1);
			Bar->SetPercent(FMath::Clamp(static_cast<float>(PS->GetRoundWins()) / Limit, 0.f, 1.f));
			Bar->SetFillColorAndOpacity(bOut ? NeonUI::Dim : PlayerColor);
			if (UOverlay* Stack = Cast<UOverlay>(Bar->GetParent()))
			{
				if (UBorder* Track = Cast<UBorder>(Stack->GetChildAt(0)))
				{
					Track->SetBrushColor(FLinearColor(NeonUI::Navy.R, NeonUI::Navy.G, NeonUI::Navy.B, 0.85f));
				}
			}
		}
	}

	if (StandingsPanel)
	{
		StandingsPanel->SetVisibility(Shown > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UNeonHUDWidget::UpdateTopCenter(const ANeonGameState& GS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();

	if (RoundText)
	{
		if (GS.GetRoundNumber() > 0)
		{
			const ANeonArena* MapArena = FindArena();
			const FText MapName = MapArena ? MapArena->GetLayoutName() : FText::GetEmpty();
			RoundText->SetText(FText::Format(LOCTEXT("Round", "CYCLE {0}   //   {1}   //   FIRST TO {2}"), GS.GetRoundNumber(), MapName, GS.GetScoreLimit()));
			RoundText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			RoundText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	// Closing walls (D27)
	if (CollapseText)
	{
		FText Line;
		float Alpha = 1.f;
		const ANeonArena* Arena = FindArena();
		if (Arena && Arena->IsCollapseActive() && Phase == ENeonMatchPhase::Playing)
		{
			const float Progress = Arena->GetCollapseProgress();
			if (Progress > 0.f && Progress < 1.f)
			{
				Line = LOCTEXT("WallsClosing", "PERIMETER CLOSING");
				Alpha = 0.65f + 0.35f * FMath::Sin(Pulse * 6.f);
			}
		}

		if (Line.IsEmpty())
		{
			CollapseText->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			CollapseText->SetText(Line);
			FLinearColor C = NeonUI::Bright;
			C.A = Alpha;
			CollapseText->SetColorAndOpacity(FSlateColor(C));
			CollapseText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
	}

	if (AliveText)
	{
		const UWorld* World = GetWorld();
		const UNeonCollisionSystem* Collision = World ? World->GetSubsystem<UNeonCollisionSystem>() : nullptr;
		const bool bShow = Collision && (Phase == ENeonMatchPhase::Playing || Phase == ENeonMatchPhase::Countdown || Phase == ENeonMatchPhase::RoundResults);
		if (bShow)
		{
			const int32 Alive = Collision->CountAliveVehicles();
			AliveText->SetText(FText::Format(LOCTEXT("Alive", "{0} LIVE"), Alive));
			AliveText->SetColorAndOpacity(FSlateColor(Alive <= 2 && Phase == ENeonMatchPhase::Playing ? NeonUI::Accent : NeonUI::Text));
		}
		if (UWidget* Parent = AliveText->GetParent())
		{
			Parent->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
	}
}

// ---------------------------------------------------------------------------
// Powerup slot
// ---------------------------------------------------------------------------

void UNeonHUDWidget::UpdatePowerupPanel(const ANeonGameState& GS)
{
	if (!PowerupPanel)
	{
		return;
	}

	const UNeonPowerupComponent* Powerups = FindLocalPowerups();
	const bool bInRound = GS.GetPhase() == ENeonMatchPhase::Playing || GS.GetPhase() == ENeonMatchPhase::Countdown;
	if (!Powerups || !bInRound)
	{
		PowerupPanel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	const ENeonPowerupType Held = Powerups->GetHeld();
	const ENeonPowerupType Active = Powerups->GetActiveType();
	const bool bEMPd = Powerups->IsEMPd();

	FText Name;
	FText Hint;
	FLinearColor Color = NeonUI::Highlight;
	float Percent = -1.f; // <0 hides the bar

	if (Active != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Active);
		Color = NeonUI::Bright;
		// An armed shield with no time limit reports 0 seconds remaining; everything else counts down.
		const bool bTimed = Powerups->GetActiveSecondsRemaining() > 0.f;
		Hint = bTimed ? LOCTEXT("PowerupActive", "RUNNING") : LOCTEXT("PowerupArmed", "ARMED");
		if (bTimed)
		{
			Percent = Powerups->GetActiveFraction();
		}
	}
	else if (Held != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Held);
		Color = NeonUI::Highlight;
		Hint = LOCTEXT("PowerupPress", "READY");
	}

	if (bEMPd)
	{
		// The jam warning takes the hint line; the bar shows how long it lasts when nothing else is active.
		Hint = LOCTEXT("Jammed", "STEERING  //  JAMMED");
		if (Name.IsEmpty())
		{
			Name = NeonPowerups::DisplayName(ENeonPowerupType::EMP);
			Color = NeonUI::Bright;
			Percent = Powerups->GetEMPFraction();
		}
	}

	if (Name.IsEmpty())
	{
		PowerupPanel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	PowerupPanel->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (PowerupName)
	{
		PowerupName->SetText(Name);
		PowerupName->SetColorAndOpacity(FSlateColor(Color));
	}
	if (PowerupHint)
	{
		PowerupHint->SetText(Hint);
		PowerupHint->SetColorAndOpacity(FSlateColor(bEMPd ? NeonUI::Bright : NeonUI::Dim));
	}
	if (PowerupBar)
	{
		PowerupBar->SetVisibility(Percent >= 0.f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		PowerupBar->SetPercent(FMath::Clamp(Percent, 0.f, 1.f));
		PowerupBar->SetFillColorAndOpacity(Color);
	}
}

// ---------------------------------------------------------------------------
// Center
// ---------------------------------------------------------------------------

void UNeonHUDWidget::SetCenter(const FText& Big, const FText& Small, const FLinearColor& BigColor, const FLinearColor& SmallColor)
{
	if (CenterBig)
	{
		CenterBig->SetText(Big);
		CenterBig->SetColorAndOpacity(FSlateColor(BigColor));
		CenterBig->SetVisibility(Big.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (CenterSmall)
	{
		CenterSmall->SetText(Small);
		CenterSmall->SetColorAndOpacity(FSlateColor(SmallColor));
		CenterSmall->SetVisibility(Small.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

void UNeonHUDWidget::UpdateResultsStandings(const ANeonGameState& GS, const ANeonPlayerState* LocalPS, bool bShow)
{
	if (!ResultsPanel)
	{
		return;
	}
	if (!bShow)
	{
		ResultsPanel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	TArray<const ANeonPlayerState*> Sorted;
	for (int32 SlotIndex = 0; SlotIndex < 4; ++SlotIndex)
	{
		if (const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex))
		{
			Sorted.Add(PS);
		}
	}
	Sorted.StableSort([](const ANeonPlayerState& A, const ANeonPlayerState& B) { return A.GetRoundWins() > B.GetRoundWins(); });

	for (int32 i = 0; i < ResultsTexts.Num(); ++i)
	{
		UTextBlock* Row = ResultsTexts[i];
		if (!Row)
		{
			continue;
		}
		if (!Sorted.IsValidIndex(i))
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const ANeonPlayerState* PS = Sorted[i];
		const FString Line = FString::Printf(TEXT("%02d   %s     %d"), i + 1, *PS->GetPlayerName().ToUpper(), PS->GetRoundWins());
		Row->SetText(FText::FromString(Line));
		FLinearColor RowColor = PS->GetColor();
		if (PS == LocalPS)
		{
			RowColor = FMath::Lerp(RowColor, FLinearColor::White, 0.25f);
		}
		Row->SetColorAndOpacity(FSlateColor(RowColor));
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	ResultsPanel->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UNeonHUDWidget::UpdateCenter(const ANeonGameState& GS, const ANeonPlayerState* LocalPS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	bool bShowButtons = false;
	bool bShowStandings = false;
	bool bShowRound = false;

	// Spectator line shared by Playing and RoundResults.
	auto SetBigSize = [&](int32 Size)
	{
		if (!CenterBig)
		{
			return;
		}
		FSlateFontInfo Font = CenterBig->GetFont();
		Font.Size = Size;
		CenterBig->SetFont(Font);
	};

	const ANeonPlayerController* ViewPC = Cast<ANeonPlayerController>(GetOwningPlayer());
	const ANeonVehiclePawn* Watch = ViewPC ? ViewPC->GetSpectateTarget() : nullptr;
	const FText WatchLine = Watch
		? FText::Format(LOCTEXT("Watching", "WATCHING {0}    A / D"), NameForSlot(GS, Watch->GetPlayerSlot()))
		: LOCTEXT("SpectateHint", "A / D SWITCHES CAMERA");

	switch (Phase)
	{
	case ENeonMatchPhase::Countdown:
	{
		const float Remaining = GS.GetPhaseTimeRemaining();
		const int32 Count = FMath::CeilToInt(Remaining);
		SetCenter(Count > 0 ? FText::FromString(FString::Printf(TEXT("%02d"), Count)) : LOCTEXT("Go", "ENGAGE"), FText::GetEmpty(), NeonUI::Highlight, NeonUI::Text);
		SetBigSize(64);
		bShowRound = true;
		break;
	}
	case ENeonMatchPhase::Playing:
	{
		const bool bDead = LocalPS && !LocalPS->IsAlive();
		bShowRound = true;
		if (TimeInPhase < GoDisplaySeconds)
		{
			SetCenter(LOCTEXT("Go", "ENGAGE"), bDead ? WatchLine : FText::GetEmpty(), NeonUI::Highlight, NeonUI::Dim);
			SetBigSize(48);
		}
		else if (bDead)
		{
			SetCenter(LOCTEXT("Eliminated", "ELIMINATED"), WatchLine, NeonUI::Highlight, NeonUI::Dim);
			SetBigSize(28);
		}
		else
		{
			SetCenter(FText::GetEmpty(), FText::GetEmpty(), NeonUI::Accent, NeonUI::Dim);
			SetBigSize(64);
		}
		break;
	}
	case ENeonMatchPhase::RoundResults:
	{
		const int32 Winner = GS.GetRoundWinnerSlot();
		if (Winner >= 0)
		{
			const bool bYou = LocalPS && LocalPS->GetSlot() == Winner;
			SetCenter(
				bYou ? LOCTEXT("YouWinRound", "YOU WIN THE ROUND") : FText::Format(LOCTEXT("RoundWin", "{0} WINS THE ROUND"), NameForSlot(GS, Winner)),
				FText::GetEmpty(),
				NeonUI::Highlight, NeonUI::Text);
			SetBigSize(28);
			bShowRound = true;
		}
		else
		{
			SetCenter(LOCTEXT("NoSurvivors", "NO SURVIVORS"), LOCTEXT("NoPoint", "NO POINT AWARDED"), NeonUI::Text, NeonUI::Dim);
			SetBigSize(28);
			bShowRound = true;
		}
		break;
	}
	case ENeonMatchPhase::MatchResults:
	{
		const int32 Winner = GS.GetMatchWinnerSlot();
		const bool bYou = LocalPS && LocalPS->GetSlot() == Winner;
		SetCenter(
			bYou ? LOCTEXT("YouWinMatch", "YOU WIN THE MATCH") : FText::Format(LOCTEXT("MatchWin", "{0} WINS THE MATCH"), NameForSlot(GS, Winner)),
			LOCTEXT("FinalStandings", "MATCH COMPLETE"),
			NeonUI::Highlight, NeonUI::Dim);
		bShowButtons = true;
		bShowStandings = true;
		bShowRound = true;
		SetBigSize(28);
		break;
	}
	case ENeonMatchPhase::MainMenu:
	default:
		SetCenter(FText::GetEmpty(), FText::GetEmpty(), NeonUI::Accent, NeonUI::Text);
		break;
	}

	UpdateResultsStandings(GS, LocalPS, bShowStandings);
	if (ResultsButtons)
	{
		ResultsButtons->SetVisibility(bShowButtons ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (RoundLabel)
	{
		RoundLabel->SetText(FText::Format(LOCTEXT("RoundCenter", "ROUND {0}"), FMath::Max(GS.GetRoundNumber(), 1)));
	}
	if (RoundPanel)
	{
		RoundPanel->SetRenderOpacity(1.f);
		RoundPanel->SetVisibility(bShowRound ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UNeonHUDWidget::OnRematchClicked()
{
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestRematch();
	}
}

void UNeonHUDWidget::OnMainMenuClicked()
{
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestReturnToMainMenu();
	}
}

#undef LOCTEXT_NAMESPACE
