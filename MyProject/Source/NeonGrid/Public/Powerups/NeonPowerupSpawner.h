#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Powerups/NeonPowerupTypes.h"
#include "NeonPowerupSpawner.generated.h"

class ANeonArena;
class ANeonPowerupPickup;
class UNeonPowerupConfig;
class UNeonCollisionSystem;

/**
 * Authority-side powerup spawner and pickup resolver. Docs/TECHNICAL_SPEC.md section 7. Lives on the
 * game mode (server / offline authority) and does nothing on clients.
 *
 * Every SpawnInterval, while below MaxActivePowerups and the round is Playing, pick a random
 * lattice point in the arena that is clear of trails, vehicles and other pickups, and drop a
 * random type there. Each tick, any alive vehicle inside a pickup's radius whose slot is empty
 * collects it. Pickups expire after PickupLifetime. Everything is cleared at round reset.
 *
 * Randomness is a seeded FRandomStream so a solo match replays the same spawns for the same seed.
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonPowerupSpawner : public UActorComponent
{
	GENERATED_BODY()

public:
	UNeonPowerupSpawner();

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Powerups")
	TSubclassOf<ANeonPowerupPickup> PickupClass;

	/** Arena, config, on/off switch and seed for the match about to start. */
	void Configure(ANeonArena* InArena, UNeonPowerupConfig* InConfig, bool bInEnabled, int32 Seed);

	/** Spawning and collection run only while active (the Playing phase). */
	void SetSpawning(bool bInActive);

	/** Destroy every pickup and restart the spawn timer (round reset, return to menu). */
	void ClearPickups();

	const TArray<TObjectPtr<ANeonPowerupPickup>>& GetPickups() const { return Pickups; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	const UNeonPowerupConfig* GetConfig() const;
	void TrySpawn();
	/** Picks a clear point and the Z of the top-most floor there, so pickups sit on upper decks instead of under them. */
	bool FindSpawnPoint(FVector2D& OutPoint, float& OutFloorZ);
	void ResolvePickups();
	void ExpirePickups(float DeltaTime);

	TWeakObjectPtr<ANeonArena> Arena;

	UPROPERTY(Transient)
	TObjectPtr<UNeonPowerupConfig> Config;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ANeonPowerupPickup>> Pickups;

	TMap<TWeakObjectPtr<ANeonPowerupPickup>, float> Lifetimes;

	FRandomStream Rng;
	bool bEnabled = false;
	bool bActive = false;
	float SpawnTimer = 0.f;
};
