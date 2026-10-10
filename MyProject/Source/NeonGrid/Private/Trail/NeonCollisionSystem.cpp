#include "Trail/NeonCollisionSystem.h"

#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehicleConfig.h"

bool UNeonCollisionSystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

// ---------------------------------------------------------------------------
// Trails
// ---------------------------------------------------------------------------

uint64 UNeonCollisionSystem::MakeCellKey(int32 CellX, int32 CellY)
{
	return (static_cast<uint64>(static_cast<uint32>(CellX)) << 32) | static_cast<uint32>(CellY);
}

void UNeonCollisionSystem::InsertIntoCells(int32 SegmentIndex, const FVector2D& A, const FVector2D& B)
{
	const float Inv = 1.f / FMath::Max(CellSize, 1.f);
	const int32 MinX = FMath::FloorToInt(FMath::Min(A.X, B.X) * Inv);
	const int32 MaxX = FMath::FloorToInt(FMath::Max(A.X, B.X) * Inv);
	const int32 MinY = FMath::FloorToInt(FMath::Min(A.Y, B.Y) * Inv);
	const int32 MaxY = FMath::FloorToInt(FMath::Max(A.Y, B.Y) * Inv);

	for (int32 X = MinX; X <= MaxX; ++X)
	{
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			Cells.FindOrAdd(MakeCellKey(X, Y)).AddUnique(SegmentIndex);
		}
	}
}

int32 UNeonCollisionSystem::AddSegment(const FNeonTrailSegment& Segment)
{
	const int32 Index = Segments.Add(Segment);
	InsertIntoCells(Index, Segment.Start, Segment.End);
	OnSegmentAdded.Broadcast(Index);
	return Index;
}

bool UNeonCollisionSystem::ExtendSegment(int32 Index, const FVector2D& NewEnd, int32 Tick)
{
	if (!Segments.IsValidIndex(Index) || Segments[Index].bRemoved)
	{
		return false;
	}

	FNeonTrailSegment& Seg = Segments[Index];
	const FVector2D OldEnd = Seg.End;
	Seg.End = NewEnd;
	Seg.LastUpdateTick = Tick;
	InsertIntoCells(Index, OldEnd, NewEnd);
	OnSegmentExtended.Broadcast(Index);
	return true;
}

void UNeonCollisionSystem::RemoveSegment(int32 Index)
{
	if (!Segments.IsValidIndex(Index) || Segments[Index].bRemoved)
	{
		return;
	}
	Segments[Index].bRemoved = true;
	OnSegmentRemoved.Broadcast(Index);
}

int32 UNeonCollisionSystem::CutTrailsInCircle(const FVector2D& Center, float Radius)
{
	if (Segments.Num() == 0 || Radius <= 0.f)
	{
		return 0;
	}

	// Gather first: splitting appends segments, which must not be revisited.
	TArray<int32> Touched;
	{
		const float Inv = 1.f / FMath::Max(CellSize, 1.f);
		const int32 MinX = FMath::FloorToInt((Center.X - Radius) * Inv);
		const int32 MaxX = FMath::FloorToInt((Center.X + Radius) * Inv);
		const int32 MinY = FMath::FloorToInt((Center.Y - Radius) * Inv);
		const int32 MaxY = FMath::FloorToInt((Center.Y + Radius) * Inv);
		const float RadiusSq = Radius * Radius;
		TSet<int32> Visited;
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				const TArray<int32>* Bucket = Cells.Find(MakeCellKey(X, Y));
				if (!Bucket)
				{
					continue;
				}
				for (int32 Index : *Bucket)
				{
					bool bAlready = false;
					Visited.Add(Index, &bAlready);
					if (bAlready || Segments[Index].bRemoved)
					{
						continue;
					}
					if (PointSegmentDistanceSquared(Center, Segments[Index].Start, Segments[Index].End) <= RadiusSq)
					{
						Touched.Add(Index);
					}
				}
			}
		}
	}

	for (int32 Index : Touched)
	{
		const FNeonTrailSegment Original = Segments[Index];
		RemoveSegment(Index);

		// Parametric interval of the segment inside the circle: |A + t D - C|^2 = R^2.
		const FVector2D D = Original.End - Original.Start;
		const FVector2D F = Original.Start - Center;
		const float A = FVector2D::DotProduct(D, D);
		if (A <= KINDA_SMALL_NUMBER)
		{
			continue; // degenerate; it is inside the circle, gone
		}
		const float B = 2.f * FVector2D::DotProduct(F, D);
		const float Cc = FVector2D::DotProduct(F, F) - Radius * Radius;
		const float Disc = B * B - 4.f * A * Cc;
		if (Disc < 0.f)
		{
			continue; // should not happen for a touched segment; treat as fully removed
		}
		const float SqrtDisc = FMath::Sqrt(Disc);
		const float T1 = FMath::Clamp((-B - SqrtDisc) / (2.f * A), 0.f, 1.f);
		const float T2 = FMath::Clamp((-B + SqrtDisc) / (2.f * A), 0.f, 1.f);

		constexpr float MinPieceLength = 2.f;
		if (T1 > 0.f)
		{
			FNeonTrailSegment Piece = Original;
			Piece.bRemoved = false;
			Piece.End = Original.Start + D * T1;
			if ((Piece.End - Piece.Start).Size() >= MinPieceLength)
			{
				AddSegment(Piece);
			}
		}
		if (T2 < 1.f)
		{
			FNeonTrailSegment Piece = Original;
			Piece.bRemoved = false;
			Piece.Start = Original.Start + D * T2;
			if ((Piece.End - Piece.Start).Size() >= MinPieceLength)
			{
				AddSegment(Piece);
			}
		}
	}

	return Touched.Num();
}

void UNeonCollisionSystem::ClearAllTrails()
{
	Segments.Reset();
	Cells.Reset();
	OnTrailsCleared.Broadcast();
}

float UNeonCollisionSystem::PointSegmentDistanceSquared(const FVector2D& P, const FVector2D& A, const FVector2D& B)
{
	const FVector2D AB = B - A;
	const float LenSq = AB.SizeSquared();
	if (LenSq <= KINDA_SMALL_NUMBER)
	{
		return (P - A).SizeSquared();
	}
	const float T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.f, 1.f);
	return (P - (A + AB * T)).SizeSquared();
}

float UNeonCollisionSystem::SegmentSegmentDistanceSquared(const FVector2D& P1, const FVector2D& Q1, const FVector2D& P2, const FVector2D& Q2)
{
	// Proper intersection test first.
	auto Cross = [](const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; };
	const FVector2D D1 = Q1 - P1;
	const FVector2D D2 = Q2 - P2;
	const float Denom = Cross(D1, D2);
	if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
	{
		const FVector2D R = P2 - P1;
		const float T = Cross(R, D2) / Denom;
		const float U = Cross(R, D1) / Denom;
		if (T >= 0.f && T <= 1.f && U >= 0.f && U <= 1.f)
		{
			return 0.f;
		}
	}

	// Otherwise the closest approach is at an endpoint.
	float Best = PointSegmentDistanceSquared(P1, P2, Q2);
	Best = FMath::Min(Best, PointSegmentDistanceSquared(Q1, P2, Q2));
	Best = FMath::Min(Best, PointSegmentDistanceSquared(P2, P1, Q1));
	Best = FMath::Min(Best, PointSegmentDistanceSquared(Q2, P1, Q1));
	return Best;
}

bool UNeonCollisionSystem::SweepAgainstTrails(const FVector2D& From, const FVector2D& To, float Radius,
	int32 SelfOwnerId, int32 SelfIgnoreAfterTick, FNeonTrailHit& OutHit) const
{
	if (Segments.Num() == 0)
	{
		return false;
	}

	const float Inv = 1.f / FMath::Max(CellSize, 1.f);
	const int32 MinX = FMath::FloorToInt((FMath::Min(From.X, To.X) - Radius) * Inv);
	const int32 MaxX = FMath::FloorToInt((FMath::Max(From.X, To.X) + Radius) * Inv);
	const int32 MinY = FMath::FloorToInt((FMath::Min(From.Y, To.Y) - Radius) * Inv);
	const int32 MaxY = FMath::FloorToInt((FMath::Max(From.Y, To.Y) + Radius) * Inv);

	const float RadiusSq = Radius * Radius;
	TSet<int32> Visited;

	for (int32 X = MinX; X <= MaxX; ++X)
	{
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			const TArray<int32>* Bucket = Cells.Find(MakeCellKey(X, Y));
			if (!Bucket)
			{
				continue;
			}

			for (int32 Index : *Bucket)
			{
				bool bAlreadyVisited = false;
				Visited.Add(Index, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}

				const FNeonTrailSegment& Seg = Segments[Index];
				if (Seg.bRemoved)
				{
					continue;
				}
				if (Seg.OwnerId == SelfOwnerId && Seg.LastUpdateTick >= SelfIgnoreAfterTick)
				{
					continue;
				}

				if (SegmentSegmentDistanceSquared(From, To, Seg.Start, Seg.End) <= RadiusSq)
				{
					OutHit.SegmentIndex = Index;
					OutHit.OwnerId = Seg.OwnerId;
					return true;
				}
			}
		}
	}

	return false;
}

bool UNeonCollisionSystem::FindFirstTrailAlongRay(const FVector2D& From, const FVector2D& Direction, float MaxDistance, float Radius,
	int32 SelfOwnerId, int32 SelfIgnoreAfterTick, FVector2D& OutPoint, FNeonTrailHit& OutHit) const
{
	const FVector2D Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero() || MaxDistance <= 0.f)
	{
		return false;
	}

	const float StepLength = FMath::Max(Radius, 25.f);
	const int32 Steps = FMath::CeilToInt(MaxDistance / StepLength);
	FVector2D Prev = From;
	for (int32 i = 1; i <= Steps; ++i)
	{
		const FVector2D Next = From + Dir * FMath::Min(i * StepLength, MaxDistance);
		if (SweepAgainstTrails(Prev, Next, Radius, SelfOwnerId, SelfIgnoreAfterTick, OutHit))
		{
			OutPoint = Next;
			return true;
		}
		Prev = Next;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Vehicles
// ---------------------------------------------------------------------------

void UNeonCollisionSystem::RegisterVehicle(UNeonMovementComponent* Vehicle)
{
	if (Vehicle)
	{
		Vehicles.AddUnique(Vehicle);
	}
}

void UNeonCollisionSystem::UnregisterVehicle(UNeonMovementComponent* Vehicle)
{
	Vehicles.RemoveAll([Vehicle](const TWeakObjectPtr<UNeonMovementComponent>& V) { return !V.IsValid() || V.Get() == Vehicle; });
}

UNeonMovementComponent* UNeonCollisionSystem::FindVehicleOverlap(const UNeonMovementComponent* Self, const FVector2D& Position, float Radius) const
{
	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Vehicles)
	{
		UNeonMovementComponent* Other = Weak.Get();
		if (!Other || Other == Self || !Other->IsAlive())
		{
			continue;
		}

		const float Combined = Radius + Other->GetConfig()->CollisionRadius;
		if ((Other->GetSimPosition() - Position).SizeSquared() <= Combined * Combined)
		{
			return Other;
		}
	}
	return nullptr;
}

int32 UNeonCollisionSystem::CountAliveVehicles() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UNeonMovementComponent>& Weak : Vehicles)
	{
		if (Weak.IsValid() && Weak->IsAlive())
		{
			++Count;
		}
	}
	return Count;
}
