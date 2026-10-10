#pragma once

#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "NeonGameState.generated.h"

class ANeonPlayerState;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNeonPhaseChangedSignature, ENeonMatchPhase, NewPhase);

/**
 * Replicated match state: phase, round number, score limit, phase timer, round and match winners.
 * Docs/TECHNICAL_SPEC.md section 4. The game mode (server) writes it; HUD and menus read it on every
 * machine. Scores live on each ANeonPlayerState.
 */
UCLASS()
class NEONGRID_API ANeonGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	ANeonGameState();

	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	ENeonMatchPhase GetPhase() const { return Phase; }

	/** 1-based. 0 before the first round. */
	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	int32 GetRoundNumber() const { return RoundNumber; }

	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	int32 GetScoreLimit() const { return ScoreLimit; }

	/** Seconds until the current timed phase (Countdown, RoundResults) ends. 0 for untimed phases. */
	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	float GetPhaseTimeRemaining() const;

	/** Slot of the last round's winner, or -1 (none yet, or everyone died on the same tick). */
	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	int32 GetRoundWinnerSlot() const { return RoundWinnerSlot; }

	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	int32 GetMatchWinnerSlot() const { return MatchWinnerSlot; }

	UFUNCTION(BlueprintPure, Category = "Neon|Match")
	ANeonPlayerState* FindPlayerStateBySlot(int32 Slot) const;

	/** Fires on the server when the phase is set and on clients when it replicates. */
	UPROPERTY(BlueprintAssignable, Category = "Neon|Match")
	FNeonPhaseChangedSignature OnPhaseChanged;

	// ---- Server-only writers (game mode) ----

	void SetPhase(ENeonMatchPhase NewPhase, float Duration);
	void SetRoundNumber(int32 InRound) { RoundNumber = InRound; }
	void SetScoreLimit(int32 InLimit) { ScoreLimit = InLimit; }
	void SetRoundWinnerSlot(int32 InSlot) { RoundWinnerSlot = InSlot; }
	void SetMatchWinnerSlot(int32 InSlot) { MatchWinnerSlot = InSlot; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UFUNCTION()
	void OnRep_Phase();

	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	ENeonMatchPhase Phase = ENeonMatchPhase::MainMenu;

	UPROPERTY(Replicated)
	int32 RoundNumber = 0;

	UPROPERTY(Replicated)
	int32 ScoreLimit = 5;

	/** Server world time at which the current timed phase ends. <= 0 means untimed. */
	UPROPERTY(Replicated)
	float PhaseEndServerTime = 0.f;

	UPROPERTY(Replicated)
	int32 RoundWinnerSlot = -1;

	UPROPERTY(Replicated)
	int32 MatchWinnerSlot = -1;
};
