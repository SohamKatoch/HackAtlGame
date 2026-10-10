#pragma once

#include "Blueprint/UserWidget.h"
#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "NeonHUDWidget.generated.h"

class UTextBlock;
class UButton;
class UWidget;
class UProgressBar;
class ANeonGameState;
class ANeonPlayerState;
class UNeonPowerupComponent;

/**
 * In-match HUD (guide section 69): one score entry per slot in the four corners, round number top
 * center, and a center message for countdown / round result / match result. In MatchResults it also
 * shows REMATCH and MAIN MENU buttons. Bottom center: the powerup slot (held name, ACTIVE / ARMED
 * state, a bar for the time left) or a STEERING JAMMED warning while hit by EMP (Milestone 4).
 *
 * The tree is built in code when no Blueprint subclass provides one. Named members are
 * BindWidgetOptional so a designer can lay the same elements out in UMG and keep this logic.
 * Reads ANeonGameState / ANeonPlayerState every tick; writes nothing.
 */
UCLASS(Blueprintable)
class NEONGRID_API UNeonHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** How long "GO" stays on screen after the countdown. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|HUD", meta = (ClampMin = "0"))
	float GoDisplaySeconds = 0.8f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void BuildTree();
	void UpdateScores(const ANeonGameState& GS, const ANeonPlayerState* LocalPS);
	void UpdateCenter(const ANeonGameState& GS, const ANeonPlayerState* LocalPS, float DeltaTime);
	void SetCenter(const FText& Big, const FText& Small, const FLinearColor& BigColor);
	void UpdatePowerupPanel(const ANeonGameState& GS);
	const UNeonPowerupComponent* FindLocalPowerups() const;
	FText NameForSlot(const ANeonGameState& GS, int32 SlotIndex) const;
	FLinearColor ColorForSlot(const ANeonGameState& GS, int32 SlotIndex) const;

	UFUNCTION()
	void OnRematchClicked();

	UFUNCTION()
	void OnMainMenuClicked();

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> Score0;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> Score1;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> Score2;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> Score3;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RoundText;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> CenterBig;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> CenterSmall;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UWidget> ResultsButtons;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> RematchButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> MainMenuButton;

	// Powerup slot (bottom center)
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UWidget> PowerupPanel;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PowerupName;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PowerupHint;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> PowerupBar;

	ENeonMatchPhase LastPhase = ENeonMatchPhase::MainMenu;
	float TimeInPhase = 0.f;
	bool bBoundButtons = false;
};
