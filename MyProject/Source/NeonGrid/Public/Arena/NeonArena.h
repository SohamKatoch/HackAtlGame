#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NeonArena.generated.h"

class UStaticMeshComponent;
class UProceduralMeshComponent;
class UCameraComponent;
class UDirectionalLightComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * THE GRID. Docs/GAME_DESIGN.md section 8, Docs/TECHNICAL_SPEC.md section 4.
 *
 * Owns the gameplay rectangle, four inward-facing spawn points, a fixed whole-arena camera, and
 * cosmetic terrain/wall meshes. The meshes have no collision; the movement step uses GetBounds().
 *
 * Terrain (DECISIONS.md D25): a 2.5D stepped pyramid. The simulation is still 2D in X/Y; Z is
 * derived from GetFloorHeight(), which is continuous (plateaus joined by ramp bands), so height
 * never changes who hits what. Vehicles, trails and the camera read the height for presentation.
 *
 * Collapse (DECISIONS.md D27): the playable rectangle shrinks toward the center during a round.
 * After CollapseStartDelay seconds of play the half extents interpolate linearly from HalfExtents to
 * MinHalfExtents over CollapseDuration. GetBounds() returns the current rectangle; the movement step
 * reads it every tick, so a vehicle caught by the moving wall is eliminated (Boundary). The game mode
 * drives SetRoundElapsed(); the AI asks GetBoundsAt() for where the wall will be.
 *
 * The whole-arena camera is the main menu backdrop and a future spectator view; during play each
 * player views their vehicle's follow camera (DECISIONS.md D21).
 *
 * The actor is expected to be unrotated. Yaw is ignored by the bounds math.
 */
UCLASS()
class NEONGRID_API ANeonArena : public AActor
{
	GENERATED_BODY()

public:
	ANeonArena();

	/**
	 * Half size of the playable rectangle at round start, cm (X = long axis). 24000 x 16000 total
	 * (DECISIONS.md D27). At 1400 cm/s a vehicle crosses the long axis in about 17 s.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Arena", meta = (ClampMin = "100"))
	FVector2D HalfExtents = FVector2D(12000.f, 8000.f);

	// ---- Collapse (D27): the walls close in during the round ----

	/** Master switch. The match settings can also turn it off per match. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Collapse")
	bool bCollapseEnabled = true;

	/** Seconds of play before the walls start moving. Short so a 4-bike round actually sees them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Collapse", meta = (ClampMin = "0"))
	float CollapseStartDelay = 8.f;

	/** Seconds the walls take to move from HalfExtents to MinHalfExtents. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Collapse", meta = (ClampMin = "1"))
	float CollapseDuration = 40.f;

	/** Final half size, cm. Default is roughly the top two tiers of the pyramid. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Collapse", meta = (ClampMin = "100"))
	FVector2D MinHalfExtents = FVector2D(4000.f, 2600.f);

	/** Wall tint while the walls are moving. Pulses against WallColor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Collapse")
	FLinearColor CollapseWallColor = FLinearColor(1.0f, 0.35f, 0.08f);

	/** Z offset above the floor surface for spawned vehicles. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	float VehicleSpawnHeight = 20.f;

	/** Spawn inset as a fraction of HalfExtents (0.65 = 65% toward the edge). Default puts spawns on the first plateau. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Arena", meta = (ClampMin = "0.1", ClampMax = "0.95"))
	FVector2D SpawnInsetFraction = FVector2D(0.65f, 0.6f);

	// ---- Terrain: stepped tiers (D25). All cosmetic; the sim is flat. ----

	/** Number of raised tiers toward the center. 0 = flat floor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Terrain", meta = (ClampMin = "0", ClampMax = "8"))
	int32 TierCount = 3;

	/** Rise of each tier, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Terrain", meta = (ClampMin = "0"))
	float TierHeight = 140.f;

	/** Distance inward from the wall between successive tier edges, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Terrain", meta = (ClampMin = "100"))
	float TierSpacing = 2000.f;

	/** Width of the sloped band that climbs onto each tier, cm. Smaller = steeper, more cliff-like. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Terrain", meta = (ClampMin = "50"))
	float RampWidth = 450.f;

	/** Vertex spacing of the terrain mesh, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Terrain", meta = (ClampMin = "25"))
	float TerrainResolution = 100.f;

	// ---- Whole-arena camera (menu backdrop / spectator; Docs/GAME_DESIGN.md section 11) ----

	/** Lower (closer to -30) reads more like a 3D driving view; higher (toward -89) is flatter top-down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Camera", meta = (ClampMin = "-89", ClampMax = "-30"))
	float CameraPitch = -55.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Camera", meta = (ClampMin = "20", ClampMax = "120"))
	float CameraFieldOfView = 60.f;

	/** Extra framing around the arena. 1.0 = edges touch the frame. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Camera", meta = (ClampMin = "1", ClampMax = "2"))
	float CameraFitPadding = 1.12f;

	/** 0 = compute distance from bounds and FOV. Otherwise use this distance in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Camera", meta = (ClampMin = "0"))
	float CameraDistanceOverride = 0.f;

	// ---- Cosmetics ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	float WallHeight = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	float WallThickness = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	FLinearColor FloorColor = FLinearColor(0.012f, 0.014f, 0.03f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	FLinearColor WallColor = FLinearColor(0.05f, 0.7f, 1.0f);

	/** Dev-only directional light so an empty level is visible. Turn off once the level has real lighting (Milestone 8). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	bool bEnableDevLight = true;

	// ---- Gameplay queries ----

	/** World-space XY rectangle right now (shrunk if the collapse is under way). */
	UFUNCTION(BlueprintPure, Category = "Neon|Arena")
	FBox2D GetBounds() const;

	/** Rectangle at round start, ignoring the collapse. */
	UFUNCTION(BlueprintPure, Category = "Neon|Arena")
	FBox2D GetFullBounds() const;

	/** Rectangle SecondsAhead from now, for prediction. Clamped to the final size. */
	UFUNCTION(BlueprintPure, Category = "Neon|Arena")
	FBox2D GetBoundsAt(float SecondsAhead) const;

	// ---- Collapse state (authority writes; everyone reads) ----

	/** Seconds since GO this round, or a negative value outside a round. Set by the game mode every frame. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Collapse")
	void SetRoundElapsed(float Seconds);

	/** Per-match override of bCollapseEnabled (Solo menu setting). */
	UFUNCTION(BlueprintCallable, Category = "Neon|Collapse")
	void SetCollapseActiveForMatch(bool bActive) { bCollapseActiveForMatch = bActive; }

	bool IsCollapseActive() const { return bCollapseEnabled && bCollapseActiveForMatch; }

	/** 0 before the walls move, 1 when they have stopped. */
	UFUNCTION(BlueprintPure, Category = "Neon|Collapse")
	float GetCollapseProgress() const { return CollapseProgressAt(RoundElapsed); }

	/** Seconds until the walls start moving. 0 while moving or done; negative when the collapse is off or no round is running. */
	UFUNCTION(BlueprintPure, Category = "Neon|Collapse")
	float GetSecondsUntilCollapse() const;

	UFUNCTION(BlueprintPure, Category = "Neon|Collapse")
	bool IsCollapsing() const;

	virtual void Tick(float DeltaTime) override;

	/**
	 * Guide layout: P1 and P3 on the left facing +X, P2 and P4 on the right facing -X.
	 * Index wraps for safety. Z sits VehicleSpawnHeight above the terrain.
	 */
	UFUNCTION(BlueprintPure, Category = "Neon|Arena")
	FTransform GetSpawnTransform(int32 PlayerIndex) const;

	// ---- Terrain queries (presentation) ----

	/** World Z of the floor surface at a world XY. Continuous. Outside the rectangle it clamps to the edge. */
	UFUNCTION(BlueprintPure, Category = "Neon|Terrain")
	float GetFloorHeight(FVector2D WorldXY) const;

	/** Unit surface normal at a world XY (finite differences). */
	UFUNCTION(BlueprintPure, Category = "Neon|Terrain")
	FVector GetFloorNormal(FVector2D WorldXY) const;

	/** First arena actor in the world, or null. */
	static ANeonArena* FindArena(const UWorld* World);

	UCameraComponent* GetCamera() const { return Camera; }

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	TObjectPtr<USceneComponent> Root;

	/** Terraced floor. Generated from GetFloorHeight(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Terrain")
	TObjectPtr<UProceduralMeshComponent> Terrain;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	TObjectPtr<UStaticMeshComponent> WallNorth;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	TObjectPtr<UStaticMeshComponent> WallSouth;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	TObjectPtr<UStaticMeshComponent> WallEast;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Arena")
	TObjectPtr<UStaticMeshComponent> WallWest;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Camera")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Cosmetic")
	TObjectPtr<UDirectionalLightComponent> DevLight;

private:
	/** Height in local (actor-relative) XY. */
	float LocalFloorHeight(float LocalX, float LocalY) const;
	float CollapseProgressAt(float Elapsed) const;
	FVector2D HalfExtentsAt(float Elapsed) const;
	void BuildTerrainMesh();
	void LayoutCosmetics();
	void LayoutWalls(const FVector2D& Half);
	void LayoutCamera();
	void TintMesh(UMeshComponent* Mesh, const FLinearColor& Color) const;
	void TintWalls(const FLinearColor& Color);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlaceholderMaterial;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> WallMIDs;

	float RoundElapsed = -1.f;
	bool bCollapseActiveForMatch = true;
	FVector2D LastWallHalf = FVector2D::ZeroVector;
};
