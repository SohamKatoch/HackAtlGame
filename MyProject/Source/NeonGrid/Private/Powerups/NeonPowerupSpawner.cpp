#include "Powerups/NeonPowerupSpawner.h"

#include "Arena/NeonArena.h"
#include "Engine/World.h"
#include "NeonGrid.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Powerups/NeonPowerupConfig.h"
#include "Powerups/NeonPowerupPickup.h"
#include "Trail/NeonCollisionSystem.h"
#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehicleConfig.h"

UNeonPowerupSpawner::UNeonPowerupSpawner()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics; // after every vehicle has stepped this frame
	PickupClass = ANeonPowerupPickup::StaticClass();
	Rng.Initialize(7);
}

const UNeonPowerupConfig* UNeonPowerupSpawner::GetConfig() const
{
	return Config ? Config.Get() : GetDefault<UNeonPowerupConfig>();
}

void UNeonPowerupSpawner::Configure(ANeonArena* InArena, UNeonPowerupConfig* InConfig, bool bInEnabled, int32 Seed)
{
	Arena = InArena;
	Config = InConfig;
	bEnabled = bInEnabled;
	Rng.Initialize(Seed);
	ClearPickups();
}

void UNeonPowerupSpawner::SetSpawning(bool bInActive)
{
	bActive = bInActive;
	if (bActive)
	{
		SpawnTimer = GetConfig()->FirstSpawnDelay;
	}
}

void UNeonPowerupSpawner::ClearPickups()
{
	for (ANeonPowerupPickup* P : Pickups)
	{
		if (IsValid(P))
		{
			P->Destroy();
		}
	}
	Pickups.Reset();
	Lifetimes.Reset();
	SpawnTimer = GetConfig()->FirstSpawnDelay;
}

void UNeonPowerupSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearPickups();
	Super::EndPlay(EndPlayReason);
}

void UNeonPowerupSpawner::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnabled || !bActive)
	{
		return;
	}
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	ExpirePickups(DeltaTime);
	ResolvePickups();

	SpawnTimer -= DeltaTime;
	if (SpawnTimer <= 0.f)
	{
		if (Pickups.Num() < GetConfig()->MaxActivePowerups)
		{
			TrySpawn();
		}
		SpawnTimer = GetConfig()->SpawnInterval;
	}
}

// ---------------------------------------------------------------------------

void UNeonPowerupSpawner::ExpirePickups(float DeltaTime)
{
	// Pickups the collapsing wall has passed over are removed too (D27).
	const ANeonArena* A = Arena.Get();
	const FBox2D Bounds = A ? A->GetBounds() : FBox2D(ForceInit);
	const float Margin = GetConfig()->PickupRadius;

	for (int32 i = Pickups.Num() - 1; i >= 0; --i)
	{
		ANeonPowerupPickup* P = Pickups[i];
		if (!IsValid(P))
		{
			Pickups.RemoveAt(i);
			continue;
		}
		float& Remaining = Lifetimes.FindOrAdd(P);
		Remaining -= DeltaTime;
		bool bOutside = false;
		if (A)
		{
			const FVector2D Pos = P->GetPosition2D();
			bOutside = Pos.X < Bounds.Min.X + Margin || Pos.X > Bounds.Max.X - Margin ||
				Pos.Y < Bounds.Min.Y + Margin || Pos.Y > Bounds.Max.Y - Margin;
		}
		if (Remaining <= 0.f || bOutside)
		{
			UE_LOG(LogNeonGrid, Verbose, TEXT("Pickup %s %s."), *UEnum::GetValueAsString(P->GetPowerupType()), bOutside ? TEXT("swallowed by the wall") : TEXT("expired"));
			Lifetimes.Remove(P);
			P->Destroy();
			Pickups.RemoveAt(i);
		}
	}
}

void UNeonPowerupSpawner::ResolvePickups()
{
	UWorld* World = GetWorld();
	UNeonCollisionSystem* Collision = World ? World->GetSubsystem<UNeonCollisionSystem>() : nullptr;
	if (!Collision)
	{
		return;
	}
	const float PickupRadius = GetConfig()->PickupRadius;

	for (int32 i = Pickups.Num() - 1; i >= 0; --i)
	{
		ANeonPowerupPickup* P = Pickups[i];
		if (!IsValid(P))
		{
			Pickups.RemoveAt(i);
			continue;
		}
		const FVector2D PickupPos = P->GetPosition2D();

		for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
		{
			UNeonMovementComponent* M = Weak.Get();
			if (!M || !M->IsAlive())
			{
				continue;
			}
			const float Reach = PickupRadius + M->GetConfig()->CollisionRadius;
			if ((M->GetSimPosition() - PickupPos).SizeSquared() > Reach * Reach)
			{
				continue;
			}
			// Same floor only: a bike driving under a deck leaves the pickup sitting on top of it.
			if (FMath::Abs(M->GetSimZ() - P->GetActorLocation().Z) > 220.f)
			{
				continue;
			}
			UNeonPowerupComponent* Inventory = M->GetPowerups();
			if (!Inventory || !Inventory->CanCollect())
			{
				continue; // slot full: the pickup stays for someone else (D7)
			}
			if (Inventory->Collect(P->GetPowerupType()))
			{
				Lifetimes.Remove(P);
				P->Destroy();
				Pickups.RemoveAt(i);
				break;
			}
		}
	}
}

bool UNeonPowerupSpawner::FindSpawnPoint(FVector2D& OutPoint, float& OutFloorZ)
{
	const ANeonArena* A = Arena.Get();
	UWorld* World = GetWorld();
	const UNeonCollisionSystem* Collision = World ? World->GetSubsystem<UNeonCollisionSystem>() : nullptr;
	if (!A || !Collision)
	{
		return false;
	}
	const UNeonPowerupConfig* C = GetConfig();

	// Lattice of authored points.
	const FBox2D Bounds = A->GetBounds();
	const float Inset = C->SpawnPointInset;
	const float Spacing = FMath::Max(C->SpawnPointSpacing, 200.f);
	const FVector2D Min = Bounds.Min + FVector2D(Inset, Inset);
	const FVector2D Max = Bounds.Max - FVector2D(Inset, Inset);
	if (Max.X <= Min.X || Max.Y <= Min.Y)
	{
		return false;
	}
	const int32 CountX = FMath::Max(1, FMath::FloorToInt((Max.X - Min.X) / Spacing) + 1);
	const int32 CountY = FMath::Max(1, FMath::FloorToInt((Max.Y - Min.Y) / Spacing) + 1);
	const FVector2D Origin = (Min + Max) * 0.5f - FVector2D((CountX - 1) * Spacing, (CountY - 1) * Spacing) * 0.5f;

	TArray<FVector2D> Candidates;
	Candidates.Reserve(CountX * CountY);
	for (int32 J = 0; J < CountY; ++J)
	{
		for (int32 I = 0; I < CountX; ++I)
		{
			Candidates.Add(Origin + FVector2D(I * Spacing, J * Spacing));
		}
	}

	// Shuffle with the seeded stream, then take the first valid point.
	for (int32 i = Candidates.Num() - 1; i > 0; --i)
	{
		Candidates.Swap(i, Rng.RandRange(0, i));
	}

	const float VehicleDistSq = C->MinDistanceFromVehicles * C->MinDistanceFromVehicles;
	const float PickupDistSq = C->MinDistanceBetweenPickups * C->MinDistanceBetweenPickups;

	for (const FVector2D& P : Candidates)
	{
		bool bOk = true;

		// Top-most floor at this point. Under a deck the pickup would be hidden, so the deck itself is the spawn surface.
		float TopZ = 0.f;
		bool bHasFloor = false;
		for (int32 Layer = A->GetHighestFloorLayer(); Layer >= 0; --Layer)
		{
			if (A->TryGetFloorHeightOnLayer(P, Layer, TopZ))
			{
				bHasFloor = true;
				break;
			}
		}
		if (!bHasFloor)
		{
			continue;
		}
		FNeonTrailHit Hit;
		const float SweepZ = TopZ + A->VehicleSpawnHeight;
		if (Collision->SweepAgainstTrails(P, P, C->MinDistanceFromTrails, INDEX_NONE, 0, Hit, SweepZ, SweepZ, 90.f))
		{
			continue; // inside or beside a trail
		}
		for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Collision->GetVehicles())
		{
			const UNeonMovementComponent* M = Weak.Get();
			if (M && M->IsAlive() && (M->GetSimPosition() - P).SizeSquared() < VehicleDistSq)
			{
				bOk = false;
				break;
			}
		}
		if (!bOk)
		{
			continue;
		}
		for (const ANeonPowerupPickup* Existing : Pickups)
		{
			if (IsValid(Existing) && (Existing->GetPosition2D() - P).SizeSquared() < PickupDistSq)
			{
				bOk = false;
				break;
			}
		}
		if (bOk)
		{
			OutPoint = P;
			OutFloorZ = TopZ;
			return true;
		}
	}
	return false;
}

void UNeonPowerupSpawner::TrySpawn()
{
	UWorld* World = GetWorld();
	const ANeonArena* A = Arena.Get();
	if (!World || !A || !PickupClass)
	{
		return;
	}

	const ENeonPowerupType Type = GetConfig()->PickRandomType(Rng);
	if (Type == ENeonPowerupType::None)
	{
		return;
	}

	FVector2D Point;
	float FloorZ = 0.f;
	if (!FindSpawnPoint(Point, FloorZ))
	{
		UE_LOG(LogNeonGrid, Verbose, TEXT("No clear powerup spawn point this attempt."));
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Location(Point.X, Point.Y, FloorZ);
	ANeonPowerupPickup* Pickup = World->SpawnActor<ANeonPowerupPickup>(PickupClass, FTransform(Location), Params);
	if (!Pickup)
	{
		return;
	}
	Pickup->InitPickup(Type);
	Pickups.Add(Pickup);
	Lifetimes.Add(Pickup, GetConfig()->PickupLifetime);
	UE_LOG(LogNeonGrid, Log, TEXT("Spawned %s pickup at (%.0f, %.0f). %d active."), *UEnum::GetValueAsString(Type), Point.X, Point.Y, Pickups.Num());
}
