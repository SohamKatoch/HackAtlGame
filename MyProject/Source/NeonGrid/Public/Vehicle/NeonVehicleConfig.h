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

	/**
	 * Cruise speed in cm/s. MVP speed is constant at this value unless a powerup overrides it.
	 * Was 2200, then 1700; playtest still ended rounds in a few seconds, so cruise is 1400 (D27).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "1"))
	float BaseSpeed = 1400.f;

	/** Speed floor in cm/s. Equal to cruise in the MVP (no acceleration model yet). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "1"))
	float MinSpeed = 1400.f;

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

	/**
	 * A laser only hits when the vehicle center is this close to the laser's Z, cm.
	 * Same-floor trails still kill. A jump clears a wall that is below the bike.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ClampMin = "10"))
	float TrailVerticalRadius = 90.f;

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

	// ---- Sprint (DECISIONS.md D29). A full bar is one committed burst, then a long refill. ----

	/** Speed multiplier during the burst. Stacks with Boost. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "1"))
	float SprintSpeedMultiplier = 1.85f;

	/** Turn-rate multiplier during the burst, before the radius clamp. The wide line is the risk. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "0.1", ClampMax = "1"))
	float SprintTurnMultiplier = 0.55f;

	/** How long one full-bar sprint lasts, seconds. The bar must be full to start it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "0.2"))
	float SprintBurstSeconds = 0.85f;

	/** Fraction restored per second after the burst. ~22 s from empty back to a full bar. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "0.01"))
	float SprintRechargePerSecond = 0.045f;

	// ---- Jump meter and air (D29) ----

	/** Fraction of the jump meter one hop spends. 1 = the bar must be full, and the hop empties it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "0.05", ClampMax = "1"))
	float JumpCost = 1.f;

	/** Fraction restored per second, on the ground and in the air. ~22 s from empty back to a hop. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "0.01"))
	float JumpRechargePerSecond = 0.045f;

	/** Upward speed added by a jump, cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "100"))
	float JumpImpulse = 1050.f;

	/** Downward acceleration in the air, cm/s^2. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "100"))
	float Gravity = 2400.f;

	/**
	 * A ramp launches the bike when the climb built at least this much upward speed (cm/s)
	 * and the floor then drops away. Cruise on the gentle pyramid stays glued; sprinting off
	 * a tier, and every authored launch ramp, clears it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "0"))
	float LaunchMinUpSpeed = 820.f;

	/** Floor slope (rise/run) below this, after a climb, counts as a lip. Negative. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMax = "0"))
	float LaunchDropSlope = -0.2f;

	/** Two vehicles farther apart than this on Z do not collide. A hop clears a bike. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "20"))
	float AirCollisionHeight = 180.f;

	/** Fraction of the climb's upward speed kept when leaving a lip. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Jump", meta = (ClampMin = "0", ClampMax = "1.5"))
	float LaunchCarry = 0.9f;

	// Powerup multipliers (Boost, Overdrive, EMP) live on UNeonPowerupConfig since Milestone 4.

	/** Piecewise-linear lean curve through (0,0), (1/3, Low), (2/3, Mid), (1, High), capped by MaxLeanAngle. */
	float EvaluateLeanAngle(float AbsSteer) const;
};
