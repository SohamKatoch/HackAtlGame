#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Vehicle/NeonMovementComponent.h"
#include "NeonVehiclePawn.generated.h"

class UNeonLeanComponent;
class UNeonTrailComponent;
class UNeonTrailRendererComponent;
class UNeonPowerupComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USpringArmComponent;
class UCameraComponent;

/**
 * The vehicle. Shared by human and AI players. Docs/VEHICLE_SPEC.md.
 *
 * Root: plain scene component. Gameplay collision is the radius in the vehicle config, used by the
 * movement step; there is no physics body.
 * LeanRoot: cosmetic roll. Placeholder meshes (engine cubes) hang under it with collision disabled.
 *
 * Input is not bound here. The controller (human or AI) writes an FNeonVehicleInput to the movement component.
 */
UCLASS()
class NEONGRID_API ANeonVehiclePawn : public APawn
{
	GENERATED_BODY()

public:
	ANeonVehiclePawn();

	/** Body and trail color for this player. Set by the game mode / lobby. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Vehicle")
	FLinearColor VehicleColor = FLinearColor(0.1f, 0.9f, 1.0f);

	/** Move the simulation and the actor to a spawn transform and mark alive. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Vehicle")
	void ResetToSpawn(const FTransform& SpawnTransform);

	UFUNCTION(BlueprintCallable, Category = "Neon|Vehicle")
	void SetVehicleColor(FLinearColor InColor);

	/** Player slot (0-3). Also the trail owner id. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Vehicle")
	void SetPlayerSlot(int32 InSlot);

	UFUNCTION(BlueprintPure, Category = "Neon|Vehicle")
	int32 GetPlayerSlot() const { return PlayerSlot; }

	UNeonMovementComponent* GetMovement() const { return Movement; }
	UNeonLeanComponent* GetLeanRoot() const { return LeanRoot; }
	UNeonTrailComponent* GetTrail() const { return Trail; }
	UNeonPowerupComponent* GetPowerups() const { return Powerups; }
	UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	/** Spectator / possession: turn this pawn's follow camera on or off. */
	void SetFollowCameraActive(bool bActive);

	virtual void Tick(float DeltaTime) override;

	// ---- Follow camera (DECISIONS.md D21). Cosmetic, local-only; never read by the simulation. ----

	/** Distance behind the vehicle, cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "100"))
	float CameraDistance = 650.f;

	/** Height of the camera pivot above the vehicle, cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "0"))
	float CameraHeight = 190.f;

	/** Downward tilt in degrees. Small values look along the floor; larger ones look down at the car. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "0", ClampMax = "60"))
	float CameraPitchDown = 12.f;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "40", ClampMax = "120"))
	float CameraFieldOfView = 85.f;

	/** Position smoothing. Higher = tighter. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "0"))
	float CameraLagSpeed = 14.f;

	/** Yaw smoothing so the view swings into turns rather than snapping. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Camera", meta = (ClampMin = "0"))
	float CameraRotationLagSpeed = 9.f;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleEliminated(AActor* Vehicle, ENeonEliminationCause Cause);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UNeonTrailComponent> Trail;

	/** Cosmetic trail mirror. Absolute transform; survives the pawn being hidden. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UNeonTrailRendererComponent> TrailRenderer;

	/** Held powerup and active effects (Milestone 4). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UNeonPowerupComponent> Powerups;

	/** Flat disc under the vehicle while a Shield charge is armed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UStaticMeshComponent> ShieldDisc;

	int32 PlayerSlot = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UNeonMovementComponent> Movement;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UNeonLeanComponent> LeanRoot;

	/** Placeholder until the Blender vehicle lands (Milestone 8). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	/** Small forward block so heading is obvious. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Vehicle")
	TObjectPtr<UStaticMeshComponent> NoseMesh;

	/** Attached to Root (not LeanRoot) so the view does not roll with the lean. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Neon|Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

private:
	void ApplyColor();
	/** Phase blink, boost nose glow, EMP body dim, shield disc. Cosmetic; reads the powerup component. */
	void UpdatePowerupVisuals(float DeltaTime);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlaceholderMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> NoseMID;

	FLinearColor NoseColor = FLinearColor::White;
	float PhaseBlinkTime = 0.f;
	bool bPhaseBlinkHidden = false;
};
