#include "UI/NeonMainMenuWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Core/NeonGameInstance.h"
#include "Core/NeonPlayerController.h"
#include "UI/NeonUIStyle.h"

#define LOCTEXT_NAMESPACE "NeonGrid"

UNeonMainMenuWidget::UNeonMainMenuWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true); // UI-only input mode focuses this widget
}

TSharedRef<SWidget> UNeonMainMenuWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	TSharedRef<SWidget> Result = Super::RebuildWidget();
	BindButtons();
	return Result;
}

void UNeonMainMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();
	LoadSettings();
	RefreshSelection();
	ShowMainPage();
}

namespace
{
	UVerticalBoxSlot* AddRow(UVerticalBox* Box, UWidget* Child, float TopPad = 10.f)
	{
		UVerticalBoxSlot* S = Box->AddChildToVerticalBox(Child);
		S->SetHorizontalAlignment(HAlign_Center);
		S->SetPadding(FMargin(0.f, TopPad, 0.f, 0.f));
		return S;
	}

	UHorizontalBox* MakeOptionRow(UWidgetTree* Tree, UVerticalBox* Parent, const FText& Label)
	{
		AddRow(Parent, NeonUI::MakeText(Tree, Label, 20, NeonUI::Dim), 22.f);
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		AddRow(Parent, Row, 6.f);
		return Row;
	}

	UButton* AddOption(UWidgetTree* Tree, UHorizontalBox* Row, const FText& Label)
	{
		UButton* B = NeonUI::MakeButton(Tree, Label, 22);
		Row->AddChildToHorizontalBox(B)->SetPadding(FMargin(6.f, 0.f));
		return B;
	}
}

void UNeonMainMenuWidget::BuildTree()
{
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Canvas;

	// Panel on the left third of the screen, leaving the arena visible behind.
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Panel->SetBrushColor(NeonUI::Panel);
	Panel->SetPadding(FMargin(60.f, 50.f));
	Panel->SetHorizontalAlignment(HAlign_Fill);
	Panel->SetVerticalAlignment(VAlign_Center);
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Panel);
		S->SetAnchors(FAnchors(0.f, 0.f, 0.f, 1.f));
		S->SetAlignment(FVector2D(0.f, 0.f));
		S->SetOffsets(FMargin(0.f, 0.f, 620.f, 0.f)); // left, top, width, bottom
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->AddChild(Column);

	AddRow(Column, NeonUI::MakeText(WidgetTree, LOCTEXT("Title", "NEON GRID"), 72, NeonUI::Accent), 0.f);

	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass());
	AddRow(Column, Pages, 40.f);

	// ---- Page 0: main ----
	UVerticalBox* Main = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Main);

	auto MainButton = [&](const FText& Label, UTextBlock** OutLabel = nullptr)
	{
		UButton* B = NeonUI::MakeButton(WidgetTree, Label, 28, OutLabel);
		AddRow(Main, B, 14.f)->SetHorizontalAlignment(HAlign_Fill);
		return B;
	};

	SoloButton = MainButton(LOCTEXT("Solo", "SOLO"));

	UTextBlock* LanLabel = nullptr;
	LanButton = MainButton(LOCTEXT("Lan", "LAN PLAY"), &LanLabel);
	NeonUI::SetButtonEnabled(LanButton, LanLabel, false);

	UTextBlock* OptionsLabel = nullptr;
	OptionsButton = MainButton(LOCTEXT("Options", "OPTIONS"), &OptionsLabel);
	NeonUI::SetButtonEnabled(OptionsButton, OptionsLabel, false);

	UTextBlock* CreditsLabel = nullptr;
	CreditsButton = MainButton(LOCTEXT("Credits", "CREDITS"), &CreditsLabel);
	NeonUI::SetButtonEnabled(CreditsButton, CreditsLabel, false);

	QuitButton = MainButton(LOCTEXT("Quit", "QUIT"));

	AddRow(Main, NeonUI::MakeText(WidgetTree, LOCTEXT("MainHint", "LAN, options and credits arrive in later milestones."), 14, NeonUI::Dim, false), 24.f);

	// ---- Page 1: solo ----
	UVerticalBox* Solo = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Solo);

	AddRow(Solo, NeonUI::MakeText(WidgetTree, LOCTEXT("SoloTitle", "SOLO"), 34, NeonUI::Text), 0.f);

	UHorizontalBox* AIRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("AIPlayers", "AI PLAYERS"));
	AI1Button = AddOption(WidgetTree, AIRow, FText::AsNumber(1));
	AI2Button = AddOption(WidgetTree, AIRow, FText::AsNumber(2));
	AI3Button = AddOption(WidgetTree, AIRow, FText::AsNumber(3));

	UHorizontalBox* DiffRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Difficulty", "DIFFICULTY"));
	EasyButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Easy", "EASY"));
	NormalButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Normal", "NORMAL"));
	HardButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Hard", "HARD"));
	ExpertButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Expert", "EXPERT"));

	UHorizontalBox* PowerupRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Powerups", "POWERUPS"));
	PowerupsOnButton = AddOption(WidgetTree, PowerupRow, LOCTEXT("On", "ON"));
	PowerupsOffButton = AddOption(WidgetTree, PowerupRow, LOCTEXT("Off", "OFF"));

	AddRow(Solo, NeonUI::MakeText(WidgetTree, LOCTEXT("Arena", "ARENA"), 20, NeonUI::Dim), 22.f);
	ArenaText = NeonUI::MakeText(WidgetTree, LOCTEXT("TheGrid", "THE GRID"), 24, NeonUI::Text);
	AddRow(Solo, ArenaText, 6.f);

	StartButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Start", "START"), 30);
	NeonUI::SetButtonSelected(StartButton, true);
	AddRow(Solo, StartButton, 36.f)->SetHorizontalAlignment(HAlign_Fill);

	BackButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Back", "BACK"), 22);
	AddRow(Solo, BackButton, 12.f)->SetHorizontalAlignment(HAlign_Fill);

	AddRow(Solo, NeonUI::MakeText(WidgetTree, LOCTEXT("Controls", "A / D or Left / Right to steer.  Space uses a powerup.  Esc returns here."), 14, NeonUI::Dim, false), 24.f);
}

void UNeonMainMenuWidget::BindButtons()
{
	if (bBoundButtons)
	{
		return;
	}
	bBoundButtons = true;

	// AddDynamic needs the literal function name (it stringifies it), so no helper lambda here.
	if (SoloButton)   { SoloButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnSoloClicked); }
	if (QuitButton)   { QuitButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnQuitClicked); }
	if (BackButton)   { BackButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnBackClicked); }
	if (StartButton)  { StartButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnStartClicked); }
	if (AI1Button)    { AI1Button->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnAI1Clicked); }
	if (AI2Button)    { AI2Button->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnAI2Clicked); }
	if (AI3Button)    { AI3Button->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnAI3Clicked); }
	if (EasyButton)   { EasyButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnEasyClicked); }
	if (NormalButton) { NormalButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnNormalClicked); }
	if (HardButton)   { HardButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnHardClicked); }
	if (ExpertButton) { ExpertButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnExpertClicked); }
	if (PowerupsOnButton)  { PowerupsOnButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnPowerupsOnClicked); }
	if (PowerupsOffButton) { PowerupsOffButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnPowerupsOffClicked); }
}

// ---------------------------------------------------------------------------

void UNeonMainMenuWidget::LoadSettings()
{
	if (const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		Settings = GI->MatchSettings;
	}
	Settings.AIPlayerCount = FMath::Clamp(Settings.AIPlayerCount, 1, 3);
}

void UNeonMainMenuWidget::SaveSettings() const
{
	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->MatchSettings = Settings;
	}
}

void UNeonMainMenuWidget::RefreshSelection()
{
	NeonUI::SetButtonSelected(AI1Button, Settings.AIPlayerCount == 1);
	NeonUI::SetButtonSelected(AI2Button, Settings.AIPlayerCount == 2);
	NeonUI::SetButtonSelected(AI3Button, Settings.AIPlayerCount == 3);

	NeonUI::SetButtonSelected(EasyButton, Settings.AIDifficulty == ENeonAIDifficulty::Easy);
	NeonUI::SetButtonSelected(NormalButton, Settings.AIDifficulty == ENeonAIDifficulty::Normal);
	NeonUI::SetButtonSelected(HardButton, Settings.AIDifficulty == ENeonAIDifficulty::Hard);
	NeonUI::SetButtonSelected(ExpertButton, Settings.AIDifficulty == ENeonAIDifficulty::Expert);

	NeonUI::SetButtonSelected(PowerupsOnButton, Settings.bPowerupsEnabled);
	NeonUI::SetButtonSelected(PowerupsOffButton, !Settings.bPowerupsEnabled);
}

void UNeonMainMenuWidget::ShowMainPage()
{
	if (Pages)
	{
		Pages->SetActiveWidgetIndex(0);
	}
}

void UNeonMainMenuWidget::SetAICount(int32 Count)
{
	Settings.AIPlayerCount = FMath::Clamp(Count, 1, 3);
	SaveSettings();
	RefreshSelection();
}

void UNeonMainMenuWidget::SetDifficulty(ENeonAIDifficulty Level)
{
	Settings.AIDifficulty = Level;
	SaveSettings();
	RefreshSelection();
}

void UNeonMainMenuWidget::SetPowerupsEnabled(bool bEnabled)
{
	Settings.bPowerupsEnabled = bEnabled;
	SaveSettings();
	RefreshSelection();
}

void UNeonMainMenuWidget::OnSoloClicked()
{
	if (Pages)
	{
		Pages->SetActiveWidgetIndex(1);
	}
}

void UNeonMainMenuWidget::OnBackClicked()
{
	ShowMainPage();
}

void UNeonMainMenuWidget::OnStartClicked()
{
	SaveSettings();
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestStartSoloMatch(Settings);
	}
}

void UNeonMainMenuWidget::OnQuitClicked()
{
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestQuit();
	}
}

void UNeonMainMenuWidget::OnAI1Clicked() { SetAICount(1); }
void UNeonMainMenuWidget::OnAI2Clicked() { SetAICount(2); }
void UNeonMainMenuWidget::OnAI3Clicked() { SetAICount(3); }
void UNeonMainMenuWidget::OnEasyClicked() { SetDifficulty(ENeonAIDifficulty::Easy); }
void UNeonMainMenuWidget::OnNormalClicked() { SetDifficulty(ENeonAIDifficulty::Normal); }
void UNeonMainMenuWidget::OnHardClicked() { SetDifficulty(ENeonAIDifficulty::Hard); }
void UNeonMainMenuWidget::OnExpertClicked() { SetDifficulty(ENeonAIDifficulty::Expert); }
void UNeonMainMenuWidget::OnPowerupsOnClicked() { SetPowerupsEnabled(true); }
void UNeonMainMenuWidget::OnPowerupsOffClicked() { SetPowerupsEnabled(false); }

#undef LOCTEXT_NAMESPACE
