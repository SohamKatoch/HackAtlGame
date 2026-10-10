#include "AI/NeonAIController.h"

#include "EngineUtils.h"
#include "GameFramework/PlayerState.h"
#include "NeonGrid.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Powerups/NeonPowerupConfig.h"
#include "Powerups/NeonPowerupPickup.h"
#include "Trail/NeonCollisionSystem.h"
#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehicleConfig.h"
#include "Vehicle/NeonVehiclePawn.h"

namespace
{
	// Turn angles considered, in priority order. A level with TurnAnglesPerArc = N uses the first N.
	constexpr float GTurnAngles[5] = { 90.f, 45.f, 135.f, 30.f, 180.f };
}

ANeonAIController::ANeonAIController()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bAttachToPawn = false;
	Rng.Initialize(1);
}

void ANeonAIController::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Scores and the HUD treat AI like any player: give it an ANeonPlayerState (what AAIController does
	// with bWantsPlayerState; AController itself never creates one).
	if (IsValidChecked(this) && GetNetMode() != NM_Client)
	{
		InitPlayerState();
	}
}

void ANeonAIController::InitPlayerState()
{
	Super::InitPlayerState();
	if (PlayerState)
	{
		PlayerState->SetIsABot(true);
	}
}

void ANeonAIController::ConfigureAI(UNeonAIConfig* InConfig, ENeonAIDifficulty InDifficulty, int32 Seed)
{
	Config = InConfig;
	Difficulty = InDifficulty;
	Rng.Initialize(Seed);
	ResetBrain();
}

void ANeonAIController::ResetBrain()
{
	CurrentInput = FNeonVehicleInput();
	Committed = FPlan();
	RecentTurn = 0.f;
	if (Movement)
	{
		Committed.StartHeading = Movement->GetHeading();
		LastHeading = Movement->GetHeading();
		Movement->SetInput(CurrentInput);
	}
	State = EState::Cruise;
	TimeToNextDecision = 0.f;
	TimeSinceEmergency = Params().ReactionTime;
}

FName ANeonAIController::GetStateName() const
{
	switch (State)
	{
	case EState::Cruise: return TEXT("Cruise");
	case EState::Turn:   return TEXT("Turn");
	case EState::Avoid:  return TEXT("Avoid");
	case EState::Chase:  return TEXT("Chase");
	case EState::Doomed: return TEXT("Doomed");
	}
	return NAME_None;
}

const FNeonAIDifficultyParams& ANeonAIController::Params() const
{
	const UNeonAIConfig* C = Config ? Config.Get() : GetDefault<UNeonAIConfig>();
	return C->GetParams(Difficulty);
}

void ANeonAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	Movement = nullptr;
	if (const ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(InPawn))
	{
		Movement = Vehicle->GetMovement();
	}
	else if (InPawn)
	{
		Movement = InPawn->FindComponentByClass<UNeonMovementComponent>();
	}

	if (UWorld* World = GetWorld())
	{
		Collision = World->GetSubsystem<UNeonCollisionSystem>();
	}

	bWasAlive = Movement && Movement->IsAlive();
	ResetBrain();
}

void ANeonAIController::OnUnPossess()
{
	Movement = nullptr;
	Super::OnUnPossess();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void ANeonAIController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!Movement)
	{
		return;
	}

	const bool bAlive = Movement->IsAlive();
	if (!bAlive)
	{
		if (bWasAlive)
		{
			CurrentInput = FNeonVehicleInput();
			Movement->SetInput(CurrentInput);
		}
		bWasAlive = false;
		return;
	}

	if (!bWasAlive)
	{
		// Respawned. Think immediately.
		bWasAlive = true;
		ResetBrain();
	}

	if (Movement->IsFrozen())
	{
		// Countdown / results: nothing moves, so do not spend decisions (keeps the RNG sequence tied to play time).
		LastHeading = Movement->GetHeading();
		return;
	}

	const FNeonAIDifficultyParams& P = Params();
	TimeToNextDecision -= DeltaTime;
	TimeSinceEmergency += DeltaTime;

	// Heading-change accumulator with a ~3 s memory, for loop aversion.
	{
		const float Heading = Movement->GetHeading();
		const float Delta = FRotator::NormalizeAxis(Heading - LastHeading);
		LastHeading = Heading;
		RecentTurn = RecentTurn * FMath::Exp(-DeltaTime / 3.f) + Delta;
	}

	// The activate request is a one-frame pulse; the movement component latches its rising edge.
	CurrentInput.bActivatePowerup = false;

	bool bDecide = TimeToNextDecision <= 0.f;
	bool bEmergency = false;

	// Avoid interrupt: the committed plan may have become lethal since the last decision
	// (another vehicle's trail grew across it). Checked every frame, acted on at the reaction rate.
	if (!bDecide && TimeSinceEmergency >= P.ReactionTime)
	{
		FPlanSample Current;
		FPathPoints Path;
		EvaluatePlan(*Movement, P, Committed.Steering, CommittedTurnRemaining(), Current, Path);
		if (!Current.bSurvives)
		{
			bDecide = true;
			bEmergency = true;
		}
	}

	if (bDecide)
	{
		Decide(bEmergency);
		TimeToNextDecision = P.DecisionInterval * Rng.FRandRange(0.8f, 1.2f);
		if (bEmergency)
		{
			TimeSinceEmergency = 0.f;
		}
	}

	CurrentInput.Steering = ExecutePlan();
	Movement->SetInput(CurrentInput);
}

float ANeonAIController::CommittedTurnRemaining() const
{
	if (!Movement || Committed.Steering == 0.f || Committed.TurnAngle <= 0.f)
	{
		return 0.f;
	}

	// Signed heading change since the plan started, measured in the turn direction.
	const float Turned = FRotator::NormalizeAxis(Movement->GetHeading() - Committed.StartHeading) * FMath::Sign(Committed.Steering);
	return FMath::Max(Committed.TurnAngle - Turned, 0.f);
}

float ANeonAIController::ExecutePlan() const
{
	return CommittedTurnRemaining() > 0.f ? Committed.Steering : 0.f;
}

// ---------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------

void ANeonAIController::Decide(bool bEmergency)
{
	const FNeonAIDifficultyParams& P = Params();
	const UNeonMovementComponent& M = *Movement;

	const int32 PerSide = FMath::Clamp(P.ArcsPerSide, 1, 8);
	const int32 AngleCount = FMath::Clamp(P.TurnAnglesPerArc, 1, 5);

	TArray<FPlanSample, TInlineAllocator<81>> Samples;

	const bool bUseSpace = P.SpaceWeight > 0.f && Collision && M.HasArenaBounds();
	if (bUseSpace)
	{
		SpaceGrid.Build(*Collision, M.GetArenaBounds(), P.SpaceCellSize);
	}

	// Loop aversion: how much the vehicle has already been turning, as a 0..1 factor past 90 degrees.
	const float LoopSign = FMath::Sign(RecentTurn);
	const float LoopFactor = FMath::Clamp((FMath::Abs(RecentTurn) - 90.f) / 180.f, 0.f, 1.f);

	// Pickup worth a detour, if any (slot must be empty).
	FVector2D PickupPos;
	const bool bWantPickup = FindNearestPickup(M, P, PickupPos);

	// Steering, TurnAngle: the plan. StartHeading: where its turn is measured from. bContinuation: it is the
	// committed plan carried on (gets the hysteresis bonus; nothing else does).
	auto AddSample = [&](float Steering, float TurnAngle, float StartHeading, bool bContinuation)
	{
		FPlanSample& S = Samples.AddDefaulted_GetRef();
		S.Plan.Steering = Steering;
		S.Plan.TurnAngle = TurnAngle;
		S.Plan.StartHeading = StartHeading;

		// Turn still owed from the current heading.
		const float Remaining = (Steering != 0.f)
			? FMath::Max(TurnAngle - FRotator::NormalizeAxis(M.GetHeading() - StartHeading) * FMath::Sign(Steering), 0.f)
			: 0.f;

		FPathPoints Path;
		EvaluatePlan(M, P, Steering, Remaining, S, Path);

		S.Openness = ProbeOpenness(M, P, S.EndPos, S.EndHeading);
		S.Escape = ProbeEscape(M, P, S, Path);
		if (bUseSpace)
		{
			S.Space = SpaceGrid.ReachableFraction(S.EndPos, TArrayView<const FVector2D>(Path.GetData(), Path.Num()));
		}
		if (S.bSurvives)
		{
			S.Chase = ScoreChase(M, P, S);
			S.Collect = ScoreCollect(P, S, bWantPickup ? &PickupPos : nullptr);
		}

		const float Sign = FMath::Sign(Steering);
		const float Loop = (Sign != 0.f && Sign == LoopSign) ? P.LoopAversion * LoopFactor * (Remaining / 90.f) : 0.f;

		// A plan that survives but leaves no way to turn afterwards is a trap; Escape carries that.
		// Space is the territory the plan keeps; it is what prevents self-sealing.
		S.Score =
			(S.bSurvives ? 2.f : 0.f)
			+ 2.f * S.FreeFraction
			+ P.OpennessWeight * (0.5f * S.Openness + 0.5f * S.Escape)
			+ P.SpaceWeight * S.Space
			+ P.ChaseWeight * S.Chase
			+ P.CollectWeight * S.Collect
			- P.StraightBias * FMath::Abs(Steering)
			- Loop
			+ (bContinuation ? P.CommitBias : 0.f);
	};

	// Continuation of the committed plan first (straight counts as its own continuation).
	if (Committed.Steering != 0.f && CommittedTurnRemaining() > 0.f)
	{
		AddSample(Committed.Steering, Committed.TurnAngle, Committed.StartHeading, true);
		AddSample(0.f, 0.f, M.GetHeading(), false);
	}
	else
	{
		AddSample(0.f, 0.f, M.GetHeading(), true);
	}

	for (int32 i = 1; i <= PerSide; ++i)
	{
		const float Magnitude = static_cast<float>(i) / static_cast<float>(PerSide);
		for (int32 a = 0; a < AngleCount; ++a)
		{
			AddSample(+Magnitude, GTurnAngles[a], M.GetHeading(), false);
			AddSample(-Magnitude, GTurnAngles[a], M.GetHeading(), false);
		}
	}

	// Panic set: if nothing above survives, every level may try the full-steer U-turn (180) and the
	// tight 135 both ways. These are the plans a cornered human reaches for; a level that normally
	// skips them should still not drive into a wall it could have turned away from.
	auto AnySurvives = [&]()
	{
		for (const FPlanSample& S : Samples)
		{
			if (S.bSurvives)
			{
				return true;
			}
		}
		return false;
	};
	if (!AnySurvives())
	{
		for (const float Angle : { 180.f, 135.f })
		{
			AddSample(+1.f, Angle, M.GetHeading(), false);
			AddSample(-1.f, Angle, M.GetHeading(), false);
		}
	}

	// Best plan, and the list of survivors for mistakes.
	int32 BestIndex = 0;
	TArray<int32, TInlineAllocator<81>> Survivors;
	for (int32 i = 0; i < Samples.Num(); ++i)
	{
		if (Samples[i].Score > Samples[BestIndex].Score)
		{
			BestIndex = i;
		}
		if (Samples[i].bSurvives)
		{
			Survivors.Add(i);
		}
	}

	int32 Chosen = BestIndex;
	if (!bEmergency && Survivors.Num() >= 2 && P.MistakeChance > 0.f && Rng.FRand() < P.MistakeChance)
	{
		Chosen = Survivors[Rng.RandRange(0, Survivors.Num() - 1)];
	}

	const FPlanSample& Pick = Samples[Chosen];
	Committed = Pick.Plan;
	CurrentInput.bActivatePowerup = ShouldActivatePowerup(M, P, Pick, bEmergency, Survivors.Num());

	// State label (logging / later HUD). The decision itself is the scoring above.
	EState NewState;
	if (Survivors.Num() == 0)
	{
		NewState = EState::Doomed;
	}
	else if (bEmergency)
	{
		NewState = EState::Avoid;
	}
	else if (P.ChaseWeight > 0.f && Pick.Chase > 0.75f && Pick.Plan.Steering != 0.f)
	{
		NewState = EState::Chase;
	}
	else if (Pick.Plan.Steering != 0.f)
	{
		NewState = EState::Turn;
	}
	else
	{
		NewState = EState::Cruise;
	}

	const bool bChanged = NewState != State;
	State = NewState;

#if !NO_LOGGING
	const FString Line = FString::Printf(
		TEXT("%s AI slot %d -> %s (steer %.2f for %.0f deg, %d/%d plans survive, free %.2f open %.2f esc %.2f space %.2f) at (%.0f, %.0f) hdg %.0f tick %d"),
		*GetNameSafe(GetPawn()), M.GetPlayerSlot(), *GetStateName().ToString(), Pick.Plan.Steering, Pick.Plan.TurnAngle,
		Survivors.Num(), Samples.Num(), Pick.FreeFraction, Pick.Openness, Pick.Escape, Pick.Space,
		M.GetSimPosition().X, M.GetSimPosition().Y, M.GetHeading(), M.GetSimulationTick());
	if (bChanged)
	{
		UE_LOG(LogNeonGrid, Verbose, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogNeonGrid, VeryVerbose, TEXT("%s"), *Line);
	}
#endif
}

// ---------------------------------------------------------------------------
// Space grid
// ---------------------------------------------------------------------------

void ANeonAIController::FSpaceGrid::Build(const UNeonCollisionSystem& Collision, const FBox2D& InBounds, float InCellSize)
{
	Bounds = InBounds;
	CellSize = FMath::Max(InCellSize, 50.f);
	const FVector2D Size = Bounds.Max - Bounds.Min;
	Width = FMath::Max(1, FMath::CeilToInt(Size.X / CellSize));
	Height = FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize));
	Occupied.Init(false, Width * Height);

	for (const FNeonTrailSegment& Seg : Collision.GetSegments())
	{
		if (!Seg.bRemoved)
		{
			MarkSegment(Seg.Start, Seg.End);
		}
	}

	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision.GetVehicles())
	{
		if (const UNeonMovementComponent* V = Weak.Get(); V && V->IsAlive())
		{
			int32 X, Y;
			if (CellOf(V->GetSimPosition(), X, Y))
			{
				Occupied[Y * Width + X] = true;
			}
		}
	}

	// Dilate by one cell so gaps narrower than roughly a cell do not count as passages. The flood
	// fill is topological; the vehicle (700 cm full-steer radius) cannot thread a 250 cm gap.
	const TBitArray<> Source = Occupied;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			if (!Source[Y * Width + X])
			{
				continue;
			}
			if (X > 0) { Occupied[Y * Width + X - 1] = true; }
			if (X + 1 < Width) { Occupied[Y * Width + X + 1] = true; }
			if (Y > 0) { Occupied[(Y - 1) * Width + X] = true; }
			if (Y + 1 < Height) { Occupied[(Y + 1) * Width + X] = true; }
		}
	}
}

bool ANeonAIController::FSpaceGrid::CellOf(const FVector2D& P, int32& OutX, int32& OutY) const
{
	OutX = FMath::FloorToInt((P.X - Bounds.Min.X) / CellSize);
	OutY = FMath::FloorToInt((P.Y - Bounds.Min.Y) / CellSize);
	return OutX >= 0 && OutX < Width && OutY >= 0 && OutY < Height;
}

void ANeonAIController::FSpaceGrid::MarkSegment(const FVector2D& A, const FVector2D& B)
{
	const float Length = (B - A).Size();
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / (CellSize * 0.5f)));
	for (int32 i = 0; i <= Steps; ++i)
	{
		const FVector2D P = FMath::Lerp(A, B, static_cast<float>(i) / Steps);
		int32 X, Y;
		if (CellOf(P, X, Y))
		{
			Occupied[Y * Width + X] = true;
		}
	}
}

float ANeonAIController::FSpaceGrid::ReachableFraction(const FVector2D& Start, const TArrayView<const FVector2D>& Extra) const
{
	const int32 Total = Width * Height;
	if (Total <= 0)
	{
		return 0.f;
	}

	TBitArray<> Visited = Occupied;
	for (const FVector2D& E : Extra)
	{
		int32 X, Y;
		if (CellOf(E, X, Y))
		{
			Visited[Y * Width + X] = true;
		}
	}

	int32 SX, SY;
	if (!CellOf(Start, SX, SY))
	{
		return 0.f;
	}
	// The start cell may be marked by the path itself; fill from it regardless.
	Visited[SY * Width + SX] = true;

	TArray<int32, TInlineAllocator<1024>> Queue;
	Queue.Add(SY * Width + SX);
	int32 Count = 1;
	int32 Head = 0;

	while (Head < Queue.Num())
	{
		const int32 Cell = Queue[Head++];
		const int32 CX = Cell % Width;
		const int32 CY = Cell / Width;

		const int32 Neighbors[4][2] = { { CX + 1, CY }, { CX - 1, CY }, { CX, CY + 1 }, { CX, CY - 1 } };
		for (const auto& N : Neighbors)
		{
			if (N[0] < 0 || N[0] >= Width || N[1] < 0 || N[1] >= Height)
			{
				continue;
			}
			const int32 Index = N[1] * Width + N[0];
			if (Visited[Index])
			{
				continue;
			}
			Visited[Index] = true;
			Queue.Add(Index);
			++Count;
		}
	}

	return static_cast<float>(Count) / static_cast<float>(Total);
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------

FVector2D ANeonAIController::DirectionFromHeading(float HeadingDeg)
{
	const float Rad = FMath::DegreesToRadians(HeadingDeg);
	return FVector2D(FMath::Cos(Rad), FMath::Sin(Rad));
}

float ANeonAIController::PredictedSpeed(const UNeonMovementComponent& M) const
{
	const float Speed = M.GetCurrentSpeed();
	return Speed > KINDA_SMALL_NUMBER ? Speed : M.GetConfig()->BaseSpeed;
}

float ANeonAIController::FullSteerTurnRateDeg(const UNeonMovementComponent& M, float Speed) const
{
	// Same clamp as UNeonMovementComponent::ComputeTurnRateDegrees without powerup multipliers.
	const UNeonVehicleConfig* C = M.GetConfig();
	float Rate = C->MaxTurnRate;
	if (C->MinTurnRadius > KINDA_SMALL_NUMBER)
	{
		Rate = FMath::Min(Rate, FMath::RadiansToDegrees(Speed / C->MinTurnRadius));
	}
	return Rate;
}

bool ANeonAIController::IsStepLethal(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, float Radius, const FVector2D& From, const FVector2D& To, float TimeAhead) const
{
	// Boundary.
	if (M.HasArenaBounds())
	{
		const FBox2D& B = M.GetArenaBounds();
		if (To.X < B.Min.X + Radius || To.X > B.Max.X - Radius ||
			To.Y < B.Min.Y + Radius || To.Y > B.Max.Y - Radius)
		{
			return true;
		}
	}

	if (!Collision)
	{
		return false;
	}

	// Trails, with the same own-trail grace the real step uses.
	FNeonTrailHit Hit;
	const int32 SelfIgnoreAfterTick = M.GetSimulationTick() - M.GetConfig()->SelfTrailGraceTicks;
	if (Collision->SweepAgainstTrails(From, To, Radius, M.GetPlayerSlot(), SelfIgnoreAfterTick, Hit))
	{
		return true;
	}

	// Vehicles: the body where it is now, and the trail it will have laid by TimeAhead if it holds its
	// heading (capped). The future path is treated as a trail segment, since that is what it becomes.
	const float Extrapolate = FMath::Min(TimeAhead, P.OpponentExtrapolationTime);
	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
	{
		const UNeonMovementComponent* Other = Weak.Get();
		if (!Other || Other == &M || !Other->IsAlive())
		{
			continue;
		}

		const float Combined = Radius + Other->GetConfig()->CollisionRadius;
		const FVector2D Now = Other->GetSimPosition();
		if ((Now - To).SizeSquared() <= Combined * Combined)
		{
			return true;
		}

		if (Extrapolate > 0.f)
		{
			// Constant-curvature extrapolation: hold the opponent's current steering. A straight line misses
			// a turning opponent entirely; this is what it will actually lay down for the next fraction of a second.
			const float OtherSpeed = Other->GetCurrentSpeed();
			const float OtherSteer = Other->GetAppliedSteering();
			if (FMath::Abs(OtherSteer) < 0.05f)
			{
				const FVector2D Future = Now + DirectionFromHeading(Other->GetHeading()) * OtherSpeed * Extrapolate;
				if (UNeonCollisionSystem::SegmentSegmentDistanceSquared(From, To, Now, Future) <= Radius * Radius)
				{
					return true;
				}
			}
			else
			{
				constexpr int32 ArcSteps = 4;
				const float Dt = Extrapolate / ArcSteps;
				const float RateDeg = FullSteerTurnRateDeg(*Other, OtherSpeed) * OtherSteer;
				FVector2D Prev = Now;
				float Heading = Other->GetHeading();
				for (int32 i = 0; i < ArcSteps; ++i)
				{
					Heading += RateDeg * Dt;
					const FVector2D Next = Prev + DirectionFromHeading(Heading) * OtherSpeed * Dt;
					if (UNeonCollisionSystem::SegmentSegmentDistanceSquared(From, To, Prev, Next) <= Radius * Radius)
					{
						return true;
					}
					Prev = Next;
				}
			}
		}
	}

	return false;
}

ANeonAIController::FPathResult ANeonAIController::PredictPath(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P,
	const FVector2D& StartPos, float StartHeading, float Steering, float TurnAngle,
	float Seconds, int32 Steps, float TimeOffset, FPathPoints& InOutPath) const
{
	const UNeonVehicleConfig* C = M.GetConfig();
	Steps = FMath::Clamp(Steps, 1, 32);
	const float Dt = Seconds / static_cast<float>(Steps);
	const float Speed = PredictedSpeed(M);
	const float Radius = C->CollisionRadius + P.SafetyMargin;
	const float RadiusSq = Radius * Radius;
	const float FullRate = FullSteerTurnRateDeg(M, Speed);

	// Path points at least this far back (in distance) count as trail already laid. Mirrors the sim's
	// rear offset plus grace window.
	const float GraceSeconds = static_cast<float>(C->SelfTrailGraceTicks) / static_cast<float>(FMath::Max(M.SimulationHz, 1));
	const float SelfGapDistance = C->TrailRearOffset + GraceSeconds * Speed + Radius;

	FPathResult R;
	R.Steps = Steps;

	FVector2D Pos = StartPos;
	float Heading = StartHeading;
	float Turned = 0.f;
	float Travelled = 0.f;

	// Cumulative distance along the whole path for each stored point, for the self gap test.
	TArray<float, TInlineAllocator<48>> PathDistance;
	for (int32 k = 0; k < InOutPath.Num(); ++k)
	{
		if (k > 0)
		{
			Travelled += (InOutPath[k] - InOutPath[k - 1]).Size();
		}
		PathDistance.Add(Travelled);
	}
	if (InOutPath.Num() == 0)
	{
		InOutPath.Add(Pos);
		PathDistance.Add(0.f);
	}

	for (int32 i = 0; i < Steps; ++i)
	{
		float StepSteering = 0.f;
		if (Steering != 0.f && Turned < TurnAngle)
		{
			StepSteering = Steering;
			Turned += FMath::Abs(Steering) * FullRate * Dt;
		}
		Heading = FRotator::NormalizeAxis(Heading + StepSteering * FullRate * Dt);
		const FVector2D Next = Pos + DirectionFromHeading(Heading) * Speed * Dt;
		const float NextDistance = Travelled + Speed * Dt;

		if (IsStepLethal(M, P, Radius, Pos, Next, TimeOffset + (i + 1) * Dt))
		{
			break;
		}

		// The trail this path would have laid.
		bool bSelfHit = false;
		for (int32 j = 0; j < InOutPath.Num(); ++j)
		{
			if (NextDistance - PathDistance[j] < SelfGapDistance)
			{
				break; // remaining points are too recent to be trail yet
			}
			if ((InOutPath[j] - Next).SizeSquared() <= RadiusSq)
			{
				bSelfHit = true;
				break;
			}
		}
		if (bSelfHit)
		{
			break;
		}

		Pos = Next;
		Travelled = NextDistance;
		InOutPath.Add(Pos);
		PathDistance.Add(Travelled);
		++R.Survived;
	}

	R.EndPos = Pos;
	R.EndHeading = Heading;
	return R;
}

void ANeonAIController::EvaluatePlan(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, float Steering, float TurnRemaining, FPlanSample& Out, FPathPoints& OutPath) const
{
	OutPath.Reset();
	const FPathResult R = PredictPath(M, P, M.GetSimPosition(), M.GetHeading(), Steering, TurnRemaining,
		P.LookAheadTime, P.PredictionSteps, 0.f, OutPath);

	Out.FreeFraction = R.FreeFraction();
	Out.bSurvives = R.Survives();
	Out.EndPos = R.EndPos;
	Out.EndHeading = R.EndHeading;
}

float ANeonAIController::ProbeEscape(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Sample, const FPathPoints& PlanPath) const
{
	// From the plan's end, can the vehicle still make a full-steer 90-degree turn either way?
	const float Speed = PredictedSpeed(M);
	const float FullRate = FullSteerTurnRateDeg(M, Speed);
	const float Seconds = 90.f / FMath::Max(FullRate, 1.f);
	constexpr int32 Steps = 4;

	float Best = 0.f;
	for (float Steer : { -1.f, 1.f })
	{
		FPathPoints Path = PlanPath;
		const FPathResult R = PredictPath(M, P, Sample.EndPos, Sample.EndHeading, Steer, 90.f, Seconds, Steps, P.LookAheadTime, Path);
		Best = FMath::Max(Best, R.FreeFraction());
		if (Best >= 1.f)
		{
			break;
		}
	}
	return Best;
}

float ANeonAIController::ProbeOpenness(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FVector2D& From, float HeadingDeg) const
{
	// Five straight probes from the path end, weighted toward where the vehicle will be heading.
	const float Radius = M.GetConfig()->CollisionRadius + P.SafetyMargin;
	const float Length = P.OpennessProbeDistance;
	constexpr int32 ProbeSteps = 6;
	constexpr float Offsets[5] = { 0.f, -45.f, 45.f, -90.f, 90.f };
	constexpr float Weights[5] = { 2.f, 1.f, 1.f, 0.5f, 0.5f };

	float Total = 0.f;
	float WeightSum = 0.f;
	for (int32 k = 0; k < UE_ARRAY_COUNT(Offsets); ++k)
	{
		const FVector2D Dir = DirectionFromHeading(HeadingDeg + Offsets[k]);
		FVector2D Pos = From;
		int32 Survived = 0;
		for (int32 i = 0; i < ProbeSteps; ++i)
		{
			const FVector2D Next = Pos + Dir * (Length / ProbeSteps);
			if (IsStepLethal(M, P, Radius, Pos, Next, P.LookAheadTime))
			{
				break;
			}
			Pos = Next;
			++Survived;
		}
		Total += Weights[k] * static_cast<float>(Survived) / ProbeSteps;
		WeightSum += Weights[k];
	}
	return Total / WeightSum;
}

const UNeonMovementComponent* ANeonAIController::FindNearestOpponent(const UNeonMovementComponent& M) const
{
	if (!Collision)
	{
		return nullptr;
	}

	const UNeonMovementComponent* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
	{
		const UNeonMovementComponent* Other = Weak.Get();
		if (!Other || Other == &M || !Other->IsAlive())
		{
			continue;
		}
		const float DistSq = (Other->GetSimPosition() - M.GetSimPosition()).SizeSquared();
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Other;
		}
	}
	return Best;
}

float ANeonAIController::ScoreChase(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Sample) const
{
	if (P.ChaseWeight <= 0.f)
	{
		return 0.f;
	}

	const UNeonMovementComponent* Target = FindNearestOpponent(M);
	if (!Target)
	{
		return 0.f;
	}

	// Aim at the point the opponent will reach after the lead time: crossing it first leaves a trail in their way.
	const FVector2D CutOff = Target->GetSimPosition() + DirectionFromHeading(Target->GetHeading()) * Target->GetCurrentSpeed() * P.CutOffLeadTime;
	FVector2D ToCutOff = CutOff - Sample.EndPos;
	const float Dist = ToCutOff.Size();
	if (Dist <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}
	ToCutOff /= Dist;

	const float Alignment = FVector2D::DotProduct(DirectionFromHeading(Sample.EndHeading), ToCutOff); // -1..1
	return 0.5f * (Alignment + 1.f);
}

// ---------------------------------------------------------------------------
// Powerups
// ---------------------------------------------------------------------------

bool ANeonAIController::FindNearestPickup(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, FVector2D& OutPos) const
{
	if (P.CollectWeight <= 0.f || P.CollectRange <= 0.f)
	{
		return false;
	}
	const UNeonPowerupComponent* Inventory = M.GetPowerups();
	if (!Inventory || !Inventory->CanCollect())
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector2D Self = M.GetSimPosition();
	float BestDistSq = P.CollectRange * P.CollectRange;
	bool bFound = false;
	for (TActorIterator<ANeonPowerupPickup> It(World); It; ++It)
	{
		const ANeonPowerupPickup* Pickup = *It;
		if (!IsValid(Pickup) || Pickup->GetPowerupType() == ENeonPowerupType::None)
		{
			continue;
		}
		const FVector2D Pos = Pickup->GetPosition2D();
		const float DistSq = (Pos - Self).SizeSquared();
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			OutPos = Pos;
			bFound = true;
		}
	}
	return bFound;
}

float ANeonAIController::ScoreCollect(const FNeonAIDifficultyParams& P, const FPlanSample& Sample, const FVector2D* PickupPos) const
{
	if (!PickupPos)
	{
		return 0.f;
	}
	FVector2D ToPickup = *PickupPos - Sample.EndPos;
	const float Dist = ToPickup.Size();
	if (Dist <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}
	ToPickup /= Dist;
	// Alignment with the pickup, scaled up as it gets closer so the AI actually commits to the last stretch.
	const float Alignment = 0.5f * (FVector2D::DotProduct(DirectionFromHeading(Sample.EndHeading), ToPickup) + 1.f);
	const float Proximity = 1.f - FMath::Clamp(Dist / FMath::Max(P.CollectRange, 1.f), 0.f, 1.f);
	return Alignment * (0.5f + 0.5f * Proximity);
}

bool ANeonAIController::ShouldActivatePowerup(const UNeonMovementComponent& M, const FNeonAIDifficultyParams& P, const FPlanSample& Pick, bool bEmergency, int32 SurvivorCount)
{
	const UNeonPowerupComponent* Inventory = M.GetPowerups();
	if (!Inventory)
	{
		return false;
	}
	const ENeonPowerupType Held = Inventory->GetHeld();
	if (Held == ENeonPowerupType::None)
	{
		return false;
	}
	// One effect at a time: do not stack a second timed powerup on a running one.
	if (Inventory->GetActiveType() != ENeonPowerupType::None && Inventory->GetActiveSecondsRemaining() > 0.f)
	{
		return false;
	}

	const float Skill = FMath::Clamp(P.PowerupSkill, 0.f, 1.f);
	const bool bInDanger = bEmergency || SurvivorCount == 0 || !Pick.bSurvives || Pick.FreeFraction < 0.6f;
	const bool bOpenStraight = Pick.bSurvives && Pick.Plan.Steering == 0.f && Pick.Openness > 0.7f && Pick.Escape > 0.5f;

	bool bGoodMoment = false;
	switch (Held)
	{
	case ENeonPowerupType::Shield:
		// Cheap insurance: arm it early rather than hold it. Skilled AI still prefers a dangerous moment,
		// but after a while holding it any moment is fine.
		bGoodMoment = bInDanger || Rng.FRand() < 0.15f;
		break;
	case ENeonPowerupType::Phase:
	case ENeonPowerupType::TrailCutter:
		bGoodMoment = bInDanger;
		break;
	case ENeonPowerupType::Boost:
	case ENeonPowerupType::Overdrive:
		bGoodMoment = bOpenStraight && !bInDanger;
		break;
	case ENeonPowerupType::EMP:
	{
		const UNeonMovementComponent* Target = FindNearestOpponent(M);
		const float Range = Inventory->GetConfig()->EMPRange;
		bGoodMoment = Target && (Target->GetSimPosition() - M.GetSimPosition()).SizeSquared() <= Range * Range;
		break;
	}
	default:
		break;
	}

	if (bGoodMoment)
	{
		// Skilled AI takes the moment; the rest hesitate (and may lose it).
		return Rng.FRand() < FMath::Lerp(0.35f, 1.f, Skill);
	}
	// Unskilled AI also fires at random, wasting the powerup. Per-decision chance, so roughly once every few seconds at Easy.
	const float WasteChance = (1.f - Skill) * 0.03f;
	return WasteChance > 0.f && Rng.FRand() < WasteChance;
}
