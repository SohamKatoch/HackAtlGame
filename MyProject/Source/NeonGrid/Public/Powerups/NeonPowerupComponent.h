#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Powerups/NeonPowerupTypes.h"
#include "NeonPowerupComponent.generated.h"

class UNeonPowerupConfig;
class UNeonMovementComponent;
class UNeonCollisionSystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNeonPowerupEventSignature, ENeonPowerupType, Type, ENeonPowerupEvent, Event);

/**
 * One vehicle's powerup slot and active effects. Docs/GAME_DESIGN.md section 9, TECHNICAL_SPEC section 7.
 *
 * Inventory: one held type (D7). Pickup is refused while holding. Activation is manual: the movement
 * component forwards the input's activate edge into PreStep() on the simulation tick it happens, so
 * humans, AI and (later) LAN clients all activate through the same path and on the same tick.
 *
 * Effects are counted in simulation ticks (converted from the config's seconds when they start):
 *   Boost / Overdrive  speed and turn-rate multipliers read by the movement step
 *   Shield             charge(s); TryAbsorbTrailHit() spends one and grants a few ticks of pass-through
 *   Phase              a forward jump; trail sweeps are ignored for the hop; the boundary and vehicles still kill
 *   EMP                applied to the nearest opponent in range: their steering multiplier drops
 *   Trail Cutter       immediate: cuts a hole in the first trail ahead (refused if nothing is in reach)
 *
 * Solo and host run this same code (D18). Nothing here reads the camera, the renderer, or the HUD.
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonPowerupComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNeonPowerupComponent();

	/** Tuning asset. Null uses UNeonPowerupConfig class defaults. Set by the game mode. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Powerups")
	TObjectPtr<UNeonPowerupConfig> Config;

	const UNeonPowerupConfig* GetConfig() const;

	// ---- Inventory ----

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	ENeonPowerupType GetHeld() const { return Held; }

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	bool CanCollect() const { return Held == ENeonPowerupType::None; }

	/** Server: put a pickup in the slot. False if the slot is full or Type is None. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Powerups")
	bool Collect(ENeonPowerupType Type);

	// ---- Active state (HUD / visuals / AI) ----

	/** Timed effect running now (Boost, Overdrive, Phase, or Shield while armed). None otherwise. */
	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	ENeonPowerupType GetActiveType() const { return ActiveType; }

	/** 1 at activation, 0 at expiry. Shield with no time limit reports 1. */
	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	float GetActiveFraction() const;

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	float GetActiveSecondsRemaining() const;

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	bool HasShield() const { return ShieldCharges > 0; }

	/** Trail sweeps are ignored this tick (Phase running, or the shield just popped). */
	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	bool IsPhased() const;

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	bool IsEMPd() const { return EMPTicksRemaining > 0; }

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	float GetEMPFraction() const;

	/** Product of every active speed effect. 1 = none. */
	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	float GetSpeedMultiplier() const;

	/** Product of every active turn-rate effect (Boost, Overdrive, EMP). 1 = none. */
	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	float GetTurnRateMultiplier() const;

	// ---- Simulation hooks (movement component, in step order) ----

	/**
	 * Called at the start of every simulation step. Counts effects down one tick and, if an activation
	 * was requested since the last step, tries to activate the held powerup.
	 */
	void PreStep(bool bActivateRequested);

	/** A trail sweep hit. Returns true if a shield charge absorbed it (the vehicle survives). */
	bool TryAbsorbTrailHit();

	/** Victim side of EMP. */
	void ApplyEMP(int32 DurationTicks, float SteeringMultiplier);

	/** Round reset: empty the slot, stop every effect. */
	void ResetForRound();

	UPROPERTY(BlueprintAssignable, Category = "Neon|Powerups")
	FNeonPowerupEventSignature OnPowerupEvent;

protected:
	virtual void BeginPlay() override;

private:
	bool TryActivate();
	bool ActivateBoost();
	bool ActivateOverdrive();
	bool ActivateShield();
	bool ActivatePhase();
	bool ActivateTrailCutter();
	bool ActivateEMP();

	int32 SecondsToTicks(float Seconds) const;
	void StartTimed(ENeonPowerupType Type, int32 Ticks);
	void ExpireActive();

	UPROPERTY(Transient)
	TObjectPtr<UNeonMovementComponent> Movement;

	UPROPERTY(Transient)
	TObjectPtr<UNeonCollisionSystem> Collision;

	ENeonPowerupType Held = ENeonPowerupType::None;

	ENeonPowerupType ActiveType = ENeonPowerupType::None;
	/** <0 = no time limit (armed shield). */
	int32 ActiveTicksRemaining = 0;
	int32 ActiveTicksTotal = 0;

	int32 ShieldCharges = 0;
	int32 PassThroughTicksRemaining = 0;

	int32 EMPTicksRemaining = 0;
	int32 EMPTicksTotal = 0;
	float EMPSteeringMultiplier = 1.f;
};
