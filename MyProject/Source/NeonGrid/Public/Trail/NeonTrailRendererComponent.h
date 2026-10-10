#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "NeonTrailRendererComponent.generated.h"

class UNeonCollisionSystem;
class ANeonArena;

/**
 * Cosmetic trail for one owner. Docs/GAME_DESIGN.md section 7 ("Look versus collision").
 *
 * Mirrors the collision system's segments for a given OwnerId as scaled box instances. Visual
 * thickness and height are presentation only; collision uses the segment math. Uses absolute
 * transforms so it does not move with the pawn it is attached to.
 *
 * Terrain (DECISIONS.md D25): segments are 2D. Each one is drawn as a run of chunks no longer than
 * ChunkLength, each chunk pitched to sit on the arena floor between its two end heights. Chunks
 * are only ever appended (segments grow, never shrink), so instance indices stay stable.
 *
 * Placeholder look (tinted engine cube). The neon ribbon/glow pass is Milestone 8 and can
 * replace this class without touching gameplay.
 */
UCLASS(ClassGroup = (NeonGrid), meta = (BlueprintSpawnableComponent))
class NEONGRID_API UNeonTrailRendererComponent : public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UNeonTrailRendererComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Trail", meta = (ClampMin = "1"))
	float VisualThickness = 24.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Trail", meta = (ClampMin = "1"))
	float VisualHeight = 110.f;

	/** Longest box drawn for one stretch of trail, cm. Shorter follows ramps more closely; costs instances. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Trail", meta = (ClampMin = "50"))
	float ChunkLength = 200.f;

	void SetOwnerId(int32 InOwnerId) { OwnerId = InOwnerId; }
	void SetTrailColor(const FLinearColor& InColor);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void HandleSegmentAdded(int32 SegmentIndex);
	void HandleSegmentExtended(int32 SegmentIndex);
	void HandleSegmentRemoved(int32 SegmentIndex);
	void HandleTrailsCleared();
	/** Add or update the chunk instances so they cover the segment A->B. */
	void SyncSegmentChunks(int32 SegmentIndex, const FVector2D& A, const FVector2D& B);
	FTransform MakeChunkTransform(const FVector2D& A, const FVector2D& B) const;
	float FloorHeightAt(const FVector2D& P) const;

	UPROPERTY(Transient)
	TObjectPtr<UNeonCollisionSystem> Collision;

	TWeakObjectPtr<const ANeonArena> Arena;

	/** Segment index -> instance indices, in order along the segment. */
	TMap<int32, TArray<int32>> SegmentToInstances;
	int32 OwnerId = INDEX_NONE;
	bool bDirty = false;
	FDelegateHandle AddedHandle;
	FDelegateHandle ExtendedHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle ClearedHandle;
};
