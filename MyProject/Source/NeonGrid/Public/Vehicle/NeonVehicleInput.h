#pragma once

#include "CoreMinimal.h"
#include "NeonVehicleInput.generated.h"

/**
 * One tick of vehicle input.
 *
 * This is the only thing that drives a vehicle. A human player controller fills it from
 * Enhanced Input, an AI controller fills it from its decision, and (later) a LAN client
 * sends it to the host. The movement component never knows where it came from.
 *
 * See Docs/VEHICLE_SPEC.md section 3 and Docs/MULTIPLAYER_SPEC.md section 3.
 */
USTRUCT(BlueprintType)
struct NEONGRID_API FNeonVehicleInput
{
	GENERATED_BODY()

	/** -1 = full left, +1 = full right, 0 = hold heading. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Input", meta = (ClampMin = "-1", ClampMax = "1"))
	float Steering = 0.f;

	/**
	 * Manual powerup activation (Space / right trigger).
	 * Part of the snapshot shape from the spec. Nothing consumes it until the powerup milestone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Input")
	bool bActivatePowerup = false;
};
