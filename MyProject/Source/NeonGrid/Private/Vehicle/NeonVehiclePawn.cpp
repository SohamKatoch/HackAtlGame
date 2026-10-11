#include "Vehicle/NeonVehiclePawn.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Trail/NeonTrailComponent.h"
#include "Trail/NeonTrailRendererComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Vehicle/NeonLeanComponent.h"

namespace
{
	/** Tints the Tron light cycle material: paint + ring take the team color, light strips take Glow. */
	void SetCycleColors(UMaterialInstanceDynamic* MID, const FLinearColor& Paint, const FLinearColor& Glow)
	{
		if (!MID)
		{
			return;
		}
		MID->SetVectorParameterValue(TEXT("Paint Color"), Paint);
		MID->SetVectorParameterValue(TEXT("Ring Color"), Paint);
		MID->SetVectorParameterValue(TEXT("Lighting Color"), Glow);
	}
}

ANeonVehiclePawn::ANeonVehiclePawn()
{
	// Actor tick is cosmetic only (powerup visuals). Simulation runs in the movement component.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	AutoPossessAI = EAutoPossessAI::Disabled;
	// The host simulates. A higher rate keeps remote bikes from stepping between updates.
	SetNetUpdateFrequency(60.f);
	SetMinNetUpdateFrequency(30.f);

	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Movement = CreateDefaultSubobject<UNeonMovementComponent>(TEXT("Movement"));
	Trail = CreateDefaultSubobject<UNeonTrailComponent>(TEXT("Trail"));
	Powerups = CreateDefaultSubobject<UNeonPowerupComponent>(TEXT("Powerups"));

	TrailRenderer = CreateDefaultSubobject<UNeonTrailRendererComponent>(TEXT("TrailRenderer"));
	TrailRenderer->SetupAttachment(Root);

	LeanRoot = CreateDefaultSubobject<UNeonLeanComponent>(TEXT("LeanRoot"));
	LeanRoot->SetupAttachment(Root);

	// Tron Uprez light cycle. Source model is ~372 cm long (+X forward, origin at the ground), assembled
	// as in BP_Light_Cycle_Uprez: body at X=+37, rear wheel at (-139, 0, 54). Scaled to ~150 cm to match
	// the 60 cm collision radius, centered on the pawn, and dropped onto the floor (Z = -15, see ShieldDisc).
	constexpr float CycleScale = 0.4f;
	constexpr float CycleCenterX = 7.f; // shifts the assembly so its overall length is centered on the pawn
	constexpr float FloorZ = -15.f;

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(LeanRoot);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetGenerateOverlapEvents(false);
	BodyMesh->SetCastShadow(false);
	BodyMesh->SetRelativeLocation(FVector((37.f + CycleCenterX) * CycleScale, 0.f, FloorZ));
	BodyMesh->SetRelativeScale3D(FVector(CycleScale));

	WheelMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WheelMesh"));
	WheelMesh->SetupAttachment(LeanRoot);
	WheelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WheelMesh->SetGenerateOverlapEvents(false);
	WheelMesh->SetCastShadow(false);
	WheelMesh->SetRelativeLocation(FVector((-139.f + CycleCenterX) * CycleScale, 0.f, FloorZ + 54.f * CycleScale));
	WheelMesh->SetRelativeScale3D(FVector(CycleScale));

	NoseMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NoseMesh"));
	NoseMesh->SetupAttachment(LeanRoot);
	NoseMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NoseMesh->SetGenerateOverlapEvents(false);
	NoseMesh->SetCastShadow(false);
	NoseMesh->SetRelativeLocation(FVector(85.f, 0.f, 5.f));
	NoseMesh->SetRelativeScale3D(FVector(0.5f, 0.3f, 0.2f));
	// The light cycle shows heading on its own; boost glow moves to the cycle's light strips.
	// HiddenInGame (not Visibility) so LeanRoot's propagated SetVisibility does not bring it back.
	NoseMesh->SetHiddenInGame(true);

	// Shield: a thin disc on the floor around the vehicle. Attached to Root so it does not lean.
	ShieldDisc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ShieldDisc"));
	ShieldDisc->SetupAttachment(Root);
	ShieldDisc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ShieldDisc->SetGenerateOverlapEvents(false);
	ShieldDisc->SetCastShadow(false);
	ShieldDisc->SetRelativeLocation(FVector(0.f, 0.f, -12.f));
	ShieldDisc->SetRelativeScale3D(FVector(2.6f, 2.6f, 0.06f)); // engine cylinder is 100 cm
	ShieldDisc->SetVisibility(false);

	// Follow camera. Yaw follows the vehicle heading; pitch/roll are fixed so lean never tilts the view.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Root);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritYaw = true;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bDoCollisionTest = false; // nothing in the arena has physics collision
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->TargetArmLength = CameraDistance;
	CameraBoom->TargetOffset = FVector(0.f, 0.f, CameraHeight);
	CameraBoom->SetRelativeRotation(FRotator(-CameraPitchDown, 0.f, 0.f));
	CameraBoom->CameraLagSpeed = CameraLagSpeed;
	CameraBoom->CameraRotationLagSpeed = CameraRotationLagSpeed;
	CameraBoom->CameraLagMaxDistance = 400.f; // respawn teleports catch up instead of flying across the arena

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->FieldOfView = CameraFieldOfView;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CycleBodyFinder(TEXT("/Game/Tron_Uprez/Vehicles/Light_Cycle_Uprez/Parts/Main_Body.Main_Body"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CycleWheelFinder(TEXT("/Game/Tron_Uprez/Vehicles/Light_Cycle_Uprez/Parts/Wheel.Wheel"));
	if (CubeFinder.Succeeded())
	{
		NoseMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (CylinderFinder.Succeeded())
	{
		ShieldDisc->SetStaticMesh(CylinderFinder.Object);
	}
	// Mesh's own material (M_lightcycle_Uprez_Inst) is kept; ApplyColor tints it per player.
	if (CycleBodyFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CycleBodyFinder.Object);
	}
	if (CycleWheelFinder.Succeeded())
	{
		WheelMesh->SetStaticMesh(CycleWheelFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		PlaceholderMaterial = MaterialFinder.Object;
		NoseMesh->SetMaterial(0, PlaceholderMaterial);
		ShieldDisc->SetMaterial(0, PlaceholderMaterial);
	}
}

void ANeonVehiclePawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	SmoothRemotePose(DeltaTime);
	UpdatePowerupVisuals(DeltaTime);
}

void ANeonVehiclePawn::PostNetReceiveLocationAndRotation()
{
	// The owning client predicts its own bike. This update is for everyone else's.
	if (IsLocallyControlled())
	{
		return;
	}
	const FRepMovement& Rep = GetReplicatedMovement();
	NoteRemotePose(Rep.Location, Rep.Rotation.Yaw);
}

void ANeonVehiclePawn::NoteRemotePose(FVector Location, float Yaw)
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (bHasRemotePose)
	{
		const double Dt = FMath::Max(Now - RemotePoseTime, 1.0 / 240.0);
		RemoteVelocity = (Location - RemoteLocation) / Dt;
		RemoteYawRate = FRotator::NormalizeAxis(Yaw - RemoteYaw) / static_cast<float>(Dt);
	}
	else
	{
		RemoteVelocity = FVector::ZeroVector;
		RemoteYawRate = 0.f;
		SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	}
	RemoteLocation = Location;
	RemoteYaw = Yaw;
	RemotePoseTime = Now;
	bHasRemotePose = true;
}

void ANeonVehiclePawn::SmoothRemotePose(float DeltaTime)
{
	if (!bHasRemotePose || HasAuthority() || IsLocallyControlled() || !GetWorld())
	{
		return;
	}

	const double Since = GetWorld()->GetTimeSeconds() - RemotePoseTime;
	// Keep gliding at the last speed through a short gap, then hold rather than run off the track.
	const double Predict = FMath::Min(FMath::Max(Since, 0.0), 0.05);
	const FVector Desired = RemoteLocation + RemoteVelocity * Predict;
	const float DesiredYaw = RemoteYaw + RemoteYawRate * static_cast<float>(Predict);

	const FVector Current = GetActorLocation();
	if (FVector::DistSquared(Current, Desired) > FMath::Square(500.f))
	{
		SetActorLocationAndRotation(Desired, FRotator(0.f, DesiredYaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

	const float Alpha = FMath::Clamp(DeltaTime * 60.f, 0.f, 1.f);
	const FVector Location = FMath::Lerp(Current, Desired, Alpha);
	const float Yaw = GetActorRotation().Yaw + FRotator::NormalizeAxis(DesiredYaw - GetActorRotation().Yaw) * Alpha;
	SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f), false, nullptr, ETeleportType::None);
}

void ANeonVehiclePawn::UpdatePowerupVisuals(float DeltaTime)
{
	if (!Powerups || !Movement)
	{
		return;
	}
	const bool bAlive = Movement->IsAlive();

	// Shield disc.
	if (ShieldDisc)
	{
		ShieldDisc->SetVisibility(bAlive && Powerups->HasShield());
	}

	// Phase: blink the body so the vehicle is obviously not solid.
	const bool bPhased = bAlive && Powerups->GetActiveType() == ENeonPowerupType::Phase;
	if (bPhased)
	{
		PhaseBlinkTime += DeltaTime;
		const bool bHide = FMath::Fmod(PhaseBlinkTime, 0.16f) < 0.08f;
		if (bHide != bPhaseBlinkHidden && LeanRoot)
		{
			bPhaseBlinkHidden = bHide;
			LeanRoot->SetVisibility(!bHide, true);
		}
	}
	else if (bPhaseBlinkHidden)
	{
		bPhaseBlinkHidden = false;
		PhaseBlinkTime = 0.f;
		if (LeanRoot && bAlive)
		{
			LeanRoot->SetVisibility(true, true);
		}
	}

	// Nose: glows the powerup color while Boost / Overdrive run. Body: dims toward EMP yellow while jammed.
	const ENeonPowerupType Active = Powerups->GetActiveType();
	const bool bSpeeding = Active == ENeonPowerupType::Boost || Active == ENeonPowerupType::Overdrive;
	if (NoseMID)
	{
		NoseMID->SetVectorParameterValue(TEXT("Color"), bSpeeding ? NeonPowerups::Color(Active) : NoseColor);
	}
	if (BodyMID || WheelMID)
	{
		FLinearColor Body = VehicleColor;
		if (Powerups->IsEMPd())
		{
			const float Flicker = 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 30.f);
			Body = FMath::Lerp(VehicleColor * 0.3f, NeonPowerups::Color(ENeonPowerupType::EMP), 0.5f * Flicker);
		}
		const FLinearColor Glow = bSpeeding ? NeonPowerups::Color(Active) : Body;
		SetCycleColors(BodyMID, Body, Glow);
		SetCycleColors(WheelMID, Body, Glow);
	}
}

void ANeonVehiclePawn::BeginPlay()
{
	Super::BeginPlay();

	// Re-apply in case a Blueprint subclass changed the tunables after the constructor ran.
	if (CameraBoom)
	{
		CameraBoom->TargetArmLength = CameraDistance;
		CameraBoom->TargetOffset = FVector(0.f, 0.f, CameraHeight);
		CameraBoom->SetRelativeRotation(FRotator(-CameraPitchDown, 0.f, 0.f));
		CameraBoom->CameraLagSpeed = CameraLagSpeed;
		CameraBoom->CameraRotationLagSpeed = CameraRotationLagSpeed;
	}
	if (FollowCamera)
	{
		FollowCamera->FieldOfView = CameraFieldOfView;
	}

	if (Movement)
	{
		Movement->SetPlayerSlot(PlayerSlot);
		Movement->OnEliminated.AddDynamic(this, &ANeonVehiclePawn::HandleEliminated);
	}
	if (TrailRenderer)
	{
		TrailRenderer->SetOwnerId(PlayerSlot);
	}

	ApplyColor();
}

void ANeonVehiclePawn::SetPlayerSlot(int32 InSlot)
{
	PlayerSlot = InSlot;
	if (Movement)
	{
		Movement->SetPlayerSlot(InSlot);
	}
	if (Trail)
	{
		Trail->SetOwnerId(InSlot);
	}
	if (TrailRenderer)
	{
		TrailRenderer->SetOwnerId(InSlot);
	}
}

void ANeonVehiclePawn::SetBodyVisible(bool bVisible)
{
	if (LeanRoot)
	{
		LeanRoot->SetVisibility(bVisible, true);
	}
}

void ANeonVehiclePawn::SetFollowCameraActive(bool bActive)
{
	if (FollowCamera)
	{
		FollowCamera->SetActive(bActive);
	}
}

void ANeonVehiclePawn::HandleEliminated(AActor* /*Vehicle*/, ENeonEliminationCause /*Cause*/)
{
	// Hide the body only. The trail stays as an obstacle and stays visible.
	if (LeanRoot)
	{
		LeanRoot->SetVisibility(false, true);
	}
	// Drop this camera so a dead local player is forced onto a living vehicle (D27 spectator).
	SetFollowCameraActive(false);
}

void ANeonVehiclePawn::ResetToSpawn(const FTransform& SpawnTransform)
{
	if (LeanRoot)
	{
		LeanRoot->SetVisibility(true, true);
	}
	SetFollowCameraActive(true);

	if (Movement)
	{
		Movement->ResetSimulation(SpawnTransform.GetLocation(), SpawnTransform.Rotator().Yaw);
	}
	else
	{
		SetActorTransform(SpawnTransform);
	}
}

void ANeonVehiclePawn::SetVehicleColor(FLinearColor InColor)
{
	VehicleColor = InColor;
	if (HasActorBegunPlay())
	{
		ApplyColor();
	}
}

void ANeonVehiclePawn::ApplyColor()
{
	auto Tint = [](UStaticMeshComponent* Mesh, const FLinearColor& Color) -> UMaterialInstanceDynamic*
	{
		if (!Mesh || !Mesh->GetMaterial(0))
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* MID = Mesh->CreateAndSetMaterialInstanceDynamic(0);
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Color);
		}
		return MID;
	};

	BodyMID = BodyMesh && BodyMesh->GetMaterial(0) ? BodyMesh->CreateAndSetMaterialInstanceDynamic(0) : nullptr;
	WheelMID = WheelMesh && WheelMesh->GetMaterial(0) ? WheelMesh->CreateAndSetMaterialInstanceDynamic(0) : nullptr;
	SetCycleColors(BodyMID, VehicleColor, VehicleColor);
	SetCycleColors(WheelMID, VehicleColor, VehicleColor);
	if (TrailRenderer)
	{
		TrailRenderer->SetTrailColor(VehicleColor);
	}
	// Brighter nose so the front reads from above.
	NoseColor = FLinearColor(FMath::Min(VehicleColor.R * 2.f + 0.5f, 1.f), FMath::Min(VehicleColor.G * 2.f + 0.5f, 1.f), FMath::Min(VehicleColor.B * 2.f + 0.5f, 1.f));
	NoseMID = Tint(NoseMesh, NoseColor);
	Tint(ShieldDisc, NeonPowerups::Color(ENeonPowerupType::Shield));
}
