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
#include "Core/NeonPlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "SocketSubsystem.h"
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
	const UWorld* World = GetWorld();
	const ENetMode Mode = World ? World->GetNetMode() : NM_Standalone;
	if (Mode == NM_ListenServer || Mode == NM_Client)
	{
		ShowLanPage();
	}
	else
	{
		ShowMainPage();
	}
}

void UNeonMainMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (Pages && Pages->GetActiveWidgetIndex() == 2)
	{
		const UWorld* World = GetWorld();
		const ENetMode Mode = World ? World->GetNetMode() : NM_Standalone;
		if (Mode == NM_Standalone)
		{
			LanScanWait -= InDeltaTime;
			if (LanScanWait <= 0.f)
			{
				LanScanWait = 4.f;
				if (const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
				{
					if (!GI->IsSearchingLan())
					{
						if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
						{
							PC->RequestFindLan();
						}
					}
				}
			}
		}
		RefreshLanPage();
	}
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

	// Section label in condensed caps with a short accent rule beside it.
	UWidget* MakeSectionLabel(UWidgetTree* Tree, const FText& Label)
	{
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* Caps = NeonUI::MakeCaps(Tree, Label, 15, NeonUI::Accent, 4);
		Row->AddChildToHorizontalBox(Caps)->SetVerticalAlignment(VAlign_Center);
		UHorizontalBoxSlot* RuleSlot = Row->AddChildToHorizontalBox(NeonUI::MakeRule(Tree, FLinearColor(NeonUI::Accent.R, NeonUI::Accent.G, NeonUI::Accent.B, 0.35f), 1.f));
		RuleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		RuleSlot->SetVerticalAlignment(VAlign_Center);
		RuleSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
		return Row;
	}

	UHorizontalBox* MakeOptionRow(UWidgetTree* Tree, UVerticalBox* Parent, const FText& Label)
	{
		AddRow(Parent, MakeSectionLabel(Tree, Label), 22.f)->SetHorizontalAlignment(HAlign_Fill);
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		AddRow(Parent, Row, 8.f)->SetHorizontalAlignment(HAlign_Fill);
		return Row;
	}

	UButton* AddOption(UWidgetTree* Tree, UHorizontalBox* Row, const FText& Label)
	{
		UButton* B = NeonUI::MakeButton(Tree, Label, 18);
		UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(B);
		S->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetHorizontalAlignment(HAlign_Fill);
		return B;
	}

	// Page heading: heavy display face over a full-width accent rule.
	void AddPageTitle(UWidgetTree* Tree, UVerticalBox* Page, const FText& Title)
	{
		UTextBlock* Heading = NeonUI::MakeDisplay(Tree, Title, 32, NeonUI::Text, 2);
		Heading->SetJustification(ETextJustify::Left);
		AddRow(Page, Heading, 0.f)->SetHorizontalAlignment(HAlign_Left);
		AddRow(Page, NeonUI::MakeRule(Tree, NeonUI::Accent, 2.f), 8.f)->SetHorizontalAlignment(HAlign_Fill);
	}

	UTextBlock* MakeHint(UWidgetTree* Tree, const FText& Hint)
	{
		UTextBlock* T = NeonUI::MakeCaps(Tree, Hint, 12, NeonUI::Dim, 1);
		T->SetAutoWrapText(true);
		T->SetJustification(ETextJustify::Left);
		return T;
	}
}

void UNeonMainMenuWidget::BuildTree()
{
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Canvas;

	// Console panel on the left third of the screen, leaving the arena visible behind.
	UBorder* Panel = NeonUI::MakePanel(WidgetTree, FMargin(48.f, 40.f), 0.f);
	Panel->SetHorizontalAlignment(HAlign_Fill);
	Panel->SetVerticalAlignment(VAlign_Center);
	{
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Panel);
		S->SetAnchors(FAnchors(0.f, 0.f, 0.f, 1.f));
		S->SetAlignment(FVector2D(0.f, 0.f));
		S->SetOffsets(FMargin(40.f, 40.f, 620.f, 40.f)); // left, top, width, bottom
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->AddChild(Column);

	// Header: system line, title, subtitle, rule.
	AddRow(Column, NeonUI::MakeCaps(WidgetTree, LOCTEXT("SysLine", "SYSTEM   //   ONLINE"), 12, NeonUI::Line, 6), 0.f)->SetHorizontalAlignment(HAlign_Left);
	{
		UTextBlock* Title = NeonUI::MakeDisplay(WidgetTree, LOCTEXT("Title", "BLUESHIFT"), 56, NeonUI::Highlight, 6);
		Title->SetJustification(ETextJustify::Left);
		AddRow(Column, Title, 8.f)->SetHorizontalAlignment(HAlign_Left);
	}
	AddRow(Column, NeonUI::MakeCaps(WidgetTree, LOCTEXT("Subtitle", "NEON GRID"), 14, NeonUI::Accent, 8), 2.f)->SetHorizontalAlignment(HAlign_Left);
	AddRow(Column, NeonUI::MakeRule(WidgetTree, NeonUI::Line, 1.f), 16.f)->SetHorizontalAlignment(HAlign_Fill);

	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass());
	AddRow(Column, Pages, 28.f)->SetHorizontalAlignment(HAlign_Fill);

	// Version footer pinned to the bottom-left corner of the screen.
	{
		UTextBlock* Version = NeonUI::MakeCaps(WidgetTree, LOCTEXT("Version", "SYS.NG   //   GRID LINK   //   UE 5.8"), 12, NeonUI::Dim, 3);
		UCanvasPanelSlot* S = Canvas->AddChildToCanvas(Version);
		S->SetAnchors(FAnchors(0.f, 1.f));
		S->SetAlignment(FVector2D(0.f, 1.f));
		S->SetPosition(FVector2D(44.f, -14.f));
		S->SetAutoSize(true);
	}

	// ---- Page 0: main ----
	UVerticalBox* Main = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Main);

	AddRow(Main, MakeSectionLabel(WidgetTree, LOCTEXT("MainSection", "SELECT")), 0.f)->SetHorizontalAlignment(HAlign_Fill);

	auto MainButton = [&](const FText& Label, UTextBlock** OutLabel = nullptr)
	{
		UButton* B = NeonUI::MakeButton(WidgetTree, Label, 24, OutLabel);
		AddRow(Main, B, 12.f)->SetHorizontalAlignment(HAlign_Fill);
		return B;
	};

	SoloButton = MainButton(LOCTEXT("Solo", "PLAY"));

	LanButton = MainButton(LOCTEXT("Lan", "MULTIPLAYER"));

	UTextBlock* OptionsLabel = nullptr;
	OptionsButton = MainButton(LOCTEXT("Options", "CONFIG"), &OptionsLabel);
	NeonUI::SetButtonEnabled(OptionsButton, OptionsLabel, false);

	UTextBlock* CreditsLabel = nullptr;
	CreditsButton = MainButton(LOCTEXT("Credits", "ARCHIVE"), &CreditsLabel);
	NeonUI::SetButtonEnabled(CreditsButton, CreditsLabel, false);

	QuitButton = MainButton(LOCTEXT("Quit", "EXIT"));

	AddRow(Main, MakeHint(WidgetTree, LOCTEXT("MainHint", "CONFIG AND ARCHIVE ARE OFFLINE.  MULTIPLAYER SCANS THE LAN.")), 22.f)->SetHorizontalAlignment(HAlign_Fill);

	// ---- Page 1: solo ----
	UVerticalBox* Solo = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Solo);

	AddPageTitle(WidgetTree, Solo, LOCTEXT("SoloTitle", "PILOT SETUP"));

	UHorizontalBox* AIRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("AIPlayers", "RIVALS"));
	AI1Button = AddOption(WidgetTree, AIRow, FText::AsNumber(1));
	AI2Button = AddOption(WidgetTree, AIRow, FText::AsNumber(2));
	AI3Button = AddOption(WidgetTree, AIRow, FText::AsNumber(3));

	UHorizontalBox* DiffRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Difficulty", "THREAT"));
	EasyButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Easy", "EASY"));
	NormalButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Normal", "NORMAL"));
	HardButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Hard", "HARD"));
	ExpertButton = AddOption(WidgetTree, DiffRow, LOCTEXT("Expert", "EXPERT"));

	UHorizontalBox* PowerupRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Powerups", "PROGRAMS"));
	PowerupsOnButton = AddOption(WidgetTree, PowerupRow, LOCTEXT("On", "ON"));
	PowerupsOffButton = AddOption(WidgetTree, PowerupRow, LOCTEXT("Off", "OFF"));

	UHorizontalBox* CollapseRow = MakeOptionRow(WidgetTree, Solo, LOCTEXT("Collapse", "PERIMETER"));
	CollapseOnButton = AddOption(WidgetTree, CollapseRow, LOCTEXT("On", "ON"));
	CollapseOffButton = AddOption(WidgetTree, CollapseRow, LOCTEXT("Off", "OFF"));

	AddRow(Solo, MakeSectionLabel(WidgetTree, LOCTEXT("Arena", "SECTOR")), 22.f)->SetHorizontalAlignment(HAlign_Fill);
	ArenaText = NeonUI::MakeCaps(WidgetTree, LOCTEXT("TheGrid", "THE GRID"), 20, NeonUI::Text, 4);
	ArenaText->SetAutoWrapText(true);
	ArenaText->SetJustification(ETextJustify::Left);
	AddRow(Solo, ArenaText, 8.f)->SetHorizontalAlignment(HAlign_Left);

	StartButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Start", "ENGAGE"), 26);
	NeonUI::SetButtonSelected(StartButton, true);
	AddRow(Solo, StartButton, 32.f)->SetHorizontalAlignment(HAlign_Fill);

	BackButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Back", "RETURN"), 18);
	AddRow(Solo, BackButton, 10.f)->SetHorizontalAlignment(HAlign_Fill);

	AddRow(Solo, MakeHint(WidgetTree, LOCTEXT("Controls", "A D  STEER     SHIFT  BOOST     CTRL  JUMP     SPACE  PROGRAM     ESC  ABORT\nDEREZ: CAMERA LOCKS A LIVE CYCLE.  A D CYCLES TARGETS.")), 22.f)->SetHorizontalAlignment(HAlign_Fill);

	// ---- Page 2: LAN ----
	UVerticalBox* Lan = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Pages->AddChild(Lan);

	AddPageTitle(WidgetTree, Lan, LOCTEXT("LanTitle", "LOCAL LINK"));
	AddRow(Lan, MakeHint(WidgetTree, LOCTEXT("LanHint", "OPEN LINK SHOWS YOUR ADDRESS.  OTHER PCS SCAN ON THEIR OWN AND JOIN IN ONE CLICK.  SAME BUILD.  ALLOW THE GAME ON PRIVATE NETWORKS.")), 12.f)->SetHorizontalAlignment(HAlign_Fill);

	AddRow(Lan, MakeSectionLabel(WidgetTree, LOCTEXT("HostSection", "HOST")), 22.f)->SetHorizontalAlignment(HAlign_Fill);
	LanHostButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Host", "OPEN LINK"), 22);
	AddRow(Lan, LanHostButton, 8.f)->SetHorizontalAlignment(HAlign_Fill);

	LanHostAddressText = NeonUI::MakeCaps(WidgetTree, FText::GetEmpty(), 16, NeonUI::Accent, 2);
	LanHostAddressText->SetAutoWrapText(true);
	LanHostAddressText->SetVisibility(ESlateVisibility::Collapsed);
	AddRow(Lan, LanHostAddressText, 8.f)->SetHorizontalAlignment(HAlign_Fill);

	LanPilotBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	LanPilotBox->AddChildToVerticalBox(MakeSectionLabel(WidgetTree, LOCTEXT("Pilots", "PILOTS")))->SetPadding(FMargin(0.f, 18.f, 0.f, 6.f));
	for (int32 i = 0; i < 4; ++i)
	{
		UTextBlock* Pilot = NeonUI::MakeCaps(WidgetTree, LOCTEXT("OpenSlot", "OPEN"), 16, NeonUI::Text, 2);
		Pilot->SetJustification(ETextJustify::Left);
		LanPilotBox->AddChildToVerticalBox(Pilot)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
		LanPilotLabels.Add(Pilot);
	}
	LanPilotBox->SetVisibility(ESlateVisibility::Collapsed);
	AddRow(Lan, LanPilotBox, 4.f)->SetHorizontalAlignment(HAlign_Fill);

	LanBrowseLabel = MakeSectionLabel(WidgetTree, LOCTEXT("ScanSection", "NETWORK GAMES"));
	AddRow(Lan, LanBrowseLabel, 20.f)->SetHorizontalAlignment(HAlign_Fill);
	LanScanButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Scan", "SCAN NETWORK"), 20);
	AddRow(Lan, LanScanButton, 8.f)->SetHorizontalAlignment(HAlign_Fill);

	for (int32 i = 0; i < 4; ++i)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* Label = NeonUI::MakeCaps(WidgetTree, FText::GetEmpty(), 14, NeonUI::Text, 1);
		Label->SetJustification(ETextJustify::Left);
		{
			UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Label);
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}
		UButton* Join = NeonUI::MakeButton(WidgetTree, LOCTEXT("JoinGame", "JOIN"), 14);
		Row->AddChildToHorizontalBox(Join)->SetVerticalAlignment(VAlign_Center);
		Row->SetVisibility(ESlateVisibility::Collapsed);
		AddRow(Lan, Row, 6.f)->SetHorizontalAlignment(HAlign_Fill);
		LanGameRows.Add(Row);
		LanGameLabels.Add(Label);
		LanGameJoinButtons.Add(Join);
	}

	LanAddressLabel = MakeSectionLabel(WidgetTree, LOCTEXT("JoinAddress", "NODE ADDRESS"));
	AddRow(Lan, LanAddressLabel, 20.f)->SetHorizontalAlignment(HAlign_Fill);
	LanAddressBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	LanAddressBox->SetHintText(LOCTEXT("IpHint", "192.168.1.10"));
	LanAddressBox->SetJustification(ETextJustify::Center);
	NeonUI::StyleAddressBox(LanAddressBox);
	AddRow(Lan, LanAddressBox, 8.f)->SetHorizontalAlignment(HAlign_Fill);

	LanJoinButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("Join", "CONNECT BY ADDRESS"), 18);
	AddRow(Lan, LanJoinButton, 8.f)->SetHorizontalAlignment(HAlign_Fill);

	LanStatusText = NeonUI::MakeCaps(WidgetTree, FText::GetEmpty(), 14, NeonUI::Accent, 2);
	LanStatusText->SetAutoWrapText(true);
	AddRow(Lan, LanStatusText, 10.f)->SetHorizontalAlignment(HAlign_Fill);

	LanEngageButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("LanEngage", "ENGAGE"), 22);
	NeonUI::SetButtonSelected(LanEngageButton, true);
	AddRow(Lan, LanEngageButton, 16.f)->SetHorizontalAlignment(HAlign_Fill);

	LanBackButton = NeonUI::MakeButton(WidgetTree, LOCTEXT("BackLan", "RETURN"), 18);
	AddRow(Lan, LanBackButton, 8.f)->SetHorizontalAlignment(HAlign_Fill);
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
	if (LanScanButton)  { LanScanButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanScanClicked); }
	if (LanJoinButton)  { LanJoinButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanJoinClicked); }
	if (LanEngageButton){ LanEngageButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanEngageClicked); }
	if (LanBackButton)  { LanBackButton->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnLanBackClicked); }
	if (LanGameJoinButtons.IsValidIndex(0) && LanGameJoinButtons[0]) { LanGameJoinButtons[0]->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnJoinFound0); }
	if (LanGameJoinButtons.IsValidIndex(1) && LanGameJoinButtons[1]) { LanGameJoinButtons[1]->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnJoinFound1); }
	if (LanGameJoinButtons.IsValidIndex(2) && LanGameJoinButtons[2]) { LanGameJoinButtons[2]->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnJoinFound2); }
	if (LanGameJoinButtons.IsValidIndex(3) && LanGameJoinButtons[3]) { LanGameJoinButtons[3]->OnClicked.AddDynamic(this, &UNeonMainMenuWidget::OnJoinFound3); }
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

void UNeonMainMenuWidget::ShowLanPage()
{
	if (Pages)
	{
		Pages->SetActiveWidgetIndex(2);
	}
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		const UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_Standalone)
		{
			PC->RequestFindLan();
		}
	}
	RefreshLanPage();
}

void UNeonMainMenuWidget::RefreshLanPage()
{
	const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>();
	const UWorld* World = GetWorld();
	const ENetMode Mode = World ? World->GetNetMode() : NM_Standalone;
	const bool bHost = Mode == NM_ListenServer || (GI && GI->IsHostingLan());
	const bool bClient = Mode == NM_Client;

	if (LanStatusText && GI)
	{
		FText Status = GI->GetLanStatus();
		if (Status.IsEmpty())
		{
			if (bHost)
			{
				Status = FText::Format(LOCTEXT("HostIdle", "LINK OPEN.  {0} PILOT{1} IN THE GRID."),
					World ? World->GetNumPlayerControllers() : 1,
					(World && World->GetNumPlayerControllers() == 1) ? FText::GetEmpty() : FText::FromString(TEXT("S")));
			}
			else if (bClient)
			{
				Status = LOCTEXT("ClientWait", "LINKED.  WAITING FOR HOST TO ENGAGE.");
			}
		}
		LanStatusText->SetText(Status);
		LanStatusText->SetColorAndOpacity(FSlateColor(GI->IsSearchingLan() ? NeonUI::Accent : NeonUI::Text));
	}

	if (LanHostButton)
	{
		LanHostButton->SetVisibility(bClient ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (LanEngageButton)
	{
		LanEngageButton->SetVisibility(bHost ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (LanScanButton)
	{
		LanScanButton->SetVisibility((bHost || bClient) ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	const ESlateVisibility BrowseVis = (bHost || bClient) ? ESlateVisibility::Collapsed : ESlateVisibility::Visible;
	if (LanBrowseLabel) { LanBrowseLabel->SetVisibility(BrowseVis); }
	if (LanAddressLabel) { LanAddressLabel->SetVisibility(BrowseVis); }
	if (LanAddressBox) { LanAddressBox->SetVisibility(BrowseVis); }
	if (LanJoinButton) { LanJoinButton->SetVisibility(BrowseVis); }

	if (LanHostAddressText)
	{
		LanHostAddressText->SetVisibility(bHost ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bHost)
		{
			FString Ip;
			if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				TArray<TSharedPtr<FInternetAddr>> Addrs;
				Sockets->GetLocalAdapterAddresses(Addrs);
				for (const TSharedPtr<FInternetAddr>& Addr : Addrs)
				{
					if (!Addr.IsValid() || !Addr->IsValid())
					{
						continue;
					}
					const FString Candidate = Addr->ToString(false);
					if (Candidate.IsEmpty() || Candidate.StartsWith(TEXT("127.")))
					{
						continue;
					}
					if (!Candidate.Contains(TEXT(":")))
					{
						Ip = Candidate;
						break;
					}
				}
			}
			LanHostAddressText->SetText(Ip.IsEmpty()
				? LOCTEXT("NoIp", "ADDRESS UNKNOWN.  OTHERS CAN STILL SCAN.")
				: FText::Format(LOCTEXT("HostIp", "TELL THEM  {0}    PORT 7777"), FText::FromString(Ip)));
		}
	}

	if (LanPilotBox)
	{
		LanPilotBox->SetVisibility((bHost || bClient) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bHost || bClient)
		{
			TArray<const APlayerState*> Pilots;
			int32 HostPlayerId = MAX_int32;
			if (World && World->GetGameState())
			{
				for (APlayerState* PS : World->GetGameState()->PlayerArray)
				{
					const ANeonPlayerState* NeonPS = Cast<ANeonPlayerState>(PS);
					if (!PS || (NeonPS && NeonPS->IsAI()))
					{
						continue;
					}
					Pilots.Add(PS);
					HostPlayerId = FMath::Min(HostPlayerId, PS->GetPlayerId());
				}
			}
			for (int32 i = 0; i < LanPilotLabels.Num(); ++i)
			{
				if (!LanPilotLabels[i])
				{
					continue;
				}
				if (!Pilots.IsValidIndex(i))
				{
					LanPilotLabels[i]->SetText(LOCTEXT("OpenSlot", "OPEN"));
					LanPilotLabels[i]->SetColorAndOpacity(FSlateColor(FLinearColor(NeonUI::Text.R, NeonUI::Text.G, NeonUI::Text.B, 0.45f)));
					continue;
				}
				const APlayerState* PS = Pilots[i];
				FString Name = PS->GetPlayerName().TrimStartAndEnd();
				if (Name.IsEmpty())
				{
					Name = TEXT("CONNECTING");
				}
				const bool bThisHost = PS->GetPlayerId() == HostPlayerId;
				LanPilotLabels[i]->SetText(FText::FromString(FString::Printf(TEXT("%s    %s"),
					bThisHost ? TEXT("HOST") : TEXT("PILOT"), *Name.ToUpper())));
				LanPilotLabels[i]->SetColorAndOpacity(FSlateColor(bThisHost ? NeonUI::Accent : NeonUI::Text));
			}
		}
	}

	const TArray<FNeonLanGame>& Games = GI ? GI->GetLanGames() : TArray<FNeonLanGame>();
	if (!bHost && !bClient && Games.Num() == 1 && !Games[0].Address.IsEmpty() && LanAddressBox)
	{
		const FString Current = LanAddressBox->GetText().ToString().TrimStartAndEnd();
		if (Current.IsEmpty() || Current == TEXT("127.0.0.1") || Current == LastAutofillAddress)
		{
			LanAddressBox->SetText(FText::FromString(Games[0].Address));
			LastAutofillAddress = Games[0].Address;
		}
	}
	for (int32 i = 0; i < LanGameRows.Num(); ++i)
	{
		UWidget* Row = LanGameRows[i];
		if (!Row)
		{
			continue;
		}
		const bool bShow = !bHost && !bClient && Games.IsValidIndex(i);
		Row->SetVisibility(bShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bShow && LanGameLabels.IsValidIndex(i) && LanGameLabels[i])
		{
			const FNeonLanGame& G = Games[i];
			const FString Where = G.Address.IsEmpty() ? FString() : FString::Printf(TEXT("    %s"), *G.Address);
			LanGameLabels[i]->SetText(FText::FromString(FString::Printf(TEXT("%s    %d/%d%s"),
				*G.HostName, G.Players, FMath::Max(G.MaxPlayers, 1), *Where)));
		}
	}
}

void UNeonMainMenuWidget::OnLanClicked()
{
	ShowLanPage();
}

void UNeonMainMenuWidget::OnLanBackClicked()
{
	ShowMainPage();
}

void UNeonMainMenuWidget::OnLanHostClicked()
{
	if (LanStatusText)
	{
		LanStatusText->SetText(LOCTEXT("Hosting", "OPENING LINK..."));
	}
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestHostLan();
	}
}

void UNeonMainMenuWidget::OnLanScanClicked()
{
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestFindLan();
	}
}

void UNeonMainMenuWidget::OnLanEngageClicked()
{
	SaveSettings();
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestStartLanMatch(Settings);
	}
}

void UNeonMainMenuWidget::JoinFound(int32 Index)
{
	if (ANeonPlayerController* PC = Cast<ANeonPlayerController>(GetOwningPlayer()))
	{
		PC->RequestJoinLanGame(Index);
	}
}

void UNeonMainMenuWidget::OnJoinFound0() { JoinFound(0); }
void UNeonMainMenuWidget::OnJoinFound1() { JoinFound(1); }
void UNeonMainMenuWidget::OnJoinFound2() { JoinFound(2); }
void UNeonMainMenuWidget::OnJoinFound3() { JoinFound(3); }

void UNeonMainMenuWidget::OnLanJoinClicked()
{
	FString Address = LanAddressBox ? LanAddressBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Address.IsEmpty())
	{
		if (LanStatusText)
		{
			LanStatusText->SetText(LOCTEXT("NeedIp", "ENTER THE HOST IPV4 ADDRESS."));
		}
		return;
	}
	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->LastJoinAddress = Address;
	}
	if (LanStatusText)
	{
		LanStatusText->SetText(FText::Format(LOCTEXT("Joining", "LINKING TO {0}..."), FText::FromString(Address)));
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
