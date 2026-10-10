#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NeonArena.generated.h"

class UStaticMeshComponent;
class UProceduralMeshComponent;
class UCameraComponent;
class UDirectionalLightComponent;
class UMaterialInterface;

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
	 * Half size of the playable rectangle in cm (X = long axis). 14000 x 9000 total (DECISIONS.md D23).
	 * At 2200 cm/s a vehicle crosses the long axis in about 6.4 s and the short axis in about 4 s.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Neon|Arena", meta = (ClampMin = "100"))
	FVector2D HalfExtents = FVector2D(7000.f, 4500.f);

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
	float TierSpacing = 1200.f;

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

	/** World-space XY rectangle. */
	UFUNCTION(BlueprintPure, Category = "Neon|Arena")
	FBox2D GetBounds() const;

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
	void BuildTerrainMesh();
	void LayoutCosmetics();
	void LayoutCamera();
	void TintMesh(UMeshComponent* Mesh, const FLinearColor& Color) const;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlaceholderMaterial;
};
