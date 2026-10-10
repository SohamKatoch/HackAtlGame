#include "Trail/NeonTrailRendererComponent.h"

#include "Arena/NeonArena.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Trail/NeonCollisionSystem.h"
#include "UObject/ConstructorHelpers.h"

UNeonTrailRendererComponent::UNeonTrailRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;

	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);

	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCastShadow(false);
	SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		SetStaticMesh(CubeFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		SetMaterial(0, MaterialFinder.Object);
	}
}

void UNeonTrailRendererComponent::SetTrailColor(const FLinearColor& InColor)
{
	if (!GetMaterial(0))
	{
		return;
	}
	if (UMaterialInstanceDynamic* MID = CreateAndSetMaterialInstanceDynamic(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), InColor);
	}
}

void UNeonTrailRendererComponent::BeginPlay()
{
	Super::BeginPlay();

	// Pin to world origin regardless of the owning pawn.
	SetWorldTransform(FTransform::Identity);

	if (UWorld* World = GetWorld())
	{
		Collision = World->GetSubsystem<UNeonCollisionSystem>();
		Arena = ANeonArena::FindArena(World);
	}
	if (Collision)
	{
		AddedHandle = Collision->OnSegmentAdded.AddUObject(this, &UNeonTrailRendererComponent::HandleSegmentAdded);
		ExtendedHandle = Collision->OnSegmentExtended.AddUObject(this, &UNeonTrailRendererComponent::HandleSegmentExtended);
		RemovedHandle = Collision->OnSegmentRemoved.AddUObject(this, &UNeonTrailRendererComponent::HandleSegmentRemoved);
		ClearedHandle = Collision->OnTrailsCleared.AddUObject(this, &UNeonTrailRendererComponent::HandleTrailsCleared);
	}
}

void UNeonTrailRendererComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Collision)
	{
		Collision->OnSegmentAdded.Remove(AddedHandle);
		Collision->OnSegmentExtended.Remove(ExtendedHandle);
		Collision->OnSegmentRemoved.Remove(RemovedHandle);
		Collision->OnTrailsCleared.Remove(ClearedHandle);
	}
	Super::EndPlay(EndPlayReason);
}

FTransform UNeonTrailRendererComponent::MakeChunkTransform(const FVector2D& A, const FVector2D& B, float ZA, float ZB) const
{
	const FVector2D Delta = B - A;
	const float Length = Delta.Size();
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
	const FVector2D Mid = (A + B) * 0.5f;

	// ZA/ZB are vehicle-center heights, including a jump. The ribbon is centered on that line.
	const float Pitch = Length > KINDA_SMALL_NUMBER ? FMath::RadiansToDegrees(FMath::Atan2(ZB - ZA, Length)) : 0.f;
	const float SlopeLength = FMath::Sqrt(Length * Length + (ZB - ZA) * (ZB - ZA));

	// Engine cube is 100 cm. Overlap each end by half the thickness so joints do not show gaps.
	const FVector Scale((SlopeLength + VisualThickness) / 100.f, VisualThickness / 100.f, VisualHeight / 100.f);
	const FRotator Rot(Pitch, Yaw, 0.f);
	const FVector Location(Mid.X, Mid.Y, (ZA + ZB) * 0.5f);
	return FTransform(Rot, Location, Scale);
}

void UNeonTrailRendererComponent::SyncSegmentChunks(int32 SegmentIndex, const FVector2D& A, const FVector2D& B, float ZA, float ZB)
{
	TArray<int32>& Instances = SegmentToInstances.FindOrAdd(SegmentIndex);

	const FVector2D Delta = B - A;
	const float Length = Delta.Size();
	const float Chunk = FMath::Max(ChunkLength, 50.f);
	const int32 Wanted = FMath::Max(1, FMath::CeilToInt(Length / Chunk));
	const FVector2D Dir = Length > KINDA_SMALL_NUMBER ? Delta / Length : FVector2D(1.f, 0.f);

	// Full chunks before the last one never move once laid down: only the last existing chunk
	// (previously partial) is refreshed, then new chunks are appended.
	const int32 FirstToTouch = FMath::Max(0, Instances.Num() - 1);
	for (int32 I = FirstToTouch; I < Wanted; ++I)
	{
		const float DistA = I * Chunk;
		const float DistB = (I == Wanted - 1) ? Length : (I + 1) * Chunk;
		const float TA = Length > KINDA_SMALL_NUMBER ? DistA / Length : 0.f;
		const float TB = Length > KINDA_SMALL_NUMBER ? DistB / Length : 1.f;
		const FVector2D CA = A + Dir * DistA;
		const FVector2D CB = (I == Wanted - 1) ? B : A + Dir * DistB;
		const FTransform T = MakeChunkTransform(CA, CB, FMath::Lerp(ZA, ZB, TA), FMath::Lerp(ZA, ZB, TB));
		if (I < Instances.Num())
		{
			UpdateInstanceTransform(Instances[I], T, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
		}
		else
		{
			Instances.Add(AddInstance(T, /*bWorldSpace*/ true));
		}
	}
	bDirty = true;
}

void UNeonTrailRendererComponent::HandleSegmentAdded(int32 SegmentIndex)
{
	if (!Collision)
	{
		return;
	}
	const TArray<FNeonTrailSegment>& Segments = Collision->GetSegments();
	if (!Segments.IsValidIndex(SegmentIndex) || Segments[SegmentIndex].OwnerId != OwnerId)
	{
		return;
	}

	const FNeonTrailSegment& Seg = Segments[SegmentIndex];
	SyncSegmentChunks(SegmentIndex, Seg.Start, Seg.End, Seg.StartZ, Seg.EndZ);
}

void UNeonTrailRendererComponent::HandleSegmentExtended(int32 SegmentIndex)
{
	if (!Collision || !SegmentToInstances.Contains(SegmentIndex))
	{
		return;
	}
	const TArray<FNeonTrailSegment>& Segments = Collision->GetSegments();
	if (!Segments.IsValidIndex(SegmentIndex))
	{
		return;
	}

	const FNeonTrailSegment& Seg = Segments[SegmentIndex];
	SyncSegmentChunks(SegmentIndex, Seg.Start, Seg.End, Seg.StartZ, Seg.EndZ);
}

void UNeonTrailRendererComponent::HandleSegmentRemoved(int32 SegmentIndex)
{
	// Removing ISM instances renumbers the rest; collapse them instead so every index stays valid.
	TArray<int32> Instances;
	if (SegmentToInstances.RemoveAndCopyValue(SegmentIndex, Instances))
	{
		const FTransform Hidden(FRotator::ZeroRotator, FVector(0.f, 0.f, -100000.f), FVector(0.001f));
		for (int32 Instance : Instances)
		{
			UpdateInstanceTransform(Instance, Hidden, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
		}
		bDirty = true;
	}
}

void UNeonTrailRendererComponent::HandleTrailsCleared()
{
	ClearInstances();
	SegmentToInstances.Reset();
	bDirty = true;
}

void UNeonTrailRendererComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// One render-state refresh per frame no matter how many segments changed.
	if (bDirty)
	{
		MarkRenderStateDirty();
		bDirty = false;
	}
}
