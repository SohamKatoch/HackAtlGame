#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Powerups/NeonPowerupTypes.h"
#include "NeonPowerupConfig.generated.h"

/**
 * Spawning rules and per-type tuning for every powerup. Docs/TECHNICAL_SPEC.md section 7.
 *
 * One asset covers all six types so the spawner, the vehicle component and the AI read the same
 * numbers. Class defaults are the starting values; author a Data Asset of this class and assign it on
 * the game mode to tune without recompiling. Durations are in seconds and converted to simulation
 * ticks when an effect starts, so expiry is deterministic.
 */
UCLASS(BlueprintType)
class NEONGRID_API UNeonPowerupConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UNeonPowerupConfig();

	// ---- Spawning (guide: cap 2-4, start at 3; never inside a trail; not right beside a player) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxActivePowerups = 3;

	/** Seconds between spawn attempts while under the cap. The guide's "powerup frequency". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0.5"))
	float SpawnInterval = 5.f;

	/** Seconds after GO before the first spawn attempt. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	float FirstSpawnDelay = 3.f;

	/** Seconds a pickup waits before vanishing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "1"))
	float PickupLifetime = 14.f;

	/** Collection radius around the pickup, cm (added to the vehicle's collision radius). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "10"))
	float PickupRadius = 110.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	float MinDistanceFromVehicles = 1200.f;

	/** Clearance from any trail, cm. "Never spawn inside a trail." */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	float MinDistanceFromTrails = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	float MinDistanceBetweenPickups = 1500.f;

	/** Candidate points are a lattice over the arena at this spacing, inset from the walls. "Predefined, safely randomized." */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "200"))
	float SpawnPointSpacing = 1500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	float SpawnPointInset = 900.f;

	/** Relative chance of each type. Missing or zero entries never spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning")
	TMap<ENeonPowerupType, float> SpawnWeights;

	// ---- Boost (guide: 12 -> 22 for 1-2 s, wider radius) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boost", meta = (ClampMin = "1"))
	float BoostSpeedMultiplier = 22.f / 12.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boost", meta = (ClampMin = "0.05", ClampMax = "1"))
	float BoostTurnRateMultiplier = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boost", meta = (ClampMin = "0.1"))
	float BoostDuration = 1.2f;

	// ---- Shield (one charge; the next trail hit is absorbed) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shield", meta = (ClampMin = "1", ClampMax = "3"))
	int32 ShieldCharges = 1;

	/** Seconds an unused charge lasts. 0 = until used. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shield", meta = (ClampMin = "0"))
	float ShieldDuration = 0.f;

	/** Ticks of trail immunity after a charge pops, so the vehicle clears the trail it just hit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shield", meta = (ClampMin = "1", ClampMax = "60"))
	int32 ShieldPassThroughTicks = 12;

	// ---- Phase (pass through trails; the boundary still kills) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Phase", meta = (ClampMin = "0.1"))
	float PhaseDuration = 2.0f;

	// ---- EMP (steering-rate reduction on the nearest opponent in range; D5) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "EMP", meta = (ClampMin = "100"))
	float EMPRange = 2500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "EMP", meta = (ClampMin = "0.1"))
	float EMPDuration = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "EMP", meta = (ClampMin = "0.05", ClampMax = "1"))
	float EMPSteeringMultiplier = 0.4f;

	// ---- Trail Cutter (removes a short piece of whatever trail is ahead) ----

	/** How far ahead of the nose the cutter looks for trail, cm. Nothing in reach = activation refused, powerup kept. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trail Cutter", meta = (ClampMin = "100"))
	float TrailCutterReach = 1500.f;

	/** Diameter of the hole cut around the first trail found, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trail Cutter", meta = (ClampMin = "50"))
	float TrailCutterLength = 700.f;

	// ---- Overdrive (faster than Boost, much worse steering) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overdrive", meta = (ClampMin = "1"))
	float OverdriveSpeedMultiplier = 1.6f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overdrive", meta = (ClampMin = "0.05", ClampMax = "1"))
	float OverdriveTurnRateMultiplier = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overdrive", meta = (ClampMin = "0.1"))
	float OverdriveDuration = 1.5f;

	/** Weighted random type using the stream. None if every weight is zero. */
	ENeonPowerupType PickRandomType(FRandomStream& Rng) const;
};
