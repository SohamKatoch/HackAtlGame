#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Trail/NeonTrailTypes.h"
#include "NeonCollisionSystem.generated.h"

class UNeonMovementComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(FNeonSegmentEvent, int32 /*SegmentIndex*/);

/**
 * Trail registry, spatial hash, and sweep queries. Docs/TECHNICAL_SPEC.md section 6.
 *
 * One per world. Holds every trail segment as data plus a uniform grid hash of segment indices.
 * Vehicles query it with a thickened sweep each simulation step. It also tracks live vehicles
 * for vehicle-versus-vehicle contact.
 *
 * This is simulation state. On a LAN host it is authoritative; clients will mirror it from
 * replicated segment events (Milestone 6). Renderers subscribe to the events below.
 */
UCLASS()
class NEONGRID_API UNeonCollisionSystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// ---- Trails ----

	/** Add a segment. Returns its index. */
	int32 AddSegment(const FNeonTrailSegment& Segment);

	/**
	 * Move a segment's End forward (straight driving extends instead of adding).
	 * Returns false if the segment no longer exists (cut away); the caller should start a new one.
	 */
	bool ExtendSegment(int32 Index, const FVector2D& NewEnd, float NewEndZ, int32 Tick);

	/** Mark one segment removed. Queries skip it; renderers get OnSegmentRemoved. */
	void RemoveSegment(int32 Index);

	/**
	 * Trail Cutter: delete every piece of trail (any owner) inside the circle. Segments that cross the
	 * circle are split; the parts outside survive as new segments with the original owner and ticks.
	 * Returns the number of segments touched.
	 */
	int32 CutTrailsInCircle(const FVector2D& Center, float Radius);

	/** Remove every segment (round reset). */
	void ClearAllTrails();

	/** Includes removed segments (check bRemoved). */
	const TArray<FNeonTrailSegment>& GetSegments() const { return Segments; }

	/**
	 * Thickened sweep From->To. Own segments still being drawn are skipped.
	 * VerticalRadius < 0 ignores height. Otherwise the laser only hits when its Z at the closest
	 * point is within VerticalRadius of the sweeper's Z.
	 */
	bool SweepAgainstTrails(const FVector2D& From, const FVector2D& To, float Radius,
		int32 SelfOwnerId, int32 SelfIgnoreAfterTick, FNeonTrailHit& OutHit,
		float FromZ = 0.f, float ToZ = 0.f, float VerticalRadius = -1.f) const;

	/**
	 * March a ray From toward Direction up to MaxDistance and return the first point at which a
	 * thickened step touches a trail. For aiming the Trail Cutter. Same own-trail grace as a sweep.
	 */
	bool FindFirstTrailAlongRay(const FVector2D& From, const FVector2D& Direction, float MaxDistance, float Radius,
		int32 SelfOwnerId, int32 SelfIgnoreAfterTick, FVector2D& OutPoint, FNeonTrailHit& OutHit,
		float FromZ = 0.f, float VerticalRadius = -1.f) const;

	// ---- Vehicles ----

	void RegisterVehicle(UNeonMovementComponent* Vehicle);
	void UnregisterVehicle(UNeonMovementComponent* Vehicle);

	/** First other alive vehicle whose radius overlaps a circle at Position. */
	UNeonMovementComponent* FindVehicleOverlap(const UNeonMovementComponent* Self, const FVector2D& Position, float Radius) const;

	const TArray<TWeakObjectPtr<UNeonMovementComponent>>& GetVehicles() const { return Vehicles; }

	int32 CountAliveVehicles() const;

	// ---- Events (renderers, later replication) ----

	FNeonSegmentEvent OnSegmentAdded;
	FNeonSegmentEvent OnSegmentExtended;
	FNeonSegmentEvent OnSegmentRemoved;
	FSimpleMulticastDelegate OnTrailsCleared;

	/** Spatial hash cell size in cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Collision", meta = (ClampMin = "50"))
	float CellSize = 400.f;

	// ---- Geometry helpers (shared with AI prediction) ----

	/** OutT is the parameter on P1-Q1, OutU on P2-Q2, both 0..1 at the closest approach. */
	static float SegmentSegmentDistanceSquared(const FVector2D& P1, const FVector2D& Q1, const FVector2D& P2, const FVector2D& Q2,
		float* OutT = nullptr, float* OutU = nullptr);
	static float PointSegmentDistanceSquared(const FVector2D& P, const FVector2D& A, const FVector2D& B);

private:
	static uint64 MakeCellKey(int32 CellX, int32 CellY);
	void InsertIntoCells(int32 SegmentIndex, const FVector2D& A, const FVector2D& B);

	TArray<FNeonTrailSegment> Segments;
	TMap<uint64, TArray<int32>> Cells;
	TArray<TWeakObjectPtr<UNeonMovementComponent>> Vehicles;
};
