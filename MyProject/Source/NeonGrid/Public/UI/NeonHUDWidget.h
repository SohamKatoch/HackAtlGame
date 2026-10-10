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
class ANeonGameState;
class ANeonPlayerState;
class ANeonArena;
class UNeonPowerupComponent;
class UNeonMovementComponent;

/**
 * In-match HUD (guide section 69).
 *
 *   Top left:      standings panel, one row per slot (color swatch, name, wins), dimmed when out.
 *   Top center:    round number / score limit, speed readout, closing-walls warning (DECISIONS.md D27).
 *   Top right:     vehicles still alive.
 *   Corners:       bracket ticks. Countdown shows a NEON GRID boot title (D29).
 *   Center:        countdown, GO, round / match result; in MatchResults the final standings and
 *                  REMATCH / MAIN MENU buttons. A dead player sees who the camera is following.
 *   Bottom left:   sprint meter (hold Shift). Bottom right: jump meter (Ctrl / F).
 *   Bottom center: the powerup slot (held name, ACTIVE / ARMED, time bar) or STEERING JAMMED (M4).
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

	// Top center / right
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RoundText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SpeedText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CollapseText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AliveText;

	// Boot title over the countdown, and the four corner brackets.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BootTitle;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BootSub;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> CornerBrackets;

	// Sprint (bottom left) and jump (bottom right).
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
