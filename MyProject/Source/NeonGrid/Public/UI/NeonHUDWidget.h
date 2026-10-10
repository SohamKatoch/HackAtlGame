#pragma once

#include "Blueprint/UserWidget.h"
#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "NeonHUDWidget.generated.h"

class UTextBlock;
class UButton;
class UWidget;
class UProgressBar;
class UVerticalBox;
class UCanvasPanelSlot;
class ANeonGameState;
class ANeonPlayerState;
class ANeonArena;
class UNeonPowerupComponent;
class UNeonMovementComponent;

/**
 * In-match HUD (guide section 69).
 *
 *   Top left:      [ P# ] name, blue score line. Local rider is highlight-blue.
 *   Top center:    speed (KM/H), the strongest HUD mark. Perimeter warning under it (D27).
 *   Top right:     black tactical map, thin blue field, blue dots.
 *   Center:        03 / ENGAGE, then ROUND N for a short beat. Match results are MATCH COMPLETE.
 *   Bottom:        BOOST and JUMP meters on a black gauge. Powerup slot is a blue READY frame.
 *
 * The tree is built in code when no Blueprint subclass provides one. Named members are
 * BindWidgetOptional so a designer can lay the same elements out in UMG and keep this logic.
 * Reads ANeonGameState / ANeonPlayerState / ANeonArena every tick; writes nothing.
 */
UCLASS(Blueprintable)
class NEONGRID_API UNeonHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** How long "GO" stays on screen after the countdown. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|HUD", meta = (ClampMin = "0"))
	float GoDisplaySeconds = 0.8f;

	/** How long ROUND N stays after GO, then it collapses. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|HUD", meta = (ClampMin = "0"))
	float RoundDisplaySeconds = 1.8f;

	/** The closing-walls countdown appears this many seconds before the walls start. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|HUD", meta = (ClampMin = "0"))
	float CollapseWarningSeconds = 10.f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void BuildTree();
	void UpdateStandings(const ANeonGameState& GS, const ANeonPlayerState* LocalPS);
	void UpdateTopCenter(const ANeonGameState& GS);
	void UpdateCenter(const ANeonGameState& GS, const ANeonPlayerState* LocalPS);
	void UpdateResultsStandings(const ANeonGameState& GS, const ANeonPlayerState* LocalPS, bool bShow);
	void SetCenter(const FText& Big, const FText& Small, const FLinearColor& BigColor, const FLinearColor& SmallColor);
	void UpdatePowerupPanel(const ANeonGameState& GS);
	void UpdateRideHUD(const ANeonGameState& GS);
	void UpdateMinimap(const ANeonGameState& GS);
	void UpdateBoot(const ANeonGameState& GS);
	const UNeonPowerupComponent* FindLocalPowerups() const;
	const UNeonMovementComponent* FindLocalMovement() const;
	const ANeonArena* FindArena() const;
	FText NameForSlot(const ANeonGameState& GS, int32 SlotIndex) const;
	FLinearColor ColorForSlot(const ANeonGameState& GS, int32 SlotIndex) const;

	UFUNCTION()
	void OnRematchClicked();

	UFUNCTION()
	void OnMainMenuClicked();

	// Standings (top left): one row per slot.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> StandingsPanel;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> StandingRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> StandingSwatches;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StandingNames;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StandingWins;
	UPROPERTY(Transient) TArray<TObjectPtr<UProgressBar>> StandingBars;

	// Top-right grid map. One dot per slot, moved every tick.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> MapPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> MapField;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> MapDots;
	UPROPERTY(Transient) TArray<TObjectPtr<UCanvasPanelSlot>> MapDotSlots;

	// "round N" over the bottom gauge during play, with a short red mark under it.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> RoundPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RoundLabel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> RoundTick;

	// Top center / right. Speed is split so the fraction can sit smaller next to the whole number.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RoundText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SpeedText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SpeedFracText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CollapseText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AliveText;

	// Boot title over the countdown, and the four corner brackets.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BootTitle;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BootSub;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> CornerBrackets;

	// Bottom gauge: boost meter on the left, jump meter on the right. SprintPanel is the whole gauge.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> SprintPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SprintLabel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> SprintBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> JumpPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> JumpLabel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> JumpBar;

	// Center
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CenterBig;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CenterSmall;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> ResultsPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> ResultsRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> ResultsTexts;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> ResultsButtons;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> RematchButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> MainMenuButton;

	// Powerup slot (bottom center)
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UWidget> PowerupPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PowerupName;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PowerupHint;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> PowerupBar;

	ENeonMatchPhase LastPhase = ENeonMatchPhase::MainMenu;
	float TimeInPhase = 0.f;
	float Pulse = 0.f;
	bool bBoundButtons = false;
};
