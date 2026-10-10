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
#include "Components/EditableTextBox.h"
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

	// Floating rounded panel on the left third of the screen, leaving the arena visible behind.
	UBorder* Panel = NeonUI::MakePanel(WidgetTree, FMargin(56.f, 44.f), 16.f);
	Panel->SetHorizontalAlignment(HAlign_Fill);
	Panel->SetVerticalAlignment(VAlign_Center);
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Panel);
		S->SetAnchors(FAnchors(0.f, 0.f, 0.f, 1.f));
		S->SetAlignment(FVector2D(0.f, 0.f));
		S->SetOffsets(FMargin(40.f, 40.f, 600.f, 40.f)); // left, top, width, bottom
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->AddChild(Column);

	AddRow(Column, NeonUI::MakeText(WidgetTree, LOCTEXT("Title", "NEON GRID"), 72, NeonUI::Accent), 0.f);
	AddRow(Column, NeonUI::MakeText(WidgetTree, LOCTEXT("Subtitle", "LIGHT CYCLE ARENA"), 16, NeonUI::Dim), 2.f);

	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass());
	AddRow(Column, Pages, 36.f);

	// Version footer pinned to the bottom-left corner of the screen.
	{
		UTextBlock* Version = NeonUI::MakeText(WidgetTree, LOCTEXT("Version", "NEON GRID  pre-alpha  \u00B7  Milestone 6"), 13, NeonUI::Dim, false);
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Version);
		S->SetAnchors(FAnchors(0.f, 1.f));
		S->SetAlignment(FVector2D(0.f, 1.f));
		S->SetPosition(FVector2D(44.f, -14.f));
		S->SetAutoSize(true);
	}

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

	LanButton = MainButton(LOCTEXT("Lan", "LAN PLAY"));

	UTextBlock* OptionsLabel = nullptr;
	OptionsButton = MainButton(LOCTEXT("Options", "OPTIONS"), &OptionsLabel);
	NeonUI::SetButtonEnabled(OptionsButton, OptionsLabel, false);

	UTextBlock* CreditsLabel = nullptr;
	CreditsButton = MainButton(LOCTEXT("Credits", "CREDITS"), &CreditsLabel);
	NeonUI::SetButtonEnabled(CreditsButton, CreditsLabel, false);

	QuitButton = MainButton(LOCTEXT("Quit", "QUIT"));

	AddRow(Main, NeonUI::MakeText(WidgetTree, LOCTEXT("MainHint", "Options and credits arrive later. LAN is host + join by IP."), 14, NeonUI::Dim, false), 24.f);

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

	UHorizontalBox* CollapseRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Collapse", "CLOSING WALLS"));
	CollapseOnButton = AddOption(WidgetTree, CollapseRow, LOCTEXT("On", "ON"));
	CollapseOffButton = AddOption(WidgetTree, CollapseRow, LOCTEXT("Off", "OFF"));

	AddRow(Solo, NeonUI::MakeText(WidgetTree, LOCTEXT("Arena", "ARENA"), 20, NeonUI::Dim), 22.f);
	ArenaText = NeonUI::MakeText(WidgetTree, LOCTEXT("TheGrid", "THE GRID"), 18, NeonUI::Text);
	ArenaText->SetAutoWrapText(true);
	AddRow(Solo, ArenaText, 6.f);

	StartButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Start", "START"), 30);
	NeonUI::SetButtonSelected(StartButton, true);
	AddRow(Solo, StartButton, 36.f)->SetHorizontalAlignment(HAlign_Fill);

	BackButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Back", "BACK"), 22);
	AddRow(Solo, BackButton, 12.f)->SetHorizontalAlignment(HAlign_Fill);

	UTextBlock* Controls = NeonUI::MakeText(WidgetTree, LOCTEXT("Controls", "A / D or Left / Right to steer.  Space uses a powerup.\nAfter you are out, the camera follows a living bike.  A / D switches.  Esc returns here."), 14, NeonUI::Dim, false);
	Controls->SetAutoWrapText(true);
	AddRow(Solo, Controls, 24.f);

	// ---- Page 2: LAN ----
	UVerticalBox* Lan = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Lan);

	AddRow(Lan, NeonUI::MakeText(WidgetTree, LOCTEXT("LanTitle", "LAN PLAY"), 34, NeonUI::Text), 0.f);
	UTextBlock* LanHint = NeonUI::MakeText(WidgetTree, LOCTEXT("LanHint", "Host opens this match for the local network. Friends join with your IPv4 address. Windows Firewall must allow NeonGrid / UnrealEditor on private networks. Same Unreal 5.8 build on every PC."), 14, NeonUI::Dim, false);
	LanHint->SetAutoWrapText(true);
	AddRow(Lan, LanHint, 12.f);

	LanHostButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Host", "HOST"), 28);
	AddRow(Lan, LanHostButton, 28.f)->SetHorizontalAlignment(HAlign_Fill);

	AddRow(Lan, NeonUI::MakeText(WidgetTree, LOCTEXT("JoinAddress", "HOST ADDRESS"), 20, NeonUI::Dim), 22.f);
	LanAddressBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	LanAddressBox->SetHintText(LOCTEXT("IpHint", "192.168.1.10"));
	LanAddressBox->SetJustification(ETextJustify::Center);
	AddRow(Lan, LanAddressBox, 6.f)->SetHorizontalAlignment(HAlign_Fill);

	LanJoinButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Join", "JOIN"), 26);
	AddRow(Lan, LanJoinButton, 14.f)->SetHorizontalAlignment(HAlign_Fill);

	LanStatusText = NeonUI::MakeText(WidgetTree, FText::GetEmpty(), 14, NeonUI::Warning, false);
	LanStatusText->SetAutoWrapText(true);
	AddRow(Lan, LanStatusText, 10.f);

	LanBackButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Back", "BACK"), 22);
	AddRow(Lan, LanBackButton, 18.f)->SetHorizontalAlignment(HAlign_Fill);
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
	if (CollapseOnButton)  { CollapseOnButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnCollapseOnClicked); }
	if (CollapseOffButton) { CollapseOffButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnCollapseOffClicked); }
	if (LanButton)      { LanButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanClicked); }
	if (LanHostButton)  { LanHostButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanHostClicked); }
	if (LanJoinButton)  { LanJoinButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanJoinClicked); }
	if (LanBackButton)  { LanBackButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanBackClicked); }
}

// ---------------------------------------------------------------------------

void UNeonMainMenuWidget::LoadSettings()
{
	if (const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		Settings = GI->MatchSettings;
	}
	Settings.AIPlayerCount = FMath::Clamp(Settings.AIPlayerCount, 1, 3);
	if (LanAddressBox)
	{
		if (const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
		{
			LanAddressBox->SetText(FText::FromString(GI->LastJoinAddress));
		}
	}
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

	NeonUI::SetButtonSelected(CollapseOnButton, Settings.bArenaCollapse);
	NeonUI::SetButtonSelected(CollapseOffButton, !Settings.bArenaCollapse);
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

void UNeonMainMenuWidget::SetCollapseEnabled(bool bEnabled)
{
	Settings.bArenaCollapse = bEnabled;
	SaveSettings();
	RefreshSelection();
}

void UNeonMainMenuWidget::OnLanClicked()
{
	if (Pages)
	{
		Pages->SetActiveWidgetIndex(2);
	}
}

void UNeonMainMenuWidget::OnLanBackClicked()
{
	ShowMainPage();
}

void UNeonMainMenuWidget::OnLanHostClicked()
{
	if (LanStatusText)
	{
		LanStatusText->SetText(LOCTEXT("Hosting", "Opening listen server..."));
	}
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestHostLan();
	}
}

void UNeonMainMenuWidget::OnLanJoinClicked()
{
	FString Address = LanAddressBox ? LanAddressBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Address.IsEmpty())
	{
		if (LanStatusText)
		{
			LanStatusText->SetText(LOCTEXT("NeedIp", "Type the host IPv4 address."));
		}
		return;
	}
	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->LastJoinAddress = Address;
	}
	if (LanStatusText)
	{
		LanStatusText->SetText(FText::Format(LOCTEXT("Joining", "Joining {0}..."), FText::FromString(Address)));
	}
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestJoinLan(Address);
	}
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
void UNeonMainMenuWidget::OnCollapseOnClicked() { SetCollapseEnabled(true); }
void UNeonMainMenuWidget::OnCollapseOffClicked() { SetCollapseEnabled(false); }

#undef LOCTEXT_NAMESPACE
