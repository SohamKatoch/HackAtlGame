#include "Trail/NeonTrailComponent.h"

#include "Trail/NeonCollisionSystem.h"

UNeonTrailComponent::UNeonTrailComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UNeonTrailComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		Collision = World->GetSubsystem<UNeonCollisionSystem>();
	}
}

void UNeonTrailComponent::RecordPosition(const FVector2D& Point, float Z, float HeadingDegrees, int32 Tick)
{
	if (!bEmitting || !Collision)
	{
		return;
	}

	if (!bHasLastPoint)
	{
		bHasLastPoint = true;
		LastPoint = Point;
		LastZ = Z;
		CurrentSegment = INDEX_NONE;
		return;
	}

	if ((Point - LastPoint).SizeSquared() <= KINDA_SMALL_NUMBER && FMath::Abs(Z - LastZ) <= 1.f)
	{
		return;
	}

	bool bStartNew = CurrentSegment == INDEX_NONE;
	if (!bStartNew)
	{
		const float HeadingDelta = FMath::Abs(FRotator::NormalizeAxis(HeadingDegrees - CurrentSegmentHeading));
		const float Length = (Point - CurrentSegmentStart).Size();
		// A tall segment would be a straight smear through the jump. Break it so the laser follows the arc.
		const float HeightSpan = FMath::Abs(Z - CurrentSegmentStartZ);
		bStartNew = HeadingDelta > NewSegmentHeadingThreshold || Length > MaxSegmentLength || HeightSpan > 48.f;
	}

	// A Trail Cutter may have removed the segment being drawn; start a fresh one from the last point.
	if (!bStartNew && !Collision->ExtendSegment(CurrentSegment, Point, Z, Tick))
	{
		bStartNew = true;
	}

	if (bStartNew)
	{
		FNeonTrailSegment Seg;
		Seg.Start = LastPoint;
		Seg.End = Point;
		Seg.StartZ = LastZ;
		Seg.EndZ = Z;
		Seg.OwnerId = OwnerId;
		Seg.CreationTick = Tick;
		Seg.LastUpdateTick = Tick;

		CurrentSegment = Collision->AddSegment(Seg);
		CurrentSegmentStart = LastPoint;
		CurrentSegmentStartZ = LastZ;
		CurrentSegmentHeading = HeadingDegrees;
	}

	LastPoint = Point;
	LastZ = Z;
}

void UNeonTrailComponent::StopTrail()
{
	bEmitting = false;
}

void UNeonTrailComponent::ResetTrail()
{
	bEmitting = true;
	bHasLastPoint = false;
	CurrentSegment = INDEX_NONE;
	LastZ = 0.f;
}

void UNeonTrailComponent::GapTrail()
{
	bHasLastPoint = false;
	CurrentSegment = INDEX_NONE;
}
