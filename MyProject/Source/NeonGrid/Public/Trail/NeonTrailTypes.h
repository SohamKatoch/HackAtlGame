#pragma once

#include "CoreMinimal.h"
#include "NeonTrailTypes.generated.h"

/**
 * One trail segment. Docs/GAME_DESIGN.md section 7, Docs/TECHNICAL_SPEC.md section 6.
 * Pure data. Never a physics body. Rendering is a separate concern.
 */
USTRUCT(BlueprintType)
struct NEONGRID_API FNeonTrailSegment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	FVector2D Start = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	FVector2D End = FVector2D::ZeroVector;

	/** World Z of the vehicle center when this end was laid. A jump leaves the laser in the air. */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	float StartZ = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	float EndZ = 0.f;

	/** Player slot that drew it. */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	int32 OwnerId = INDEX_NONE;

	/** Simulation tick when the segment was started. */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	int32 CreationTick = 0;

	/** Simulation tick when End was last moved. Used for the owner's self-collision grace window. */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	int32 LastUpdateTick = 0;

	/**
	 * Cut away (Trail Cutter). Removed segments stay in the array so indices remain stable for renderers
	 * and replication; every query skips them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	bool bRemoved = false;
};

USTRUCT(BlueprintType)
struct NEONGRID_API FNeonTrailHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	int32 SegmentIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Trail")
	int32 OwnerId = INDEX_NONE;
};
