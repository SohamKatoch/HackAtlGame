#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NeonTrailComponent.generated.h"

class UNeonCollisionSystem;

/**
 * Emits this vehicle's trail into the collision system. Docs/TECHNICAL_SPEC.md section 6.
 *
 * Driven by the movement component after each simulation step (not by its own tick, so the
 * ordering is exact). Straight driving extends the current segment; a heading change beyond
 * the threshold, or a long segment, starts a new one. That keeps segment counts and future
 * network traffic low without affecting collision accuracy.
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonTrailComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNeonTrailComponent();

	/** Start a new segment once the heading has drifted this far (degrees) from the current segment's heading. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Trail", meta = (ClampMin = "0.5", ClampMax = "30"))
	float NewSegmentHeadingThreshold = 3.f;

	/** Also start a new segment once the current one is this long (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Trail", meta = (ClampMin = "50"))
	float MaxSegmentLength = 400.f;

	void SetOwnerId(int32 InOwnerId) { OwnerId = InOwnerId; }
	int32 GetOwnerId() const { return OwnerId; }

	/** Called by the movement component after each step with the trail emission point (rear of vehicle). */
	/** Z is the vehicle center. A change in height starts a new segment so a jump is a polyline, not a smear. */
	void RecordPosition(const FVector2D& Point, float Z, float HeadingDegrees, int32 Tick);

	/** Stop emitting (death). The existing trail stays. */
	void StopTrail();

	/** Forget the current segment and start fresh on the next RecordPosition (respawn / round reset). */
	void ResetTrail();

	/**
	 * Break the ribbon without disabling emission. The next RecordPosition starts a new segment
	 * and does not bridge the gap. The movement step calls this on takeoff and landing so a jump
	 * does not draw a trail through the air (DECISIONS.md D29).
	 */
	void GapTrail();

	bool IsEmitting() const { return bEmitting; }

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UNeonCollisionSystem> Collision;

	int32 OwnerId = INDEX_NONE;
	bool bEmitting = true;

	bool bHasLastPoint = false;
	FVector2D LastPoint = FVector2D::ZeroVector;
	float LastZ = 0.f;

	int32 CurrentSegment = INDEX_NONE;
	FVector2D CurrentSegmentStart = FVector2D::ZeroVector;
	float CurrentSegmentStartZ = 0.f;
	float CurrentSegmentHeading = 0.f;
};
