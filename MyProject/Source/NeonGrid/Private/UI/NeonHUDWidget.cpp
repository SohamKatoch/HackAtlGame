#include "UI/NeonHUDWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Core/NeonGameState.h"
#include "Core/NeonPlayerController.h"
#include "Core/NeonPlayerState.h"
#include "Powerups/NeonPowerupComponent.h"
#include "UI/NeonUIStyle.h"
#include "Vehicle/NeonVehiclePawn.h"

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

	auto Corner = [&](TObjectPtr<UTextBlock>& Out, const FVector2D& Anchor, const FVector2D& Offset, ETextJustify::Type Justify)
	{
		Out = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 30, NeonUI::Text);
		Out->SetJustification(Justify);
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Out);
		S->SetAnchors(FAnchors(Anchor.X, Anchor.Y));
		S->SetAlignment(Anchor);
		S->SetPosition(Offset);
		S->SetAutoSize(true);
	};

	Corner(Score0, FVector2D(0.f, 0.f), FVector2D(40.f, 30.f), ETextJustify::Left);
	Corner(Score1, FVector2D(1.f, 0.f), FVector2D(-40.f, 30.f), ETextJustify::Right);
	Corner(Score2, FVector2D(0.f, 1.f), FVector2D(40.f, -30.f), ETextJustify::Left);
	Corner(Score3, FVector2D(1.f, 1.f), FVector2D(-40.f, -30.f), ETextJustify::Right);

	RoundText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 26, NeonUI::Dim);
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(RoundText);
		S->SetAnchors(FAnchors(0.5f, 0.f));
		S->SetAlignment(FVector2D(0.5f, 0.f));
		S->SetPosition(FVector2D(0.f, 30.f));
		S->SetAutoSize(true);
	}

	// Center stack: big line, small line, results buttons.
	UVerticalBox* Center = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Center);
		S->SetAnchors(FAnchors(0.5f, 0.5f));
		S->SetAlignment(FVector2D(0.5f, 0.5f));
		S->SetPosition(FVector2D(0.f, -40.f));
		S->SetAutoSize(true);
	}

	CenterBig = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 84, NeonUI::Accent);
	Center->AddChildToVerticalBox(CenterBig)->SetHorizontalAlignment(HAlign_Center);

	CenterSmall = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 30, NeonUI::Text);
	{
		UVerticalBoxSlot* S = Center->AddChildToVerticalBox(CenterSmall);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	ResultsButtons = Buttons;
	{
		UVerticalBoxSlot* S = Center->AddChildToVerticalBox(Buttons);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 36.f, 0.f, 0.f));
	}

	RematchButton = NeonUI::MakeButton(WidgetTree, NSLOCTEXT("NeonGrid", "Rematch", "REMATCH"), 26);
	Buttons->AddChildToHorizontalBox(RematchButton)->SetPadding(FMargin(12.f, 0.f));

	MainMenuButton = NeonUI::MakeButton(WidgetTree, NSLOCTEXT("NeonGrid", "MainMenu", "MAIN MENU"), 26);
	Buttons->AddChildToHorizontalBox(MainMenuButton)->SetPadding(FMargin(12.f, 0.f));

	Buttons->SetVisibility(ESlateVisibility::Collapsed);

	// Powerup slot: bottom center stack of name, hint, timer bar.
	// Fixed width so the bar has a size even when the texts are short.
	USizeBox* PanelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	PanelBox->SetWidthOverride(360.f);
	PowerupPanel = PanelBox;
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(PanelBox);
		S->SetAnchors(FAnchors(0.5f, 1.f));
		S->SetAlignment(FVector2D(0.5f, 1.f));
		S->SetPosition(FVector2D(0.f, -40.f));
		S->SetAutoSize(true);
	}

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PanelBox->AddChild(Stack);

	PowerupName = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 34, NeonUI::Text);
	Stack->AddChildToVerticalBox(PowerupName)->SetHorizontalAlignment(HAlign_Center);

	PowerupHint = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 16, NeonUI::Dim);
	{
		UVerticalBoxSlot* S = Stack->AddChildToVerticalBox(PowerupHint);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}

	PowerupBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
	PowerupBar->SetPercent(0.f);
	PowerupBar->SetFillColorAndOpacity(NeonUI::Accent);
	{
		UVerticalBoxSlot* S = Stack->AddChildToVerticalBox(PowerupBar);
		S->SetHorizontalAlignment(HAlign_Fill);
		S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
	PowerupBar->SetVisibility(ESlateVisibility::Collapsed);

	PanelBox->SetVisibility(ESlateVisibility::Collapsed);
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

	UpdateScores(*GS, LocalPS);
	UpdateCenter(*GS, LocalPS, InDeltaTime);
	UpdatePowerupPanel(*GS);
}

const UNeonPowerupComponent* UNeonHUDWidget::FindLocalPowerups() const
{
	const APlayerController* PC = GetOwningPlayer();
	const ANeonVehiclePawn* Vehicle = PC ? Cast<ANeonVehiclePawn>(PC->GetPawn()) : nullptr;
	return Vehicle ? Vehicle->GetPowerups() : nullptr;
}

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
	FLinearColor Color = NeonUI::Text;
	float Percent = -1.f; // <0 hides the bar

	if (Active != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Active);
		Color = NeonPowerups::Color(Active);
		// An armed shield with no time limit reports 0 seconds remaining; everything else counts down.
		const bool bTimed = Powerups->GetActiveSecondsRemaining() > 0.f;
		Hint = bTimed ? NSLOCTEXT("NeonGrid", "PowerupActive", "ACTIVE") : NSLOCTEXT("NeonGrid", "PowerupArmed", "ARMED");
		if (bTimed)
		{
			Percent = Powerups->GetActiveFraction();
		}
	}
	else if (Held != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Held);
		Color = NeonPowerups::Color(Held);
		Hint = NSLOCTEXT("NeonGrid", "PowerupPress", "PRESS SPACE");
	}

	if (bEMPd)
	{
		// The jam warning takes the hint line; the bar shows how long it lasts when nothing else is active.
		Hint = NSLOCTEXT("NeonGrid", "Jammed", "STEERING JAMMED");
		if (Name.IsEmpty())
		{
			Name = NeonPowerups::DisplayName(ENeonPowerupType::EMP);
			Color = NeonPowerups::Color(ENeonPowerupType::EMP);
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
		PowerupHint->SetColorAndOpacity(FSlateColor(bEMPd ? NeonPowerups::Color(ENeonPowerupType::EMP) : NeonUI::Dim));
	}
	if (PowerupBar)
	{
		PowerupBar->SetVisibility(Percent >= 0.f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		PowerupBar->SetPercent(FMath::Clamp(Percent, 0.f, 1.f));
		PowerupBar->SetFillColorAndOpacity(Color);
	}
}

FText UNeonHUDWidget::NameForSlot(const ANeonGameState& GS, int32 SlotIndex) const
{
	if (const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex))
	{
		return FText::FromString(PS->GetPlayerName());
	}
	return FText::Format(NSLOCTEXT("NeonGrid", "SlotName", "P{0}"), SlotIndex + 1);
}

FLinearColor UNeonHUDWidget::ColorForSlot(const ANeonGameState& GS, int32 SlotIndex) const
{
	if (const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex))
	{
		return PS->GetColor();
	}
	return NeonUI::Text;
}

void UNeonHUDWidget::UpdateScores(const ANeonGameState& GS, const ANeonPlayerState* LocalPS)
{
	UTextBlock* Entries[4] = { Score0, Score1, Score2, Score3 };

	for (int32 SlotIndex = 0; SlotIndex < 4; ++SlotIndex)
	{
		UTextBlock* Entry = Entries[SlotIndex];
		if (!Entry)
		{
			continue;
		}

		const ANeonPlayerState* PS = GS.FindPlayerStateBySlot(SlotIndex);
		if (!PS)
		{
			Entry->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}

		Entry->SetVisibility(ESlateVisibility::HitTestInvisible);

		FString Line = FString::Printf(TEXT("%s  %d"), *PS->GetPlayerName(), PS->GetRoundWins());
		if (PS == LocalPS)
		{
			Line += TEXT("  (YOU)");
		}
		Entry->SetText(FText::FromString(Line));

		FLinearColor Color = PS->GetColor();
		if (!PS->IsAlive() && GS.GetPhase() == ENeonMatchPhase::Playing)
		{
			Color *= 0.35f;
			Color.A = 1.f;
		}
		Entry->SetColorAndOpacity(FSlateColor(Color));
	}

	if (RoundText)
	{
		if (GS.GetRoundNumber() > 0)
		{
			RoundText->SetText(FText::Format(NSLOCTEXT("NeonGrid", "Round", "ROUND {0}   FIRST TO {1}"), GS.GetRoundNumber(), GS.GetScoreLimit()));
			RoundText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			RoundText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UNeonHUDWidget::SetCenter(const FText& Big, const FText& Small, const FLinearColor& BigColor)
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
		CenterSmall->SetVisibility(Small.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

void UNeonHUDWidget::UpdateCenter(const ANeonGameState& GS, const ANeonPlayerState* LocalPS, float /*DeltaTime*/)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	bool bShowButtons = false;

	switch (Phase)
	{
	case ENeonMatchPhase::Countdown:
	{
		const float Remaining = GS.GetPhaseTimeRemaining();
		const int32 Count = FMath::CeilToInt(Remaining);
		if (Count > 0)
		{
			SetCenter(FText::AsNumber(Count), FText::GetEmpty(), NeonUI::Accent);
		}
		else
		{
			SetCenter(NSLOCTEXT("NeonGrid", "Go", "GO"), FText::GetEmpty(), NeonUI::Accent);
		}
		break;
	}
	case ENeonMatchPhase::Playing:
	{
		FText Big = TimeInPhase < GoDisplaySeconds ? NSLOCTEXT("NeonGrid", "Go", "GO") : FText::GetEmpty();
		FText Small = FText::GetEmpty();
		if (LocalPS && !LocalPS->IsAlive())
		{
			Small = NSLOCTEXT("NeonGrid", "Eliminated", "ELIMINATED");
		}
		SetCenter(Big, Small, NeonUI::Accent);
		break;
	}
	case ENeonMatchPhase::RoundResults:
	{
		const int32 Winner = GS.GetRoundWinnerSlot();
		if (Winner >= 0)
		{
			SetCenter(
				FText::Format(NSLOCTEXT("NeonGrid", "RoundWin", "{0} WINS THE ROUND"), NameForSlot(GS, Winner)),
				FText::GetEmpty(),
				ColorForSlot(GS, Winner));
		}
		else
		{
			SetCenter(NSLOCTEXT("NeonGrid", "NoSurvivors", "NO SURVIVORS"), NSLOCTEXT("NeonGrid", "NoPoint", "No point awarded"), NeonUI::Text);
		}
		break;
	}
	case ENeonMatchPhase::MatchResults:
	{
		const int32 Winner = GS.GetMatchWinnerSlot();
		SetCenter(
			FText::Format(NSLOCTEXT("NeonGrid", "MatchWin", "{0} WINS THE MATCH"), NameForSlot(GS, Winner)),
			FText::GetEmpty(),
			ColorForSlot(GS, Winner));
		bShowButtons = true;
		break;
	}
	case ENeonMatchPhase::MainMenu:
	default:
		SetCenter(FText::GetEmpty(), FText::GetEmpty(), NeonUI::Accent);
		break;
	}

	if (ResultsButtons)
	{
		ResultsButtons->SetVisibility(bShowButtons ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
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
