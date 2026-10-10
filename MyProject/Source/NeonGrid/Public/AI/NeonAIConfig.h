#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NeonAIConfig.generated.h"

/** Solo menu difficulty choices. Docs/AI_SPEC.md section 5. */
UENUM(BlueprintType)
enum class ENeonAIDifficulty : uint8
{
	Easy,
	Normal,
	Hard,
	Expert
};

/**
 * One difficulty level. Changes how the AI thinks, never how the vehicle moves
 * (speed, radius, and collision come from UNeonVehicleConfig and are shared with humans).
 */
USTRUCT(BlueprintType)
struct NEONGRID_API FNeonAIDifficultyParams
{
	GENERATED_BODY()

	/** Seconds between full decisions. Jittered ±20% so several AIs do not think in lockstep. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.02"))
	float DecisionInterval = 0.2f;

	/** Minimum seconds between emergency re-decisions when the committed arc turns lethal. The "reaction time". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float ReactionTime = 0.18f;

	/**
	 * Seconds of travel each candidate plan is predicted. Look-ahead distance = speed * this.
	 * Keep it short enough that "survives the whole plan" is attainable in open space (about 3 turn radii);
	 * long horizons make every plan end at a wall and the scoring degenerates. Space beyond the horizon is
	 * measured by the openness probe and the escape test, not by extending this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.2"))
	float LookAheadTime = 1.0f;

	/** Length in cm of the straight probes cast from a plan's end to measure open space. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "200"))
	float OpennessProbeDistance = 3000.f;

	/** Candidate steer magnitudes per side (plus straight). 3 = {1/3, 2/3, 1} left and right. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1", ClampMax = "8"))
	int32 ArcsPerSide = 3;

	/** Polyline steps per predicted arc. More = finer hazard detection, more sweeps. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "2", ClampMax = "32"))
	int32 PredictionSteps = 10;

	/**
	 * Turn-then-straight plans per steering value: how many of {90, 45, 135, 30, 180} degree heading changes
	 * are considered (in that order). More angles = more exits seen, more sweeps per decision.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1", ClampMax = "5"))
	int32 TurnAnglesPerArc = 3;

	/**
	 * Opponents are extrapolated straight for at most this many seconds when predicting a plan.
	 * They will steer too; treating their whole straight line as lethal makes the AI panic at head-on spawns.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float OpponentExtrapolationTime = 0.6f;

	/** Extra cm added to the collision radius during prediction. Cautious drivers keep more distance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float SafetyMargin = 30.f;

	/** Chance (0-1) a decision picks a random surviving arc instead of the best one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0", ClampMax = "1"))
	float MistakeChance = 0.1f;

	/** Weight of open space probed at the end of each arc (0 ignores the future, 1 is as important as surviving the arc). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float OpennessWeight = 0.5f;

	/**
	 * Weight of reachable area (flood fill on a coarse grid) from each plan's end, 0..1 of the arena.
	 * This is what stops the AI from cutting itself into the smaller region. 0 disables the fill (Easy).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float SpaceWeight = 1.5f;

	/** Cell size in cm for the reachable-area grid. Smaller is more precise and slower. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "100"))
	float SpaceCellSize = 250.f;

	/** Weight for steering toward a cut-off point ahead of the nearest opponent. 0 = pure survival. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float ChaseWeight = 0.3f;

	/** Seconds ahead of the opponent the cut-off point is placed. Larger = cross their path earlier, further from a ram. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float CutOffLeadTime = 1.2f;

	/** Preference for small steering values when arcs otherwise tie. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float StraightBias = 0.1f;

	/** Hysteresis: bonus for continuing the committed plan (its remaining turn, then straight) so the vehicle does not wobble. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float CommitBias = 0.3f;

	/**
	 * Penalty for turning further in a direction the vehicle has already been turning for a while
	 * (decayed heading-change accumulator past 90 degrees). Stops the AI drawing a circle and trapping itself.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float LoopAversion = 0.6f;

	/**
	 * 0-1. How well the AI uses powerups: the chance a sensible activation moment is taken when it comes up
	 * (Phase / Shield / Trail Cutter when the committed plan is in danger, Boost / Overdrive on an open
	 * straight, EMP when an opponent is in range). Low values also waste powerups at random moments.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0", ClampMax = "1"))
	float PowerupSkill = 0.5f;

	/** Weight for steering toward the nearest pickup while the slot is empty. 0 ignores pickups. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float CollectWeight = 0.5f;

	/** Pickups further than this (cm) are not worth a detour. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0"))
	float CollectRange = 4000.f;
};

/**
 * All four difficulty levels in one Data Asset. Docs/AI_SPEC.md section 5.
 * If the game mode has no asset assigned, the class defaults below are used.
 */
UCLASS(BlueprintType)
class NEONGRID_API UNeonAIConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UNeonAIConfig();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|AI")
	TMap<ENeonAIDifficulty, FNeonAIDifficultyParams> Levels;

	/** Falls back to Normal, then to struct defaults, if a level is missing from the map. */
	const FNeonAIDifficultyParams& GetParams(ENeonAIDifficulty Difficulty) const;
};
