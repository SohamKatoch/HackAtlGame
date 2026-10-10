#pragma once

#include "CoreMinimal.h"
#include "AI/NeonAIConfig.h"
#include "GameFramework/Controller.h"
#include "Vehicle/NeonVehicleInput.h"
#include "NeonAIController.generated.h"

class UNeonMovementComponent;
class UNeonCollisionSystem;

/**
 * Deterministic gameplay AI. Docs/AI_SPEC.md.
 *
 * Produces the same FNeonVehicleInput a human produces; the vehicle, trail, and collision rules are
 * untouched. A "plan" is: steer at a fixed value until the heading has changed by a target angle, then
 * go straight. Decision loop:
 *
 *   every DecisionInterval (jittered):
 *     for each plan (steering in {-1..1}, ArcsPerSide per side; turn angle from {90,45,135,30,180}; plus straight):
 *       predict the path for LookAheadTime with the vehicle's own turn-rate formula
 *       stop at the first lethal step: boundary, any trail (same own-trail grace as the real step),
 *         the predicted path's own earlier points, an alive vehicle now, or an alive vehicle extrapolated
 *         along its current steering for at most OpponentExtrapolationTime
 *       score = survive bonus + free fraction + openness/escape at path end + flood-fill territory
 *               + cut-off alignment + pickup alignment (slot empty) - steering cost - loop aversion + commit bonus
 *     if nothing survives, also try full-steer 180 and 135 both ways
 *     pick the best (or, with MistakeChance, a random survivor)
 *     if holding a powerup, decide whether this is the moment to use it (PowerupSkill): defensive ones
 *       when the chosen plan does not survive or this is an Avoid interrupt, speed ones on an open
 *       straight, EMP when an opponent is in range; the input carries a one-frame activate pulse
 *   every frame: execute the committed plan (turning phase -> straight phase); if the plan has become
 *     lethal and ReactionTime has passed, decide again (Avoid interrupt)
 *
 * Derives from AController rather than AAIController: no navmesh, perception, or behavior tree is
 * needed, and avoiding AIModule keeps the AI a plain input generator. Randomness is a seeded
 * FRandomStream so a given seed replays identically. Runs only where the simulation is authoritative.
 */
UCLASS()
class NEONGRID_API ANeonAIController : public AController
{
	GENERATED_BODY()

public:
	ANeonAIController();

	/** Difficulty asset. Null uses UNeonAIConfig class defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|AI")
	TObjectPtr<UNeonAIConfig> Config;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|AI")
	ENeonAIDifficulty Difficulty = ENeonAIDifficulty::Normal;

	/** Set config, difficulty, and the random seed. Call before or after Possess. */
	UFUNCTION(BlueprintCallable, Category = "Neon|AI")
	void ConfigureAI(UNeonAIConfig* InConfig, ENeonAIDifficulty InDifficulty, int32 Seed);

	/** Forget the committed plan. The game mode calls this when it resets a round. */
	UFUNCTION(BlueprintCallable, Category = "Neon|AI")
	void ResetBrain();

	UFUNCTION(BlueprintPure, Category = "Neon|AI")
	FName GetStateName() const;

	const FNeonVehicleInput& GetCurrentInput() const { return CurrentInput; }

	virtual void Tick(float DeltaTime) override;

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void PostInitializeComponents() override;
	virtual void InitPlayerState() override;

	enum class EState : uint8 { Cruise, Turn, Avoid, Chase, Doomed };

	/** Steer at Steering until the heading has moved TurnAngle degrees from where the plan started, then straight. */
	struct FPlan
	{
		float Steering = 0.f;
		float TurnAngle = 0.f;
		float StartHeading = 0.f;
	};

	/** Points visited by a predicted path, used as obstacles for later prediction (the trail it would lay). */
	using FPathPoints = TArray<FVector2D, TInlineAllocator<48>>;

	struct FPathResult
	{
		int32 Survived = 0;
		int32 Steps = 1;
		FVector2D EndPos = FVector2D::ZeroVector;
		float EndHeading = 0.f;
		float FreeFraction() const { return static_cast<float>(Survived) / static_cast<float>(FMath::Max(Steps, 1)); }
		bool Survives() const { return Survived == Steps; }
	};

	/**
	 * Coarse occupancy grid of the arena (trails + vehicles) rebuilt once per decision, with a flood fill
	 * that counts cells reachable from a point. Reachable fraction is the "territory" of a plan.
	 */
	struct FSpaceGrid
	{
		FBox2D Bounds;
		float CellSize = 250.f;
		int32 Width = 0;
		int32 Height = 0;
		TBitArray<> Occupied;

		void Build(const UNeonCollisionSystem& Collision, const FBox2D& InBounds, float InCellSize);
		bool CellOf(const FVector2D& P, int32& OutX, int32& OutY) const;
		void MarkSegment(const FVector2D& A, const FVector2D& B);
		/** Reachable cells from Start, treating Extra points as occupied too. 0..1 of all cells. */
		float ReachableFraction(const FVector2D& Start, const TArrayView<const FVector2D>& Extra) const;
	};

	struct FPlanSample
	{
		FPlan Plan;
		float FreeFraction = 0.f;
		bool bSurvives = false;
		/** 0..1: straight probes from the plan end. */
		float Openness = 0.f;
		/** 0..1: best of a full-steer 90-degree turn left or right from the plan end. */
		float Escape = 0.f;
		/** 0..1: arena fraction reachable from the plan end. */
		float Space = 0.f;
		float Chase = 0.f;
		/** 0..1: alignment of the plan end with the nearest pickup (0 when none in range or slot full). */
		float Collect = 0.f;
		float Score = -TNumericLimits<float>::Max();
		FVector2D EndPos = FVector2D::ZeroVector;
		float EndHeading = 0.f;
	};

	const FNeonAIDifficultyParams& Params() const;

	void Decide(bool bEmergency);

	/**
	 * Predict steering at Steering until the heading has changed by TurnAngle, then straight, for Seconds in Steps.
	 * TimeOffset is how far in the future the path starts (for opponent extrapolation). InOutPath receives
	 * the visited points and is also treated as trail already laid (pass the plan path when predicting an escape).
	 */
	FPathResult PredictPath(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P,
		const FVector2D& StartPos, float StartHeading, float Steering, float TurnAngle,
		float Seconds, int32 Steps, float TimeOffset, FPathPoints& InOutPath) const;

	/** Predict a plan from the vehicle's current state and fill the survival fields. TurnRemaining is how much turn is still owed. */
	void EvaluatePlan(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, float Steering, float TurnRemaining, FPlanSample& Out, FPathPoints& OutPath) const;
	float ProbeOpenness(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FVector2D& From, float HeadingDeg) const;
	float ProbeEscape(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Sample, const FPathPoints& PlanPath) const;
	float ScoreChase(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Sample) const;
	float ScoreCollect(const FNeonAIDifficultyParams& P, const FPlanSample& Sample, const FVector2D* PickupPos) const;
	/** Nearest pickup within P.CollectRange of the vehicle, if the slot is empty and pickups exist. */
	bool FindNearestPickup(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, FVector2D& OutPos) const;
	/** Decide whether to pulse bActivatePowerup this decision. See class comment. */
	bool ShouldActivatePowerup(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Pick, bool bEmergency, int32 SurvivorCount);

	/** Lethal test for one predicted step at TimeAhead seconds in the future. See class comment. */
	bool IsStepLethal(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, float Radius, const FVector2D& From, const FVector2D& To, float TimeAhead) const;

	/** Degrees of the committed turn still to perform (0 when in the straight phase). */
	float CommittedTurnRemaining() const;
	/** Steering to apply right now for the committed plan. */
	float ExecutePlan() const;

	const UNeonMovementComponent* FindNearestOpponent(const UNeonMovementComponent& M) const;
	float FullSteerTurnRateDeg(const UNeonMovementComponent& M, float Speed) const;
	float PredictedSpeed(const UNeonMovementComponent& M) const;
	static FVector2D DirectionFromHeading(float HeadingDeg);

	UPROPERTY(Transient)
	TObjectPtr<UNeonMovementComponent> Movement;

	UPROPERTY(Transient)
	TObjectPtr<UNeonCollisionSystem> Collision;

	FRandomStream Rng;
	FNeonVehicleInput CurrentInput;
	FPlan Committed;
	EState State = EState::Cruise;

	float TimeToNextDecision = 0.f;
	float TimeSinceEmergency = 0.f;
	bool bWasAlive = false;

	/** Decayed sum of signed heading change (degrees). Positive = has been turning left for a while. */
	float RecentTurn = 0.f;
	float LastHeading = 0.f;

	FSpaceGrid SpaceGrid;
};
