#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Powerups/NeonPowerupTypes.h"
#include "NeonPowerupPickup.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;

/**
 * A powerup lying in the arena. Pure presentation plus a type: the spawner owns its lifetime and
 * decides who collected it (TECHNICAL_SPEC section 7: "the host decides location, type, time, and
 * pickup result"). Placeholder look: a tinted sphere hovering over a tall faint beacon so it reads
 * from the low follow camera across the arena. Collision is handled in 2D by the spawner, not by
 * physics overlaps.
 */
UCLASS()
class NEONGRID_API ANeonPowerupPickup : public AActor
{
	GENERATED_BODY()

public:
	ANeonPowerupPickup();

	UFUNCTION(BlueprintPure, Category = "Neon|Powerups")
	ENeonPowerupType GetPowerupType() const { return Type; }

	/** World XY of the pickup (the actor location). */
	FVector2D GetPosition2D() const { const FVector L = GetActorLocation(); return FVector2D(L.X, L.Y); }

	/** Server: set the type and apply its color. */
	void InitPickup(ENeonPowerupType InType);

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Powerups")
	float HoverHeight = 90.f;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Powerups")
	float BobAmplitude = 18.f;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Powerups")
	float SpinDegreesPerSecond = 90.f;

	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnRep_Type();

	UPROPERTY(ReplicatedUsing = OnRep_Type)
	ENeonPowerupType Type = ENeonPowerupType::None;

	UPROPERTY(VisibleAnywhere, Category = "Neon|Powerups")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Neon|Powerups")
	TObjectPtr<UStaticMeshComponent> Orb;

	UPROPERTY(VisibleAnywhere, Category = "Neon|Powerups")
	TObjectPtr<UStaticMeshComponent> Beacon;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	void ApplyColor();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlaceholderMaterial;

	float Age = 0.f;
};
