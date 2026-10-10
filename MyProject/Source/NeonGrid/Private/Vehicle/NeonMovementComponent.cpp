#include "Vehicle/NeonMovementComponent.h"

#include "Arena/NeonArena.h"
#include "NeonGrid.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Trail/NeonCollisionSystem.h"
#include "Trail/NeonTrailComponent.h"
#include "Vehicle/NeonVehicleConfig.h"

UNeonMovementComponent::UNeonMovementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UNeonMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		Collision = World->GetSubsystem<UNeonCollisionSystem>();
		if (Collision)
		{
			Collision->RegisterVehicle(this);
		}
	}

	if (AActor* Owner = GetOwner())
	{
		Trail = Owner->FindComponentByClass<UNeonTrailComponent>();
		if (Trail)
		{
			Trail->SetOwnerId(PlayerSlot);
		}
		Powerups = Owner->FindComponentByClass<UNeonPowerupComponent>();

		// Adopt whatever transform the actor was spawned with so a pawn dropped into a level still drives.
		ResetSimulation(Owner->GetActorLocation(), Owner->GetActorRotation().Yaw);
	}
}

void UNeonMovementComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Collision)
	{
		Collision->UnregisterVehicle(this);
	}
	Super::EndPlay(EndPlayReason);
}

void UNeonMovementComponent::SetPlayerSlot(int32 InSlot)
{
	PlayerSlot = InSlot;
	if (Trail)
	{
		Trail->SetOwnerId(PlayerSlot);
	}
}

void UNeonMovementComponent::ForceEliminate(ENeonEliminationCause Cause)
{
	Eliminate(Cause);
}

const UNeonVehicleConfig* UNeonMovementComponent::GetConfig() const
{
	return Config ? Config.Get() : GetDefault<UNeonVehicleConfig>();
}

void UNeonMovementComponent::SetInput(const FNeonVehicleInput& InInput)
{
	PendingInput = InInput;
	PendingInput.Steering = FMath::Clamp(PendingInput.Steering, -1.f, 1.f);

	if (PendingInput.bActivatePowerup && !bLastActivateInput)
	{
		bActivateLatched = true;
	}
	bLastActivateInput = PendingInput.bActivatePowerup;

	if (PendingInput.bJump && !bLastJumpInput)
	{
		bJumpLatched = true;
	}
	bLastJumpInput = PendingInput.bJump;
}

void UNeonMovementComponent::SetArenaBounds(const FBox2D& InBounds)
{
	ArenaBounds = InBounds;
	bHasArenaBounds = true;
}

void UNeonMovementComponent::SetArena(const ANeonArena* InArena)
{
	HeightSource = InArena;
	if (InArena)
	{
		SetArenaBounds(InArena->GetFullBounds());
		FloorOffset = InArena->VehicleSpawnHeight;
	}
}

FBox2D UNeonMovementComponent::GetArenaBounds() const
{
	if (const ANeonArena* Arena = HeightSource.Get())
	{
		return Arena->GetBounds();
	}
	return ArenaBounds;
}

FBox2D UNeonMovementComponent::GetArenaBoundsAt(float SecondsAhead) const
{
	if (const ANeonArena* Arena = HeightSource.Get())
	{
		return Arena->GetBoundsAt(SecondsAhead);
	}
	return ArenaBounds;
}

void UNeonMovementComponent::ResetSimulation(const FVector& WorldLocation, float HeadingDegrees)
{
	SimPosition = FVector2D(WorldLocation.X, WorldLocation.Y);
	PrevSimPosition = SimPosition;
	SimHeading = FRotator::NormalizeAxis(HeadingDegrees);
	PrevSimHeading = SimHeading;
	FloorZ = WorldLocation.Z;
	SimLayer = 0;
	if (const ANeonArena* Arena = HeightSource.Get())
	{
		SimLayer = Arena->ResolveLayerAt(SimPosition, WorldLocation.Z, 0);
		FloorOffset = WorldLocation.Z - Arena->GetFloorHeightOnLayer(SimPosition, SimLayer);
	}

	AppliedSteering = 0.f;
	Accumulator = 0.f;
	bAlive = true;
	bActivateLatched = false;
	bJumpLatched = false;
	bLastJumpInput = false;
	bAirborne = false;
	VerticalVelocity = 0.f;
	PeakClimbVz = 0.f;
	TimeSinceClimb = 1.f;
	SimZ = WorldLocation.Z;
	PrevSimZ = SimZ;
	SprintMeter = 1.f;
	JumpMeter = 1.f;
	SprintTimeLeft = 0.f;
	bSprintingApplied = false;
	bSprintHeld = false;

	if (Trail)
	{
		Trail->ResetTrail();
	}
	if (Powerups)
	{
		Powerups->ResetForRound();
	}

	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocationAndRotation(WorldLocation, FRotator(0.f, SimHeading, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void UNeonMovementComponent::SetFrozen(bool bInFrozen)
{
	bFrozen = bInFrozen;
	if (bFrozen)
	{
		Accumulator = 0.f;
	}
}

void UNeonMovementComponent::SetSpeedModifiers(float InSpeedMultiplier, float InTurnRateMultiplier)
{
	SpeedMultiplier = FMath::Max(InSpeedMultiplier, 0.01f);
	TurnRateMultiplier = FMath::Clamp(InTurnRateMultiplier, 0.01f, 1.f);
}

float UNeonMovementComponent::ComputeTurnRateDegrees(float Speed) const
{
	const UNeonVehicleConfig* C = GetConfig();

	float TurnMult = TurnRateMultiplier;
	if (Powerups)
	{
		TurnMult *= Powerups->GetTurnRateMultiplier(); // Boost / Overdrive / EMP
	}
	if (bSprintingApplied)
	{
		TurnMult *= C->SprintTurnMultiplier;
	}

	float TurnRateDeg = C->MaxTurnRate * TurnMult;

	// Radius clamp: radius = speed / turnRate(rad). Never let the radius fall under the minimum.
	if (C->MinTurnRadius > KINDA_SMALL_NUMBER)
	{
		const float MaxFromRadius = FMath::RadiansToDegrees(Speed / C->MinTurnRadius);
		TurnRateDeg = FMath::Min(TurnRateDeg, MaxFromRadius);
	}

	return TurnRateDeg;
}

bool UNeonMovementComponent::CanJump() const
{
	return bAlive && !bAirborne && JumpMeter + KINDA_SMALL_NUMBER >= GetConfig()->JumpCost;
}

bool UNeonMovementComponent::CanSprint() const
{
	return bAlive && SprintTimeLeft <= 0.f && SprintMeter >= 0.98f;
}

float UNeonMovementComponent::GetSprintMeter() const
{
	const UNeonVehicleConfig* C = GetConfig();
	if (SprintTimeLeft > 0.f && C->SprintBurstSeconds > KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(SprintTimeLeft / C->SprintBurstSeconds, 0.f, 1.f);
	}
	return SprintMeter;
}

void UNeonMovementComponent::UpdateMeters(float Dt)
{
	const UNeonVehicleConfig* C = GetConfig();

	// Rising edge only. Holding the key after a burst does not start the next one early.
	const bool bRisingSprint = PendingInput.bSprint && !bSprintHeld;
	bSprintHeld = PendingInput.bSprint;

	if (SprintTimeLeft > 0.f)
	{
		SprintTimeLeft = FMath::Max(0.f, SprintTimeLeft - Dt);
		bSprintingApplied = SprintTimeLeft > 0.f;
	}
	else if (bRisingSprint && SprintMeter >= 0.98f)
	{
		// The whole bar is the stake. Releasing early does not refund it.
		SprintMeter = 0.f;
		SprintTimeLeft = C->SprintBurstSeconds;
		bSprintingApplied = true;
	}
	else
	{
		bSprintingApplied = false;
		SprintMeter = FMath::Min(1.f, SprintMeter + C->SprintRechargePerSecond * Dt);
	}

	JumpMeter = FMath::Min(C->JumpCost, JumpMeter + C->JumpRechargePerSecond * Dt);
}

float UNeonMovementComponent::GetCurrentTurnRadius() const
{
	const float TurnRateRad = FMath::DegreesToRadians(ComputeTurnRateDegrees(CurrentSpeed));
	return TurnRateRad > KINDA_SMALL_NUMBER ? CurrentSpeed / TurnRateRad : TNumericLimits<float>::Max();
}

void UNeonMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const float Step = 1.f / static_cast<float>(FMath::Max(SimulationHz, 1));

	if (bFrozen)
	{
		Accumulator = 0.f;
		ApplyInterpolatedTransform(1.f);
		return;
	}

	Accumulator += DeltaTime;

	int32 Steps = 0;
	while (Accumulator >= Step && Steps < MaxStepsPerFrame)
	{
		StepSimulation(Step);
		Accumulator -= Step;
		++Steps;
	}

	// Hitch: drop the backlog rather than simulate unbounded time.
	if (Steps >= MaxStepsPerFrame && Accumulator >= Step)
	{
		Accumulator = 0.f;
	}

	ApplyInterpolatedTransform(bAlive ? FMath::Clamp(Accumulator / Step, 0.f, 1.f) : 1.f);
}

void UNeonMovementComponent::StepSimulation(float Dt)
{
	if (!bAlive)
	{
		return;
	}

	const UNeonVehicleConfig* C = GetConfig();

	PrevSimPosition = SimPosition;
	PrevSimHeading = SimHeading;
	PrevSimZ = SimZ;

	// Powerups: effects count down and a latched activation is attempted, before this step's physics.
	if (Powerups)
	{
		Powerups->PreStep(bActivateLatched);
	}
	bActivateLatched = false;

	UpdateMeters(Dt);

	// Cruise, unless sprint and/or a powerup override it. Sprint stacks with Boost.
	float SpeedMult = SpeedMultiplier;
	if (bSprintingApplied)
	{
		SpeedMult *= C->SprintSpeedMultiplier;
	}
	if (Powerups)
	{
		SpeedMult *= Powerups->GetSpeedMultiplier(); // Boost / Overdrive
	}
	CurrentSpeed = FMath::Max(C->BaseSpeed * SpeedMult, C->MinSpeed);

	// Heading.
	AppliedSteering = PendingInput.Steering;
	const float TurnRateDeg = ComputeTurnRateDegrees(CurrentSpeed);
	SimHeading = FRotator::NormalizeAxis(SimHeading + AppliedSteering * TurnRateDeg * Dt);

	// Position.
	const float HeadingRad = FMath::DegreesToRadians(SimHeading);
	const FVector2D Direction(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad));
	FVector2D NewPosition = SimPosition + Direction * CurrentSpeed * Dt;

	// Boundary. The rectangle shrinks during the collapse (D27). On The Hex the boundary is the hexagon.
	if (bHasArenaBounds)
	{
		const float R = C->CollisionRadius;
		const ANeonArena* BoundsArena = HeightSource.Get();
		const bool bInside = BoundsArena
			? BoundsArena->IsInsideBounds(NewPosition, R)
			: GetArenaBounds().IsInside(NewPosition);

		if (!bInside)
		{
			// Park the vehicle on the wall so the hit reads clearly, then eliminate.
			if (BoundsArena)
			{
				FVector2D Inside = SimPosition;
				FVector2D Outside = NewPosition;
				for (int32 Step = 0; Step < 8; ++Step)
				{
					const FVector2D Mid = (Inside + Outside) * 0.5f;
					if (BoundsArena->IsInsideBounds(Mid, R))
					{
						Inside = Mid;
					}
					else
					{
						Outside = Mid;
					}
				}
				NewPosition = Inside;
			}
			else
			{
				const FBox2D Bounds = GetArenaBounds();
				NewPosition.X = FMath::Clamp(NewPosition.X, Bounds.Min.X + R, Bounds.Max.X - R);
				NewPosition.Y = FMath::Clamp(NewPosition.Y, Bounds.Min.Y + R, Bounds.Max.Y - R);
			}
			SimPosition = NewPosition;
			PrevSimPosition = NewPosition;
			PrevSimHeading = SimHeading;
			PrevSimZ = SimZ;
			++SimulationTick;
			Eliminate(ENeonEliminationCause::Boundary);
			return;
		}
	}

	// Height. Grounded sticks to the current layer. Wrong-way ramps and deck rims are walls.
	const bool bWasAirborne = bAirborne;
	const ANeonArena* Arena = HeightSource.Get();
	if (Arena)
	{
		if (Arena->IsWrongWayApproach(SimPosition, NewPosition, SimHeading, SimLayer, SimZ))
		{
			SimPosition = NewPosition;
			PrevSimPosition = NewPosition;
			PrevSimHeading = SimHeading;
			PrevSimZ = SimZ;
			++SimulationTick;
			Eliminate(ENeonEliminationCause::Boundary);
			return;
		}

		float FloorHere = SimZ;
		float SurfaceHere = 0.f;
		if (Arena->TryGetFloorHeightOnLayer(SimPosition, SimLayer, SurfaceHere))
		{
			FloorHere = SurfaceHere + FloorOffset;
		}

		if (!bAirborne)
		{
			if (bJumpLatched && JumpMeter + KINDA_SMALL_NUMBER >= C->JumpCost)
			{
				JumpMeter = FMath::Max(0.f, JumpMeter - C->JumpCost);
				bAirborne = true;
				VerticalVelocity = C->JumpImpulse;
				SimZ = FloorHere;
				PeakClimbVz = 0.f;
			}
			else
			{
				SimLayer = Arena->AdvanceLayer(NewPosition, SimLayer);
				float Floor = 0.f;
				if (Arena->TryGetFloorHeightOnLayer(NewPosition, SimLayer, Floor))
				{
					Floor += FloorOffset;
					const float Dist = (NewPosition - SimPosition).Size();
					VerticalVelocity = Dist > 1.f ? (Floor - FloorHere) / Dist * CurrentSpeed : 0.f;
					SimZ = Floor;
				}
				else
				{
					bAirborne = true;
					VerticalVelocity = FMath::Min(VerticalVelocity, 0.f);
				}
			}
		}

		if (bAirborne)
		{
			const float PrevZ = SimZ;
			VerticalVelocity -= C->Gravity * Dt;
			SimZ += VerticalVelocity * Dt;

			float CeilingZ = 0.f;
			if (VerticalVelocity > 0.f && Arena->HitsCeiling(NewPosition, PrevZ, SimZ, CeilingZ))
			{
				SimZ = CeilingZ - 40.f;
				VerticalVelocity = -200.f;
			}

			float BestSurface = -1.e12f;
			int32 BestLayer = INDEX_NONE;
			const int32 MaxLayer = Arena->GetHighestFloorLayer();
			for (int32 Layer = 0; Layer <= MaxLayer; ++Layer)
			{
				float H = 0.f;
				if (!Arena->TryGetFloorHeightOnLayer(NewPosition, Layer, H))
				{
					continue;
				}
				const float Surface = H + FloorOffset;
				if (PrevZ + 8.f >= Surface && SimZ <= Surface && Surface > BestSurface)
				{
					BestSurface = Surface;
					BestLayer = Layer;
				}
			}
			if (BestLayer != INDEX_NONE)
			{
				SimZ = BestSurface;
				VerticalVelocity = 0.f;
				bAirborne = false;
				SimLayer = BestLayer;
				PeakClimbVz = 0.f;
				TimeSinceClimb = 1.f;
			}
			else if (SimZ < Arena->GetActorLocation().Z - 1600.f)
			{
				SimPosition = NewPosition;
				PrevSimPosition = NewPosition;
				PrevSimHeading = SimHeading;
				PrevSimZ = SimZ;
				++SimulationTick;
				Eliminate(ENeonEliminationCause::Boundary);
				return;
			}
		}
	}
	else
	{
		SimZ = FloorZ;
		bAirborne = false;
	}

	// A grounded tap is spent even when the meter is short, so it does not fire a second later.
	// A press in the air stays latched and fires on landing.
	if (!bWasAirborne)
	{
		bJumpLatched = false;
	}

	// Trails use Z. A laser in the air only kills a bike at that height.
	ENeonEliminationCause Cause = ENeonEliminationCause::None;
	if (CheckHazards(SimPosition, NewPosition, PrevSimZ, SimZ, Cause))
	{
		SimPosition = NewPosition;
		PrevSimPosition = NewPosition;
		PrevSimHeading = SimHeading;
		PrevSimZ = SimZ;
		++SimulationTick;
		Eliminate(Cause);
		return;
	}

	SimPosition = NewPosition;
	++SimulationTick;

	// The laser follows the vehicle center, including through a jump.
	if (Trail)
	{
		const FVector2D RearPoint = SimPosition - Direction * C->TrailRearOffset;
		Trail->RecordPosition(RearPoint, SimZ, SimHeading, SimulationTick);
	}
}

bool UNeonMovementComponent::CheckHazards(const FVector2D& From, const FVector2D& To, float FromZ, float ToZ, ENeonEliminationCause& OutCause)
{
	if (!Collision)
	{
		return false;
	}

	const UNeonVehicleConfig* C = GetConfig();
	const float R = C->CollisionRadius;

	FNeonTrailHit Hit;
	const int32 SelfIgnoreAfterTick = SimulationTick - C->SelfTrailGraceTicks;
	// Phase ignores trails. Height does not: a jump misses a laser that is below the bike.
	const bool bPhased = Powerups && Powerups->IsPhased();
	if (!bPhased && Collision->SweepAgainstTrails(From, To, R, PlayerSlot, SelfIgnoreAfterTick, Hit, FromZ, ToZ, C->TrailVerticalRadius))
	{
		if (!(Powerups && Powerups->TryAbsorbTrailHit()))
		{
			OutCause = (Hit.OwnerId == PlayerSlot) ? ENeonEliminationCause::OwnTrail : ENeonEliminationCause::Trail;
			return true;
		}
	}

	if (UNeonMovementComponent* Other = Collision->FindVehicleOverlap(this, To, R))
	{
		// A bike in the air clears another bike. Same height still trades.
		if (FMath::Abs(SimZ - Other->GetSimZ()) <= C->AirCollisionHeight)
		{
			OutCause = ENeonEliminationCause::Vehicle;
			Other->ForceEliminate(ENeonEliminationCause::Vehicle);
			return true;
		}
	}

	return false;
}

void UNeonMovementComponent::ApplyInterpolatedTransform(float Alpha)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const FVector2D Pos = FMath::Lerp(PrevSimPosition, SimPosition, Alpha);
	const float Yaw = PrevSimHeading + FRotator::NormalizeAxis(SimHeading - PrevSimHeading) * Alpha;

	float Z = FMath::Lerp(PrevSimZ, SimZ, Alpha);
	float Pitch = 0.f;
	float Roll = 0.f;

	if (bAirborne)
	{
		// Nose follows the ballistic arc. The camera still inherits yaw only.
		const float Speed = FMath::Max(CurrentSpeed, 1.f);
		Pitch = FMath::RadiansToDegrees(FMath::Atan2(VerticalVelocity, Speed));
	}
	else if (const ANeonArena* Arena = HeightSource.Get())
	{
		if (bAlignToFloor)
		{
			const FVector N = Arena->GetFloorNormalOnLayer(Pos, SimLayer);
			if (N.Z > KINDA_SMALL_NUMBER)
			{
				const float YawRad = FMath::DegreesToRadians(Yaw);
				const FVector2D Forward(FMath::Cos(YawRad), FMath::Sin(YawRad));
				const FVector2D Right(-Forward.Y, Forward.X);
				// Height gradient g = (-N.x, -N.y) / N.z. Slope along an axis = g . axis.
				const float SlopeForward = -(N.X * Forward.X + N.Y * Forward.Y) / N.Z;
				const float SlopeRight = -(N.X * Right.X + N.Y * Right.Y) / N.Z;
				Pitch = FMath::RadiansToDegrees(FMath::Atan(SlopeForward)); // +pitch raises the nose
				Roll = FMath::RadiansToDegrees(FMath::Atan(SlopeRight));    // +roll raises the right side
			}
		}
	}

	Owner->SetActorLocationAndRotation(FVector(Pos.X, Pos.Y, Z), FRotator(Pitch, Yaw, Roll), false, nullptr, ETeleportType::TeleportPhysics);
}

void UNeonMovementComponent::Eliminate(ENeonEliminationCause Cause)
{
	if (!bAlive)
	{
		return;
	}

	bAlive = false;
	AppliedSteering = 0.f;
	bSprintingApplied = false;
	if (bAirborne)
	{
		if (const ANeonArena* Arena = HeightSource.Get())
		{
			SimZ = Arena->GetFloorHeightOnLayer(SimPosition, SimLayer) + FloorOffset;
		}
		PrevSimZ = SimZ;
		bAirborne = false;
		VerticalVelocity = 0.f;
	}
	ApplyInterpolatedTransform(1.f);

	if (Trail)
	{
		Trail->StopTrail();
	}

	UE_LOG(LogNeonGrid, Log, TEXT("%s eliminated (%s) at tick %d, pos (%.0f, %.0f)"),
		*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Cause), SimulationTick, SimPosition.X, SimPosition.Y);

	OnEliminated.Broadcast(GetOwner(), Cause);
}
