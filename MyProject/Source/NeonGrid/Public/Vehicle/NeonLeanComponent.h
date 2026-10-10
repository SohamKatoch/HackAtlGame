#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "NeonLeanComponent.generated.h"

class UNeonMovementComponent;

/**
 * Cosmetic lean. Docs/VEHICLE_SPEC.md section 4.
 *
 * Attach the visual meshes under this component. It rolls itself toward the steer direction
 * using the lean curve in the vehicle config and eases back to neutral. The pawn root and the
 * gameplay collision radius are never affected.
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonLeanComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UNeonLeanComponent();

	/**
	 * +1 rolls so a right turn drops the right side. Flip to -1 if the placeholder mesh reads
	 * as leaning out of the turn on the test machine.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Lean", meta = (ClampMin = "-1", ClampMax = "1"))
	float LeanDirection = 1.f;

	UFUNCTION(BlueprintPure, Category = "Neon|Lean")
	float GetCurrentLeanAngle() const { return CurrentLean; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UNeonMovementComponent> Movement;

	float CurrentLean = 0.f;
};
