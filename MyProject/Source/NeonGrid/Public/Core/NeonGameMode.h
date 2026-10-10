#pragma once

#include "AI/NeonAIConfig.h"
#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Vehicle/NeonMovementComponent.h"
#include "NeonGameMode.generated.h"

class ANeonArena;
class ANeonVehiclePawn;
class ANeonAIController;
class ANeonGameState;
class ANeonPlayerState;
class UNeonPowerupConfig;
class UNeonPowerupSpawner;

/**
 * Match and round flow (guide sections 40, 41, 76; Docs/TECHNICAL_SPEC.md section 8). Server-only.
 *
 *   MainMenu -> [StartSoloMatch] -> Countdown (3 s, vehicles frozen) -> Playing -> last vehicle alive
 *   -> RoundResults (winner +1) -> Countdown ... -> first to ScoreLimit -> MatchResults -> Rematch or MainMenu.
 *
 * The menu is shown over the arena in the same level (DECISIONS.md D24): no vehicles exist while in
 * MainMenu; the player controller views the arena's fixed camera. Humans are spawned when a match
 * starts; AI opponents fill the remaining slots (cap 4). Scores live on ANeonPlayerState; phase, round,
 * timers and winners on ANeonGameState. Vehicles are frozen outside Playing.
 */
UCLASS()
class NEONGRID_API ANeonGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ANeonGameMode();

	/** Spawned at the origin if the level has no arena actor. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Arena")
	TSubclassOf<ANeonArena> ArenaClass;

	/** Guide: 3, 2, 1, GO. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Match", meta = (ClampMin = "0"))
	float CountdownDuration = 3.f;

	/** Seconds the round winner is shown before the next countdown. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Match", meta = (ClampMin = "0"))
	float RoundResultsDuration = 3.f;

	/** False starts a match immediately with the game instance settings (also -NeonAutoStart). */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Match")
	bool bStartInMainMenu = true;

	/** Null uses UNeonAIConfig class defaults. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|AI")
	TObjectPtr<UNeonAIConfig> AIConfig;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|AI")
	TSubclassOf<ANeonAIController> AIControllerClass;

	/** Base seed for AI random streams (seed + slot). Same seed, same AI behavior given the same inputs. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|AI")
	int32 AISeed = 1337;

	/** Per-slot colors until the lobby lets players choose. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Players")
	TArray<FLinearColor> SlotColors;

	/** Null uses UNeonPowerupConfig class defaults. Shared by the spawner and every vehicle. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Powerups")
	TObjectPtr<UNeonPowerupConfig> PowerupConfig;

	/** Authority-side pickup spawner; active only during Playing (Milestone 4). */
	UPROPERTY(VisibleAnywhere, Category = "Neon|Powerups")
	TObjectPtr<UNeonPowerupSpawner> PowerupSpawner;

	ANeonArena* GetArena() const { return Arena; }
	ANeonGameState* GetNeonGameState() const;

	// ---- Match control (called by the player controller's server RPCs; offline that is a direct call) ----

	/** From MainMenu: store settings, spawn humans and AI, start round 1. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void StartSoloMatch(const FNeonMatchSettings& Settings);

	/** From MatchResults: zero the scores and start round 1 with the same players. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void Rematch();

	/** From any phase: remove every vehicle and AI, clear trails, show the menu. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void ReturnToMainMenu();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual bool PlayerCanRestart_Implementation(APlayerController* Player) override;

protected:
	UFUNCTION()
	void HandleVehicleEliminated(AActor* Vehicle, ENeonEliminationCause Cause);

	void BeginCountdown();
	void BeginPlaying();
	void EndRound(int32 WinnerSlot);
	void BeginMatchResults(int32 WinnerSlot);
	void ClearTimers();

	void InitVehicle(ANeonVehiclePawn* Vehicle, int32 Slot, const FTransform* SpawnOverride = nullptr);
	/** AI spawn for this round. Humans keep the fixed corners. */
	FTransform MakeSpawnTransform(int32 Slot, bool bAI, FRandomStream& Rng, TArray<FVector2D>& Occupied) const;
	void SpawnAIPlayers(int32 Count, ENeonAIDifficulty Level);
	void DestroyAllVehicles();
	void ResetVehiclesToSpawn();
	void SetAllFrozen(bool bFrozen);
	void ResetScores();
	int32 CountHumans() const;
	int32 GetPlayerSlot(const AController* Controller) const;
	void EnsureArena();
	ANeonPlayerState* FindPlayerStateForVehicle(const ANeonVehiclePawn* Vehicle) const;
	FString SlotLabel(int32 Slot) const;

	UPROPERTY(Transient)
	TObjectPtr<ANeonArena> Arena;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ANeonVehiclePawn>> Vehicles;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ANeonAIController>> AIControllers;

	FTimerHandle PhaseTimer;

	/** Simulation tick of each vehicle when the current round began; for survival-time logging. */
	TMap<TWeakObjectPtr<ANeonVehiclePawn>, int32> RoundStartTicks;

	/** Headless test hook (-NeonRoundEndsAtZero): the round ends only when nobody is alive. */
	bool bRoundEndsAtZero = false;
};
