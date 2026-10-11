#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Vehicle/NeonVehicleInput.h"
#include "NeonMovementComponent.generated.h"

class UNeonVehicleConfig;
class UNeonCollisionSystem;
class UNeonTrailComponent;
class UNeonPowerupComponent;
class ANeonArena;

UENUM(BlueprintType)
enum class ENeonEliminationCause : uint8
{
	None,
	/** Left the arena rectangle. */
	Boundary,
	/** Hit another player's trail. */
	Trail,
	/** Hit own trail. */
	OwnTrail,
	/** Vehicle-to-vehicle contact. Both vehicles are eliminated. */
	Vehicle
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNeonVehicleEliminatedSignature, AActor*, Vehicle, ENeonEliminationCause, Cause);

/**
 * Kinematic arcade movement. Docs/VEHICLE_SPEC.md sections 1-2, Docs/TECHNICAL_SPEC.md sections 3 and 5.
 *
 *   velocity = direction(heading) * speed
 *   heading += steering * turnRate(speed, multipliers) * dt
 *
 * Runs a fixed simulation step (default 60 Hz) inside the variable render tick and writes an
 * interpolated transform to the owning actor. Gameplay state is the Sim* fields, not the actor transform.
 *
 * Shared by human and AI pawns. Input arrives only through SetInput().
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNeonMovementComponent();

	// ---- Configuration ----

	/** Tuning asset. If null, the class defaults of UNeonVehicleConfig are used. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Config")
	TObjectPtr<UNeonVehicleConfig> Config;

	/** Fixed simulation rate. Will move to a shared game config asset once one exists. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Simulation", meta = (ClampMin = "10", ClampMax = "240"))
	int32 SimulationHz = 60;

	/** Cap so a frame hitch cannot simulate unbounded time. Extra time is dropped. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Simulation", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxStepsPerFrame = 4;

	// ---- Driving API ----

	/**
	 * Latest input snapshot. Steering is applied on every simulation step until replaced. A rising edge
	 * of bActivatePowerup is latched and consumed by the next step (one activation attempt per press,
	 * whether the press lasted one frame or many).
	 */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetInput(const FNeonVehicleInput& InInput);

	/** Rectangle (world XY) the vehicle must stay inside. Set by the game mode from the arena. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetArenaBounds(const FBox2D& InBounds);

	/**
	 * Bounds plus the terrain height source. On the ground Z sticks to the floor. A jump or a steep
	 * lip leaves the floor: SimZ and vertical velocity integrate with gravity, trails are not laid
	 * and trails are not lethal until landing (DECISIONS.md D29).
	 */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetArena(const ANeonArena* InArena);

	/** Tilt the actor to the floor normal while grounded. In the air, pitch follows vertical speed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Simulation")
	bool bAlignToFloor = true;

	/** Teleport the simulation to a position/heading, clear history, and mark alive. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void ResetSimulation(const FVector& WorldLocation, float HeadingDegrees);

	/** Powerup hook (Boost / Overdrive / EMP). 1,1 = no modification. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetSpeedModifiers(float InSpeedMultiplier, float InTurnRateMultiplier);

	/** Player slot; doubles as the trail owner id. Set by the game mode. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetPlayerSlot(int32 InSlot);

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	int32 GetPlayerSlot() const { return PlayerSlot; }

	/** Eliminate from outside the step (the other party in a vehicle collision; later Shield/Phase logic lives here too). */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void ForceEliminate(ENeonEliminationCause Cause);

	/**
	 * Frozen vehicles do not step (countdown, results screens, menu). Input is still accepted and applied
	 * on the first step after unfreezing. The round system owns this flag.
	 */
	UFUNCTION(BlueprintCallable, Category = "Neon|Movement")
	void SetFrozen(bool bInFrozen);

	/** Client mirror of the server's alive flag. Does not decide the death. */
	void MirrorAlive(bool bInAlive);

	/**
	 * Owning client: the host's bike at SimTick. Replay the inputs the host has not simulated yet
	 * on top of that pose so steering stays immediate.
	 */
	void ReconcilePredictedPose(FVector Location, float Yaw, int32 ServerTick);

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool IsFrozen() const { return bFrozen; }

	// ---- Queries ----

	UNeonPowerupComponent* GetPowerups() const { return Powerups; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetHeading() const { return SimHeading; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	FVector2D GetSimPosition() const { return SimPosition; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetCurrentSpeed() const { return CurrentSpeed; }

	/** 0..1. Full bar to start a burst; during the burst this is the time left. Then a slow refill. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetSprintMeter() const;

	/** 0..1. One jump spends JumpCost (the whole bar). Refills slowly on the ground and in the air. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetJumpMeter() const { return JumpMeter; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool IsSprinting() const { return bSprintingApplied; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool IsAirborne() const { return bAirborne; }

	/** Gameplay Z of the vehicle center. Vehicle hits ignore pairs farther apart than AirCollisionHeight. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetSimZ() const { return SimZ; }

	/** Which stacked deck the bike is on. 0 is the ground. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	int32 GetSimLayer() const { return SimLayer; }

	/**
	 * Phase activation. Leaves the floor this step, climbs with UpImpulse, and runs at SpeedMultiplier
	 * for Ticks so the bike lands ahead of the trails it jumped.
	 */
	void BeginPhaseJump(float UpImpulse, float InSpeedMultiplier, int32 Ticks);

	/** Grounded, alive, and the jump bar is full. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool CanJump() const;

	/** Alive, not already bursting, and the sprint bar is full. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool CanSprint() const;

	/** Steering actually applied on the last step (-1..1). Lean reads this. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetAppliedSteering() const { return AppliedSteering; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	bool IsAlive() const { return bAlive; }

	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	int32 GetSimulationTick() const { return SimulationTick; }

	/** Radius in cm the vehicle would trace at full steer at the current speed. For tuning HUD / logs. */
	UFUNCTION(BlueprintPure, Category = "Neon|Movement")
	float GetCurrentTurnRadius() const;

	/** Assigned Config, or the class defaults of UNeonVehicleConfig when none is assigned. */
	const UNeonVehicleConfig* GetConfig() const;

	bool HasArenaBounds() const { return bHasArenaBounds; }
	/** Rectangle the vehicle must stay inside right now. Live from the arena when one is set (it shrinks during the collapse, D27). */
	FBox2D GetArenaBounds() const;
	/** Rectangle SecondsAhead from now, for AI prediction. */
	FBox2D GetArenaBoundsAt(float SecondsAhead) const;

	UPROPERTY(BlueprintAssignable, Category = "Neon|Movement")
	FNeonVehicleEliminatedSignature OnEliminated;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void StepSimulation(float Dt);
	void RecordCosmeticTrail();
	void UpdateMeters(float Dt);
	/** Returns true if the move From->To is lethal. Trail hits use Z, so a jump clears a lower laser. */
	bool CheckHazards(const FVector2D& From, const FVector2D& To, float FromZ, float ToZ, ENeonEliminationCause& OutCause);
	float ComputeTurnRateDegrees(float Speed) const;
	void ApplyInterpolatedTransform(float Alpha);
	void Eliminate(ENeonEliminationCause Cause);
	void RememberPredictedStep();
	void EnsurePredictedArena();
	bool IsPredictedLocal() const;
	bool IsMatchPlaying() const;

	struct FNeonPredictedStep
	{
		int32 Tick = 0;
		float Steering = 0.f;
		float Speed = 0.f;
		float TurnRateDeg = 0.f;
	};

	TArray<FNeonPredictedStep> PredictedSteps;
	bool bPredictionSnapped = false;

	FNeonVehicleInput PendingInput;

	FVector2D SimPosition = FVector2D::ZeroVector;
	FVector2D PrevSimPosition = FVector2D::ZeroVector;
	float SimHeading = 0.f;
	float PrevSimHeading = 0.f;
	/** Actor Z when there is no terrain source. */
	float FloorZ = 0.f;
	/** Actor Z above the terrain surface when there is one (the arena's VehicleSpawnHeight). */
	float FloorOffset = 0.f;

	/** Gameplay height of the vehicle center. Glued to the floor while grounded. */
	float SimZ = 0.f;
	float PrevSimZ = 0.f;
	float VerticalVelocity = 0.f;
	/** Phase hop: extra speed for a few ticks after leaving the floor. */
	float PhaseBoostMultiplier = 1.f;
	int32 PhaseBoostTicks = 0;
	/** Stacked deck the bike is riding. Jumps keep this until landing. */
	int32 SimLayer = 0;
	/** Highest upward speed built on the current climb. Launches off a lip if it clears the threshold. */
	float PeakClimbVz = 0.f;
	float TimeSinceClimb = 1.f;
	bool bAirborne = false;

	/** 0..1 meters. Shared by humans and AI; the HUD reads them. */
	float SprintMeter = 1.f;
	float JumpMeter = 1.f;
	/** Seconds left in a committed sprint burst. The bar is emptied when the burst starts. */
	float SprintTimeLeft = 0.f;
	bool bSprintingApplied = false;
	bool bSprintHeld = false;

	/** Terrain height source. Outlives every vehicle; weak so a destroyed arena cannot dangle. */
	TWeakObjectPtr<const ANeonArena> HeightSource;

	float CurrentSpeed = 0.f;
	float AppliedSteering = 0.f;
	bool bAlive = true;
	bool bHasCosmeticPoint = false;
	FVector2D LastCosmeticPoint = FVector2D::ZeroVector;
	bool bFrozen = false;
	int32 SimulationTick = 0;

	float SpeedMultiplier = 1.f;
	float TurnRateMultiplier = 1.f;

	/** Set by SetInput on a rising activate edge; consumed by the next step. */
	bool bActivateLatched = false;
	bool bLastActivateInput = false;

	/** Rising edge of bJump. Consumed on the next grounded step (buffered through the air). */
	bool bJumpLatched = false;
	bool bLastJumpInput = false;

	FBox2D ArenaBounds;
	bool bHasArenaBounds = false;

	float Accumulator = 0.f;

	int32 PlayerSlot = 0;

	UPROPERTY(Transient)
	TObjectPtr<UNeonCollisionSystem> Collision;

	UPROPERTY(Transient)
	TObjectPtr<UNeonTrailComponent> Trail;

	/** Optional. A vehicle without one simply never has powerups. */
	UPROPERTY(Transient)
	TObjectPtr<UNeonPowerupComponent> Powerups;
};
