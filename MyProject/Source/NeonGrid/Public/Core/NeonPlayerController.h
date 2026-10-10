#pragma once

#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Vehicle/NeonVehicleInput.h"
#include "NeonPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UNeonHUDWidget;
class UNeonMainMenuWidget;
class ANeonGameState;
class ANeonVehiclePawn;
struct FInputActionValue;

/**
 * Local human input and local UI. Docs/VEHICLE_SPEC.md section 3, Docs/TECHNICAL_SPEC.md section 4.
 *
 * Input: reads Enhanced Input and pushes one FNeonVehicleInput snapshot per frame into the possessed
 * vehicle. AI controllers write the same struct.
 *
 * UI: owns the HUD and the main menu widgets (local player only) and switches them, the input mode and
 * the view target on match phase: MainMenu views the arena's fixed camera with the menu on top;
 * everything else views the vehicle's follow camera (DECISIONS.md D21) with the HUD.
 *
 * Spectating: once the local vehicle is eliminated the camera follows a surviving vehicle (A/D or
 * Left/Right cycles through them) until the next countdown. With nobody left alive it falls back to
 * the arena camera.
 *
 * Match requests (start, rematch, return to menu) go through server RPCs so the same code serves LAN
 * clients later; offline they execute immediately.
 *
 * Input assets: if the mapping context / actions below are left null, defaults are built in code
 * (A/D, Left/Right, gamepad left stick, Esc / gamepad Menu). Assign Editor-made assets to override.
 */
UCLASS()
class NEONGRID_API ANeonPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ANeonPlayerController();

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputMappingContext> VehicleMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputAction> SteerAction;

	/** Esc / gamepad Menu: leave the match and return to the main menu. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputAction> MenuAction;

	/** Activate the held powerup. Guide default: Space / right trigger (gamepad A also works). */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputAction> ActivateAction;

	/** Hold to sprint. Left Shift / gamepad left bumper. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputAction> SprintAction;

	/** Jump. Left Ctrl, F, or gamepad right bumper. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|Input")
	TObjectPtr<UInputAction> JumpAction;

	/** Widget classes. Defaults are the code-built C++ widgets; Blueprint subclasses can replace them. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|UI")
	TSubclassOf<UNeonHUDWidget> HUDWidgetClass;

	UPROPERTY(EditDefaultsOnly, Category = "Neon|UI")
	TSubclassOf<UNeonMainMenuWidget> MainMenuWidgetClass;

	/** Camera blend when switching between the arena view and the vehicle. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|UI", meta = (ClampMin = "0"))
	float ViewBlendTime = 0.6f;

	/** Camera blend when the spectator camera jumps to another vehicle. */
	UPROPERTY(EditDefaultsOnly, Category = "Neon|UI", meta = (ClampMin = "0"))
	float SpectateBlendTime = 0.8f;

	/** Vehicle the spectator camera follows, or null when viewing our own vehicle / the arena. */
	UFUNCTION(BlueprintPure, Category = "Neon|Spectate")
	ANeonVehiclePawn* GetSpectateTarget() const;

	UFUNCTION(BlueprintPure, Category = "Neon|Spectate")
	bool IsSpectating() const { return bSpectating; }

	// ---- Requests from UI ----

	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void RequestStartSoloMatch(const FNeonMatchSettings& Settings);

	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void RequestRematch();

	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void RequestReturnToMainMenu();

	UFUNCTION(BlueprintCallable, Category = "Neon|Match")
	void RequestQuit();

	/** Reload this map as a listen server (Milestone 6 first slice). */
	UFUNCTION(BlueprintCallable, Category = "Neon|Lan")
	void RequestHostLan();

	/** Travel to a host at Address (IP or IP:port). */
	UFUNCTION(BlueprintCallable, Category = "Neon|Lan")
	void RequestJoinLan(const FString& Address);

	/** Browse the local network for advertised hosts. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Lan")
	void RequestFindLan();

	/** Join a host from the last network scan. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Lan")
	void RequestJoinLanGame(int32 SearchIndex);

	/** Host starts a LAN match: humans only, no AI fill. */
	UFUNCTION(BlueprintCallable, Category = "Neon|Lan")
	void RequestStartLanMatch(const FNeonMatchSettings& Settings);

	virtual void PlayerTick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void OnPossess(APawn* InPawn) override;

	UFUNCTION(Server, Reliable)
	void ServerStartSoloMatch(const FNeonMatchSettings& Settings);

	UFUNCTION(Server, Reliable)
	void ServerRematch();

	UFUNCTION(Server, Reliable)
	void ServerReturnToMainMenu();

	void BuildDefaultInputAssets();
	void EnsureUI();
	void BindGameState();

	UFUNCTION()
	void HandlePhaseChanged(ENeonMatchPhase NewPhase);

	void ApplyPhaseUI(ENeonMatchPhase Phase);
	void ViewArena();
	void ViewVehicle(ANeonVehiclePawn* Vehicle, float BlendTime);

	void UpdateSpectating();
	void StopSpectating(bool bViewOwnPawn);
	/** Pick the next alive vehicle after the current target (Direction +1 / -1). Returns false if none is alive. */
	bool CycleSpectateTarget(int32 Direction);
	void CollectAliveVehicles(TArray<ANeonVehiclePawn*>& Out) const;

	void OnSteer(const FInputActionValue& Value);
	void OnSteerReleased(const FInputActionValue& Value);
	void OnMenuPressed(const FInputActionValue& Value);
	void OnActivatePressed(const FInputActionValue& Value);
	void OnActivateReleased(const FInputActionValue& Value);
	void OnSprintPressed(const FInputActionValue& Value);
	void OnSprintReleased(const FInputActionValue& Value);
	void OnJumpPressed(const FInputActionValue& Value);
	void OnJumpReleased(const FInputActionValue& Value);

	FNeonVehicleInput CurrentInput;

	UPROPERTY(Transient)
	TObjectPtr<UNeonHUDWidget> HUDWidget;

	UPROPERTY(Transient)
	TObjectPtr<UNeonMainMenuWidget> MainMenuWidget;

	TWeakObjectPtr<ANeonGameState> BoundGameState;
	bool bUIBuilt = false;

	TWeakObjectPtr<ANeonVehiclePawn> SpectateTarget;
	bool bSpectating = false;
	bool bSpectatingArena = false;
	int32 LastSpectateSteerDir = 0;
};
