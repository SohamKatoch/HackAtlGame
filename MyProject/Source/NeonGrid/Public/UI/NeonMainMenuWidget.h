#pragma once

#include "Blueprint/UserWidget.h"
#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "NeonMainMenuWidget.generated.h"

class UButton;
class UTextBlock;
class UWidgetSwitcher;

/**
 * Main menu and Solo menu (guide sections 60, 61). Two pages in one widget:
 *
 *   NEON GRID: SOLO / LAN PLAY / OPTIONS / CREDITS / QUIT
 *   SOLO: AI PLAYERS 1 2 3 / DIFFICULTY EASY NORMAL HARD EXPERT / ARENA THE GRID / START / BACK
 *
 * LAN PLAY, OPTIONS and CREDITS are shown disabled until they exist (guide section 63: do not expose
 * what is not implemented). Settings are kept in UNeonGameInstance so the choice persists between
 * matches. START asks the player controller to start the match (server RPC on LAN later).
 *
 * The tree is built in code when no Blueprint subclass provides one; members are BindWidgetOptional.
 */
UCLASS(Blueprintable)
class NEONGRID_API UNeonMainMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UNeonMainMenuWidget(const FObjectInitializer& ObjectInitializer);

	/** Show the first page. Called when the menu becomes visible. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Menu")
	void ShowMainPage();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;

	void BuildTree();
	void BindButtons();
	void LoadSettings();
	void SaveSettings() const;
	void RefreshSelection();

	UFUNCTION() void OnSoloClicked();
	UFUNCTION() void OnQuitClicked();
	UFUNCTION() void OnBackClicked();
	UFUNCTION() void OnStartClicked();
	UFUNCTION() void OnAI1Clicked();
	UFUNCTION() void OnAI2Clicked();
	UFUNCTION() void OnAI3Clicked();
	UFUNCTION() void OnEasyClicked();
	UFUNCTION() void OnNormalClicked();
	UFUNCTION() void OnHardClicked();
	UFUNCTION() void OnExpertClicked();
	UFUNCTION() void OnPowerupsOnClicked();
	UFUNCTION() void OnPowerupsOffClicked();

	void SetAICount(int32 Count);
	void SetDifficulty(ENeonAIDifficulty Level);
	void SetPowerupsEnabled(bool bEnabled);

	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidgetSwitcher> Pages;

	// Main page
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> SoloButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> OptionsButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> CreditsButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> QuitButton;

	// Solo page
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> AI1Button;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> AI2Button;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> AI3Button;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> EasyButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> NormalButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> HardButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> ExpertButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> PowerupsOnButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> PowerupsOffButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ArenaText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> StartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> BackButton;

	FNeonMatchSettings Settings;
	bool bBoundButtons = false;
};
