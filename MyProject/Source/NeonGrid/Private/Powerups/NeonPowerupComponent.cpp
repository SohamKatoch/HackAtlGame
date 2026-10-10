#include "Powerups/NeonPowerupComponent.h"

#include "NeonGrid.h"
#include "Powerups/NeonPowerupConfig.h"
#include "Trail/NeonCollisionSystem.h"
#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehicleConfig.h"

UNeonPowerupComponent::UNeonPowerupComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UNeonPowerupComponent::BeginPlay()
{
	Super::BeginPlay();
	if (const AActor* Owner = GetOwner())
	{
		Movement = Owner->FindComponentByClass<UNeonMovementComponent>();
	}
	if (UWorld* World = GetWorld())
	{
		Collision = World->GetSubsystem<UNeonCollisionSystem>();
	}
}

const UNeonPowerupConfig* UNeonPowerupComponent::GetConfig() const
{
	return Config ? Config.Get() : GetDefault<UNeonPowerupConfig>();
}

int32 UNeonPowerupComponent::SecondsToTicks(float Seconds) const
{
	const int32 Hz = Movement ? FMath::Max(Movement->SimulationHz, 1) : 60;
	return FMath::Max(1, FMath::RoundToInt(Seconds * Hz));
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

bool UNeonPowerupComponent::Collect(ENeonPowerupType Type)
{
	if (Type == ENeonPowerupType::None || Held != ENeonPowerupType::None)
	{
		return false;
	}
	Held = Type;
	UE_LOG(LogNeonGrid, Log, TEXT("%s collected %s"), *GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Type));
	OnPowerupEvent.Broadcast(Type, ENeonPowerupEvent::Collected);
	return true;
}

void UNeonPowerupComponent::ResetForRound()
{
	Held = ENeonPowerupType::None;
	ActiveType = ENeonPowerupType::None;
	ActiveTicksRemaining = 0;
	ActiveTicksTotal = 0;
	ShieldCharges = 0;
	PassThroughTicksRemaining = 0;
	EMPTicksRemaining = 0;
	EMPTicksTotal = 0;
	EMPSteeringMultiplier = 1.f;
	OnPowerupEvent.Broadcast(ENeonPowerupType::None, ENeonPowerupEvent::Cleared);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

float UNeonPowerupComponent::GetActiveFraction() const
{
	if (ActiveType == ENeonPowerupType::None)
	{
		return 0.f;
	}
	if (ActiveTicksRemaining < 0 || ActiveTicksTotal <= 0)
	{
		return 1.f;
	}
	return FMath::Clamp(static_cast<float>(ActiveTicksRemaining) / static_cast<float>(ActiveTicksTotal), 0.f, 1.f);
}

float UNeonPowerupComponent::GetActiveSecondsRemaining() const
{
	if (ActiveType == ENeonPowerupType::None || ActiveTicksRemaining < 0)
	{
		return 0.f;
	}
	const int32 Hz = Movement ? FMath::Max(Movement->SimulationHz, 1) : 60;
	return static_cast<float>(ActiveTicksRemaining) / Hz;
}

bool UNeonPowerupComponent::IsPhased() const
{
	return (ActiveType == ENeonPowerupType::Phase && ActiveTicksRemaining > 0) || PassThroughTicksRemaining > 0;
}

float UNeonPowerupComponent::GetEMPFraction() const
{
	if (EMPTicksRemaining <= 0 || EMPTicksTotal <= 0)
	{
		return 0.f;
	}
	return FMath::Clamp(static_cast<float>(EMPTicksRemaining) / static_cast<float>(EMPTicksTotal), 0.f, 1.f);
}

float UNeonPowerupComponent::GetSpeedMultiplier() const
{
	const UNeonPowerupConfig* C = GetConfig();
	switch (ActiveType)
	{
	case ENeonPowerupType::Boost:     return ActiveTicksRemaining > 0 ? C->BoostSpeedMultiplier : 1.f;
	case ENeonPowerupType::Overdrive: return ActiveTicksRemaining > 0 ? C->OverdriveSpeedMultiplier : 1.f;
	default:                          return 1.f;
	}
}

float UNeonPowerupComponent::GetTurnRateMultiplier() const
{
	const UNeonPowerupConfig* C = GetConfig();
	float Mult = 1.f;
	switch (ActiveType)
	{
	case ENeonPowerupType::Boost:     Mult *= ActiveTicksRemaining > 0 ? C->BoostTurnRateMultiplier : 1.f; break;
	case ENeonPowerupType::Overdrive: Mult *= ActiveTicksRemaining > 0 ? C->OverdriveTurnRateMultiplier : 1.f; break;
	default: break;
	}
	if (EMPTicksRemaining > 0)
	{
		Mult *= EMPSteeringMultiplier;
	}
	return FMath::Clamp(Mult, 0.01f, 1.f);
}

// ---------------------------------------------------------------------------
// Simulation hooks
// ---------------------------------------------------------------------------

void UNeonPowerupComponent::PreStep(bool bActivateRequested)
{
	// Count down first so an effect started this step gets its full duration.
	if (ActiveType != ENeonPowerupType::None && ActiveTicksRemaining > 0)
	{
		if (--ActiveTicksRemaining == 0)
		{
			ExpireActive();
		}
	}
	if (PassThroughTicksRemaining > 0)
	{
		--PassThroughTicksRemaining;
	}
	if (EMPTicksRemaining > 0)
	{
		if (--EMPTicksRemaining == 0)
		{
			EMPSteeringMultiplier = 1.f;
			OnPowerupEvent.Broadcast(ENeonPowerupType::EMP, ENeonPowerupEvent::Expired);
		}
	}

	if (bActivateRequested && Held != ENeonPowerupType::None)
	{
		TryActivate();
	}
}

bool UNeonPowerupComponent::TryAbsorbTrailHit()
{
	if (ShieldCharges <= 0)
	{
		return false;
	}
	--ShieldCharges;
	PassThroughTicksRemaining = GetConfig()->ShieldPassThroughTicks;
	UE_LOG(LogNeonGrid, Log, TEXT("%s shield absorbed a trail hit (%d charges left)"), *GetNameSafe(GetOwner()), ShieldCharges);
	OnPowerupEvent.Broadcast(ENeonPowerupType::Shield, ENeonPowerupEvent::ShieldBroken);
	if (ShieldCharges == 0 && ActiveType == ENeonPowerupType::Shield)
	{
		ActiveType = ENeonPowerupType::None;
		ActiveTicksRemaining = 0;
		ActiveTicksTotal = 0;
	}
	return true;
}

void UNeonPowerupComponent::ApplyEMP(int32 DurationTicks, float SteeringMultiplier)
{
	EMPTicksRemaining = FMath::Max(DurationTicks, 1);
	EMPTicksTotal = EMPTicksRemaining;
	EMPSteeringMultiplier = FMath::Clamp(SteeringMultiplier, 0.05f, 1.f);
	UE_LOG(LogNeonGrid, Log, TEXT("%s hit by EMP for %d ticks (steer x%.2f)"), *GetNameSafe(GetOwner()), EMPTicksRemaining, EMPSteeringMultiplier);
	OnPowerupEvent.Broadcast(ENeonPowerupType::EMP, ENeonPowerupEvent::EMPHit);
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

void UNeonPowerupComponent::StartTimed(ENeonPowerupType Type, int32 Ticks)
{
	ActiveType = Type;
	ActiveTicksRemaining = Ticks;
	ActiveTicksTotal = FMath::Max(Ticks, 1);
}

void UNeonPowerupComponent::ExpireActive()
{
	const ENeonPowerupType Was = ActiveType;
	ActiveType = ENeonPowerupType::None;
	ActiveTicksRemaining = 0;
	ActiveTicksTotal = 0;
	if (Was == ENeonPowerupType::Shield)
	{
		ShieldCharges = 0; // timed-out charge
	}
	OnPowerupEvent.Broadcast(Was, ENeonPowerupEvent::Expired);
}

bool UNeonPowerupComponent::TryActivate()
{
	const ENeonPowerupType Type = Held;
	bool bDone = false;
	switch (Type)
	{
	case ENeonPowerupType::Boost:       bDone = ActivateBoost(); break;
	case ENeonPowerupType::Shield:      bDone = ActivateShield(); break;
	case ENeonPowerupType::Phase:       bDone = ActivatePhase(); break;
	case ENeonPowerupType::TrailCutter: bDone = ActivateTrailCutter(); break;
	case ENeonPowerupType::EMP:         bDone = ActivateEMP(); break;
	case ENeonPowerupType::Overdrive:   bDone = ActivateOverdrive(); break;
	default: break;
	}

	if (bDone)
	{
		Held = ENeonPowerupType::None;
		UE_LOG(LogNeonGrid, Log, TEXT("%s activated %s at tick %d"), *GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Type), Movement ? Movement->GetSimulationTick() : -1);
		OnPowerupEvent.Broadcast(Type, ENeonPowerupEvent::Activated);
	}
	return bDone;
}

bool UNeonPowerupComponent::ActivateBoost()
{
	StartTimed(ENeonPowerupType::Boost, SecondsToTicks(GetConfig()->BoostDuration));
	return true;
}

bool UNeonPowerupComponent::ActivateOverdrive()
{
	StartTimed(ENeonPowerupType::Overdrive, SecondsToTicks(GetConfig()->OverdriveDuration));
	return true;
}

bool UNeonPowerupComponent::ActivateShield()
{
	const UNeonPowerupConfig* C = GetConfig();
	ShieldCharges = FMath::Max(C->ShieldCharges, 1);
	ActiveType = ENeonPowerupType::Shield;
	if (C->ShieldDuration > 0.f)
	{
		ActiveTicksRemaining = SecondsToTicks(C->ShieldDuration);
		ActiveTicksTotal = ActiveTicksRemaining;
	}
	else
	{
		ActiveTicksRemaining = -1; // until used
		ActiveTicksTotal = 0;
	}
	return true;
}

bool UNeonPowerupComponent::ActivatePhase()
{
	StartTimed(ENeonPowerupType::Phase, SecondsToTicks(GetConfig()->PhaseDuration));
	return true;
}

bool UNeonPowerupComponent::ActivateTrailCutter()
{
	if (!Movement || !Collision)
	{
		return false;
	}
	const UNeonPowerupConfig* C = GetConfig();
	const UNeonVehicleConfig* V = Movement->GetConfig();

	const float HeadingRad = FMath::DegreesToRadians(Movement->GetHeading());
	const FVector2D Forward(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad));
	const int32 SelfIgnoreAfterTick = Movement->GetSimulationTick() - V->SelfTrailGraceTicks;

	FVector2D HitPoint;
	FNeonTrailHit Hit;
	if (!Collision->FindFirstTrailAlongRay(Movement->GetSimPosition(), Forward, C->TrailCutterReach, V->CollisionRadius,
		Movement->GetPlayerSlot(), SelfIgnoreAfterTick, HitPoint, Hit, Movement->GetSimZ(), V->TrailVerticalRadius))
	{
		return false; // nothing to cut: keep the powerup
	}

	const int32 Cut = Collision->CutTrailsInCircle(HitPoint, C->TrailCutterLength * 0.5f);
	UE_LOG(LogNeonGrid, Log, TEXT("%s trail cutter removed %d segment(s) around (%.0f, %.0f)"), *GetNameSafe(GetOwner()), Cut, HitPoint.X, HitPoint.Y);
	return Cut > 0;
}

bool UNeonPowerupComponent::ActivateEMP()
{
	if (!Movement || !Collision)
	{
		return false;
	}
	const UNeonPowerupConfig* C = GetConfig();
	const float RangeSq = C->EMPRange * C->EMPRange;

	UNeonMovementComponent* Target = nullptr;
	float BestSq = RangeSq;
	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
	{
		UNeonMovementComponent* Other = Weak.Get();
		if (!Other || Other == Movement || !Other->IsAlive())
		{
			continue;
		}
		const float DistSq = (Other->GetSimPosition() - Movement->GetSimPosition()).SizeSquared();
		if (DistSq <= BestSq)
		{
			BestSq = DistSq;
			Target = Other;
		}
	}
	if (!Target)
	{
		return false; // nobody in range: keep the powerup
	}

	UNeonPowerupComponent* Victim = Target->GetOwner() ? Target->GetOwner()->FindComponentByClass<UNeonPowerupComponent>() : nullptr;
	if (!Victim)
	{
		return false;
	}
	Victim->ApplyEMP(SecondsToTicks(C->EMPDuration), C->EMPSteeringMultiplier);
	return true;
}
