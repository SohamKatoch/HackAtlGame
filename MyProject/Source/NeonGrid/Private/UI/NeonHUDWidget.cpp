#include "UI/NeonHUDWidget.h"

#include "Arena/NeonArena.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
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
#include "Trail/NeonCollisionSystem.h"
#include "UI/NeonUIStyle.h"
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

	// ---- Standings (top left) ----
	{
		UBorder* PanelBorder = NeonUI::MakePanel(WidgetTree, FMargin(18.f, 12.f));
		StandingsPanel = PanelBorder;
		Place(PanelBorder, FVector2D(0.f, 0.f), FVector2D(30.f, 30.f));

		UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		PanelBorder->AddChild(Rows);

		for (int32 i = 0; i < 4; ++i)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			Rows->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, i == 0 ? 0.f : 6.f, 0.f, 0.f));

			UWidget* Swatch = NeonUI::MakeSwatch(WidgetTree, NeonUI::Text, 16.f);
			{
				UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Swatch);
				S->SetVerticalAlignment(VAlign_Center);
				S->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
			}

			UTextBlock* Name = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 22, NeonUI::Text);
			Name->SetJustification(ETextJustify::Left);
			Name->SetMinDesiredWidth(170.f);
			{
				UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Name);
				S->SetVerticalAlignment(VAlign_Center);
				S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}

			UTextBlock* Wins = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 24, NeonUI::Accent);
			Wins->SetJustification(ETextJustify::Right);
			Wins->SetMinDesiredWidth(34.f);
			{
				UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Wins);
				S->SetVerticalAlignment(VAlign_Center);
				S->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
			}

			StandingRows.Add(Row);
			StandingSwatches.Add(Swatch);
			StandingNames.Add(Name);
			StandingWins.Add(Wins);
		}
	}

	// ---- Top center: round line and the walls warning ----
	{
		UVerticalBox* Top = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Place(Top, FVector2D(0.5f, 0.f), FVector2D(0.f, 26.f));

		RoundText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 22, NeonUI::Dim);
		Top->AddChildToVerticalBox(RoundText)->SetHorizontalAlignment(HAlign_Center);

		CollapseText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 26, NeonUI::Warning);
		{
			UVerticalBoxSlot* S = Top->AddChildToVerticalBox(CollapseText);
			S->SetHorizontalAlignment(HAlign_Center);
			S->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
		}
		CollapseText->SetVisibility(ESlateVisibility::Collapsed);
	}

	// ---- Top right: alive count ----
	{
		UBorder* PanelBorder = NeonUI::MakePanel(WidgetTree, FMargin(18.f, 10.f));
		Place(PanelBorder, FVector2D(1.f, 0.f), FVector2D(-30.f, 30.f));
		AliveText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 24, NeonUI::Text);
		PanelBorder->AddChild(AliveText);
	}

	// ---- Center stack: big line, small line, results standings, results buttons ----
	UVerticalBox* Center = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Place(Center, FVector2D(0.5f, 0.5f), FVector2D(0.f, -40.f));

	CenterBig = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 84, NeonUI::Accent);
	Center->AddChildToVerticalBox(CenterBig)->SetHorizontalAlignment(HAlign_Center);

	CenterSmall = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 28, NeonUI::Text);
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

	RematchButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Rematch", "REMATCH"), 26);
	NeonUI::SetButtonSelected(RematchButton, true);
	Buttons->AddChildToHorizontalBox(RematchButton)->SetPadding(FMargin(12.f, 0.f));

	MainMenuButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("MainMenu", "MAIN MENU"), 26);
	Buttons->AddChildToHorizontalBox(MainMenuButton)->SetPadding(FMargin(12.f, 0.f));

	Buttons->SetVisibility(ESlateVisibility::Collapsed);

	// ---- Powerup slot: bottom center stack of name, hint, timer bar ----
	{
		UBorder* PanelBorder = NeonUI::MakePanel(WidgetTree, FMargin(22.f, 12.f));
		PowerupPanel = PanelBorder;
		Place(PanelBorder, FVector2D(0.5f, 1.f), FVector2D(0.f, -36.f));

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
		PowerupBar->SetFillColorAndOpacity(NeonUI::Accent);
		{
			UVerticalBoxSlot* S = Stack->AddChildToVerticalBox(PowerupBar);
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
		}
		PowerupBar->SetVisibility(ESlateVisibility::Collapsed);

		PanelBorder->SetVisibility(ESlateVisibility::Collapsed);
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
}

const UNeonPowerupComponent* UNeonHUDWidget::FindLocalPowerups() const
{
	const APlayerController* PC = GetOwningPlayer();
	const ANeonVehiclePawn* Vehicle = PC ? Cast<ANeonVehiclePawn>(PC->GetPawn()) : nullptr;
	return Vehicle ? Vehicle->GetPowerups() : nullptr;
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
		FLinearColor Color = PS->GetColor();
		FLinearColor TextColor = NeonUI::Text;
		if (bOut)
		{
			Color *= 0.3f;
			Color.A = 1.f;
			TextColor = NeonUI::Dim;
		}

		if (StandingSwatches.IsValidIndex(SlotIndex))
		{
			NeonUI::SetSwatchColor(StandingSwatches[SlotIndex], Color);
		}
		if (UTextBlock* Name = StandingNames.IsValidIndex(SlotIndex) ? StandingNames[SlotIndex].Get() : nullptr)
		{
			FString Line = PS->GetPlayerName();
			if (PS == LocalPS)
			{
				Line += TEXT("  (YOU)");
			}
			else if (PS->IsAI())
			{
				Line += TEXT("  AI");
			}
			if (bOut)
			{
				Line += TEXT("  OUT");
			}
			Name->SetText(FText::FromString(Line));
			Name->SetColorAndOpacity(FSlateColor(TextColor));
		}
		if (UTextBlock* Wins = StandingWins.IsValidIndex(SlotIndex) ? StandingWins[SlotIndex].Get() : nullptr)
		{
			Wins->SetText(FText::AsNumber(PS->GetRoundWins()));
			Wins->SetColorAndOpacity(FSlateColor(bOut ? NeonUI::Dim : NeonUI::Accent));
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
			RoundText->SetText(FText::Format(LOCTEXT("Round", "ROUND {0}   \u00B7   FIRST TO {1}"), GS.GetRoundNumber(), GS.GetScoreLimit()));
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
			const float Until = Arena->GetSecondsUntilCollapse();
			const float Progress = Arena->GetCollapseProgress();
			if (Progress >= 1.f)
			{
				Line = LOCTEXT("WallsStopped", "WALLS LOCKED");
				Alpha = 0.7f;
			}
			else if (Progress > 0.f)
			{
				Line = LOCTEXT("WallsClosing", "WALLS CLOSING");
				Alpha = 0.65f + 0.35f * FMath::Sin(Pulse * 6.f);
			}
			else if (Until >= 0.f && Until <= CollapseWarningSeconds)
			{
				Line = FText::Format(LOCTEXT("WallsIn", "WALLS CLOSE IN {0}"), FMath::CeilToInt(Until));
				Alpha = Until <= 3.f ? 0.6f + 0.4f * FMath::Sin(Pulse * 10.f) : 1.f;
			}
		}

		if (Line.IsEmpty())
		{
			CollapseText->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			CollapseText->SetText(Line);
			FLinearColor C = NeonUI::Warning;
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
			AliveText->SetText(FText::Format(LOCTEXT("Alive", "{0} ALIVE"), Alive));
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
	FLinearColor Color = NeonUI::Text;
	float Percent = -1.f; // <0 hides the bar

	if (Active != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Active);
		Color = NeonPowerups::Color(Active);
		// An armed shield with no time limit reports 0 seconds remaining; everything else counts down.
		const bool bTimed = Powerups->GetActiveSecondsRemaining() > 0.f;
		Hint = bTimed ? LOCTEXT("PowerupActive", "ACTIVE") : LOCTEXT("PowerupArmed", "ARMED");
		if (bTimed)
		{
			Percent = Powerups->GetActiveFraction();
		}
	}
	else if (Held != ENeonPowerupType::None)
	{
		Name = NeonPowerups::DisplayName(Held);
		Color = NeonPowerups::Color(Held);
		Hint = LOCTEXT("PowerupPress", "PRESS SPACE");
	}

	if (bEMPd)
	{
		// The jam warning takes the hint line; the bar shows how long it lasts when nothing else is active.
		Hint = LOCTEXT("Jammed", "STEERING JAMMED");
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
		const FString Line = FString::Printf(TEXT("%d.  %s%s     %d"), i + 1, *PS->GetPlayerName(), PS == LocalPS ? TEXT("  (YOU)") : TEXT(""), PS->GetRoundWins());
		Row->SetText(FText::FromString(Line));
		Row->SetColorAndOpacity(FSlateColor(PS->GetColor()));
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	ResultsPanel->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UNeonHUDWidget::UpdateCenter(const ANeonGameState& GS, const ANeonPlayerState* LocalPS)
{
	const ENeonMatchPhase Phase = GS.GetPhase();
	bool bShowButtons = false;
	bool bShowStandings = false;

	// Spectator line shared by Playing and RoundResults.
	auto SpectateLine = [&]() -> FText
	{
		const ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer());
		const ANeonVehiclePawn* Target = PC ? PC->GetSpectateTarget() : nullptr;
		if (Target)
		{
			return FText::Format(LOCTEXT("Spectating", "ELIMINATED   \u00B7   WATCHING {0}   \u00B7   A / D TO SWITCH"), NameForSlot(GS, Target->GetPlayerSlot()));
		}
		return LOCTEXT("Eliminated", "ELIMINATED");
	};

	switch (Phase)
	{
	case ENeonMatchPhase::Countdown:
	{
		const float Remaining = GS.GetPhaseTimeRemaining();
		const int32 Count = FMath::CeilToInt(Remaining);
		SetCenter(Count > 0 ? FText::AsNumber(Count) : LOCTEXT("Go", "GO"), FText::GetEmpty(), NeonUI::Accent, NeonUI::Text);
		break;
	}
	case ENeonMatchPhase::Playing:
	{
		const bool bDead = LocalPS && !LocalPS->IsAlive();
		if (TimeInPhase < GoDisplaySeconds)
		{
			SetCenter(LOCTEXT("Go", "GO"), bDead ? SpectateLine() : FText::GetEmpty(), NeonUI::Accent, NeonUI::Dim);
		}
		else if (bDead)
		{
			SetCenter(SpectateLine(), LOCTEXT("SpectateHint", "A / D switches camera"), NeonUI::Warning, NeonUI::Dim);
		}
		else
		{
			SetCenter(FText::GetEmpty(), FText::GetEmpty(), NeonUI::Accent, NeonUI::Dim);
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
				ColorForSlot(GS, Winner), NeonUI::Text);
		}
		else
		{
			SetCenter(LOCTEXT("NoSurvivors", "NO SURVIVORS"), LOCTEXT("NoPoint", "No point awarded"), NeonUI::Text, NeonUI::Dim);
		}
		break;
	}
	case ENeonMatchPhase::MatchResults:
	{
		const int32 Winner = GS.GetMatchWinnerSlot();
		const bool bYou = LocalPS && LocalPS->GetSlot() == Winner;
		SetCenter(
			bYou ? LOCTEXT("YouWinMatch", "YOU WIN THE MATCH") : FText::Format(LOCTEXT("MatchWin", "{0} WINS THE MATCH"), NameForSlot(GS, Winner)),
			LOCTEXT("FinalStandings", "FINAL STANDINGS"),
			ColorForSlot(GS, Winner), NeonUI::Dim);
		bShowButtons = true;
		bShowStandings = true;
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
