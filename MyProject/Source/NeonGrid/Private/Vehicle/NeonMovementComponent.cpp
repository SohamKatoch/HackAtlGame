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
		SetArenaBounds(InArena->GetBounds());
		FloorOffset = InArena->VehicleSpawnHeight;
	}
}

void UNeonMovementComponent::ResetSimulation(const FVector& WorldLocation, float HeadingDegrees)
{
	SimPosition = FVector2D(WorldLocation.X, WorldLocation.Y);
	PrevSimPosition = SimPosition;
	SimHeading = FRotator::NormalizeAxis(HeadingDegrees);
	PrevSimHeading = SimHeading;
	FloorZ = WorldLocation.Z;
	if (const ANeonArena* Arena = HeightSource.Get())
	{
		FloorOffset = WorldLocation.Z - Arena->GetFloorHeight(SimPosition);
	}

	AppliedSteering = 0.f;
	Accumulator = 0.f;
	bAlive = true;
	bActivateLatched = false;

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

	float TurnRateDeg = C->MaxTurnRate * TurnMult;

	// Radius clamp: radius = speed / turnRate(rad). Never let the radius fall under the minimum.
	if (C->MinTurnRadius > KINDA_SMALL_NUMBER)
	{
		const float MaxFromRadius = FMath::RadiansToDegrees(Speed / C->MinTurnRadius);
		TurnRateDeg = FMath::Min(TurnRateDeg, MaxFromRadius);
	}

	return TurnRateDeg;
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

	// Powerups: effects count down and a latched activation is attempted, before this step's physics.
	if (Powerups)
	{
		Powerups->PreStep(bActivateLatched);
	}
	bActivateLatched = false;

	// Speed. MVP is constant cruise unless a powerup overrides it.
	float SpeedMult = SpeedMultiplier;
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

	// Boundary.
	if (bHasArenaBounds)
	{
		const float R = C->CollisionRadius;
		const FVector2D InnerMin = ArenaBounds.Min + FVector2D(R, R);
		const FVector2D InnerMax = ArenaBounds.Max - FVector2D(R, R);

		const bool bInside =
			NewPosition.X >= InnerMin.X && NewPosition.X <= InnerMax.X &&
			NewPosition.Y >= InnerMin.Y && NewPosition.Y <= InnerMax.Y;

		if (!bInside)
		{
			// Park the vehicle on the wall so the hit reads clearly, then eliminate.
			NewPosition.X = FMath::Clamp(NewPosition.X, InnerMin.X, InnerMax.X);
			NewPosition.Y = FMath::Clamp(NewPosition.Y, InnerMin.Y, InnerMax.Y);
			SimPosition = NewPosition;
			PrevSimPosition = NewPosition;
			PrevSimHeading = SimHeading;
			++SimulationTick;
			Eliminate(ENeonEliminationCause::Boundary);
			return;
		}
	}

	// Trails and other vehicles. Phase and Shield are applied inside CheckHazards.
	ENeonEliminationCause Cause = ENeonEliminationCause::None;
	if (CheckHazards(SimPosition, NewPosition, Cause))
	{
		SimPosition = NewPosition;
		PrevSimPosition = NewPosition;
		PrevSimHeading = SimHeading;
		++SimulationTick;
		Eliminate(Cause);
		return;
	}

	SimPosition = NewPosition;
	++SimulationTick;

	// Emit trail from the rear of the vehicle, after the move has been accepted.
	if (Trail)
	{
		const FVector2D RearPoint = SimPosition - Direction * C->TrailRearOffset;
		Trail->RecordPosition(RearPoint, SimHeading, SimulationTick);
	}
}

bool UNeonMovementComponent::CheckHazards(const FVector2D& From, const FVector2D& To, ENeonEliminationCause& OutCause)
{
	if (!Collision)
	{
		return false;
	}

	const UNeonVehicleConfig* C = GetConfig();
	const float R = C->CollisionRadius;

	FNeonTrailHit Hit;
	const int32 SelfIgnoreAfterTick = SimulationTick - C->SelfTrailGraceTicks;
	// Phase ignores trails outright; a Shield charge absorbs the first hit and grants a few pass-through ticks.
	const bool bPhased = Powerups && Powerups->IsPhased();
	if (!bPhased && Collision->SweepAgainstTrails(From, To, R, PlayerSlot, SelfIgnoreAfterTick, Hit))
	{
		if (!(Powerups && Powerups->TryAbsorbTrailHit()))
		{
			OutCause = (Hit.OwnerId == PlayerSlot) ? ENeonEliminationCause::OwnTrail : ENeonEliminationCause::Trail;
			return true;
		}
	}

	if (UNeonMovementComponent* Other = Collision->FindVehicleOverlap(this, To, R))
	{
		OutCause = ENeonEliminationCause::Vehicle;
		Other->ForceEliminate(ENeonEliminationCause::Vehicle);
		return true;
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

	float Z = FloorZ;
	float Pitch = 0.f;
	float Roll = 0.f;

	// Terrain is presentation only (D25): Z and tilt are derived from the 2D position after the step.
	if (const ANeonArena* Arena = HeightSource.Get())
	{
		Z = Arena->GetFloorHeight(Pos) + FloorOffset;

		if (bAlignToFloor)
		{
			const FVector N = Arena->GetFloorNormal(Pos);
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
	ApplyInterpolatedTransform(1.f);

	if (Trail)
	{
		Trail->StopTrail();
	}

	UE_LOG(LogNeonGrid, Log, TEXT("%s eliminated (%s) at tick %d, pos (%.0f, %.0f)"),
		*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Cause), SimulationTick, SimPosition.X, SimPosition.Y);

	OnEliminated.Broadcast(GetOwner(), Cause);
}
