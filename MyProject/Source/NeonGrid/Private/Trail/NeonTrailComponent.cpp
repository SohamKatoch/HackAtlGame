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

void UNeonTrailComponent::RecordPosition(const FVector2D& Point, float HeadingDegrees, int32 Tick)
{
	if (!bEmitting || !Collision)
	{
		return;
	}

	if (!bHasLastPoint)
	{
		bHasLastPoint = true;
		LastPoint = Point;
		CurrentSegment = INDEX_NONE;
		return;
	}

	if ((Point - LastPoint).SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	bool bStartNew = CurrentSegment == INDEX_NONE;
	if (!bStartNew)
	{
		const float HeadingDelta = FMath::Abs(FRotator::NormalizeAxis(HeadingDegrees - CurrentSegmentHeading));
		const float Length = (Point - CurrentSegmentStart).Size();
		bStartNew = HeadingDelta > NewSegmentHeadingThreshold || Length > MaxSegmentLength;
	}

	// A Trail Cutter may have removed the segment being drawn; start a fresh one from the last point.
	if (!bStartNew && !Collision->ExtendSegment(CurrentSegment, Point, Tick))
	{
		bStartNew = true;
	}

	if (bStartNew)
	{
		FNeonTrailSegment Seg;
		Seg.Start = LastPoint;
		Seg.End = Point;
		Seg.OwnerId = OwnerId;
		Seg.CreationTick = Tick;
		Seg.LastUpdateTick = Tick;

		CurrentSegment = Collision->AddSegment(Seg);
		CurrentSegmentStart = LastPoint;
		CurrentSegmentHeading = HeadingDegrees;
	}

	LastPoint = Point;
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
}
