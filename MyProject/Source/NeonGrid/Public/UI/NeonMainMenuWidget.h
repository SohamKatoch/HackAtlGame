#pragma once

#include "Blueprint/UserWidget.h"
#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "NeonMainMenuWidget.generated.h"

class UButton;
class UTextBlock;
class UWidgetSwitcher;
class UEditableTextBox;

/**
 * Main menu, Solo menu, and LAN host/join (guide sections 60, 61). Three pages in one widget:
 *
 *   NEON GRID: SOLO / LAN PLAY / OPTIONS / CREDITS / QUIT
 *   SOLO: AI, difficulty, powerups, closing walls, START
 *   LAN: HOST / SCAN NETWORK / join a listed game / JOIN BY ADDRESS / ENGAGE / BACK
 *
 * OPTIONS and CREDITS stay disabled until they exist. START / HOST go through the player controller.
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
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

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
	UFUNCTION() void OnCollapseOnClicked();
	UFUNCTION() void OnCollapseOffClicked();
	UFUNCTION() void OnLanClicked();
	UFUNCTION() void OnLanHostClicked();
	UFUNCTION() void OnLanScanClicked();
	UFUNCTION() void OnLanJoinClicked();
	UFUNCTION() void OnLanEngageClicked();
	UFUNCTION() void OnLanBackClicked();
	UFUNCTION() void OnJoinFound0();
	UFUNCTION() void OnJoinFound1();
	UFUNCTION() void OnJoinFound2();
	UFUNCTION() void OnJoinFound3();

	void ShowLanPage();
	void RefreshLanPage();
	void JoinFound(int32 Index);

	void SetAICount(int32 Count);
	void SetDifficulty(ENeonAIDifficulty Level);
	void SetPowerupsEnabled(bool bEnabled);
	void SetCollapseEnabled(bool bEnabled);

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
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> CollapseOnButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> CollapseOffButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ArenaText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> StartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> BackButton;

	// LAN page
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanHostButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanScanButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanJoinButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanEngageButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> LanBackButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> LanAddressBox;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> LanStatusText;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> LanGameRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> LanGameLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UButton>> LanGameJoinButtons;

	FNeonMatchSettings Settings;
	bool bBoundButtons = false;
};
