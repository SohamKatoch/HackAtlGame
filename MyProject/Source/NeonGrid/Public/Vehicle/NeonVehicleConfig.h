#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NeonVehicleConfig.generated.h"

/**
 * Tunable vehicle parameters. Docs/VEHICLE_SPEC.md section 7.
 *
 * Units are Unreal centimeters and seconds. The guide's "baseSpeed = 12" is a tuning token,
 * not a literal cm/s value (see Docs/DECISIONS.md D4). Class defaults below are the starting
 * targets; create a Data Asset of this class in the Editor to tune without recompiling.
 */
UCLASS(BlueprintType)
class NEONGRID_API UNeonVehicleConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// ---- Speed ----

	/** Cruise speed in cm/s. MVP speed is constant at this value unless a powerup overrides it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "1"))
	float BaseSpeed = 2200.f;

	/** Speed floor in cm/s. Equal to cruise in the MVP (no acceleration model yet). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "1"))
	float MinSpeed = 2200.f;

	// ---- Steering ----

	/**
	 * Max heading change in degrees per second at full steer, before the radius clamp.
	 * Guide target was 120; first playtest read as sluggish, raised to 180.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steering", meta = (ClampMin = "1"))
	float MaxTurnRate = 180.f;

	/**
	 * Minimum turn radius in cm. Turn rate is clamped so radius = speed / turnRate never drops
	 * below this. Together with MinSpeed this makes an instant 180 impossible. 0 disables the clamp.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steering", meta = (ClampMin = "0"))
	float MinTurnRadius = 400.f;

	// ---- Collision ----

	/** Gameplay radius in cm. Independent of the visual mesh and of lean. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ClampMin = "1"))
	float CollisionRadius = 60.f;

	/** The trail is emitted this far behind the vehicle center, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ClampMin = "0"))
	float TrailRearOffset = 70.f;

	/**
	 * Own trail geometry updated within this many simulation ticks is ignored for self-collision,
	 * so the vehicle does not hit the segment it is still drawing. Older own trail is lethal.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ClampMin = "1", ClampMax = "60"))
	int32 SelfTrailGraceTicks = 10;

	// ---- Visual lean (cosmetic only) ----

	/** Hard cap on the visual roll, degrees. Guide target 15-25. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0", ClampMax = "45"))
	float MaxLeanAngle = 20.f;

	/** Lean at about one third steer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0"))
	float LeanAngleLow = 5.f;

	/** Lean at about two thirds steer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0"))
	float LeanAngleMid = 12.f;

	/** Lean at full steer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0"))
	float LeanAngleHigh = 20.f;

	/** Interp speed toward a larger lean. Higher is snappier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0.1"))
	float LeanResponseSpeed = 20.f;

	/** Interp speed back toward neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lean", meta = (ClampMin = "0.1"))
	float LeanReturnSpeed = 7.f;

	// Powerup multipliers (Boost, Overdrive, EMP) live on UNeonPowerupConfig since Milestone 4.

	/** Piecewise-linear lean curve through (0,0), (1/3, Low), (2/3, Mid), (1, High), capped by MaxLeanAngle. */
	float EvaluateLeanAngle(float AbsSteer) const;
};
