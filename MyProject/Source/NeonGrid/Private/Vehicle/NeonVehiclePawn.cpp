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

ANeonVehiclePawn::ANeonVehiclePawn()
{
	// Actor tick is cosmetic only (powerup visuals). Simulation runs in the movement component.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	AutoPossessAI = EAutoPossessAI::Disabled;
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

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(LeanRoot);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetGenerateOverlapEvents(false);
	BodyMesh->SetCastShadow(false);
	// Engine cube is 100 cm. Body: 140 x 70 x 30 cm, low and wide.
	BodyMesh->SetRelativeScale3D(FVector(1.4f, 0.7f, 0.3f));

	NoseMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NoseMesh"));
	NoseMesh->SetupAttachment(LeanRoot);
	NoseMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NoseMesh->SetGenerateOverlapEvents(false);
	NoseMesh->SetCastShadow(false);
	NoseMesh->SetRelativeLocation(FVector(85.f, 0.f, 5.f));
	NoseMesh->SetRelativeScale3D(FVector(0.5f, 0.3f, 0.2f));

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
	if (CubeFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CubeFinder.Object);
		NoseMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (CylinderFinder.Succeeded())
	{
		ShieldDisc->SetStaticMesh(CylinderFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		PlaceholderMaterial = MaterialFinder.Object;
		BodyMesh->SetMaterial(0, PlaceholderMaterial);
		NoseMesh->SetMaterial(0, PlaceholderMaterial);
		ShieldDisc->SetMaterial(0, PlaceholderMaterial);
	}
}

void ANeonVehiclePawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdatePowerupVisuals(DeltaTime);
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
	if (BodyMID)
	{
		FLinearColor Body = VehicleColor;
		if (Powerups->IsEMPd())
		{
			const float Flicker = 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 30.f);
			Body = FMath::Lerp(VehicleColor * 0.3f, NeonPowerups::Color(ENeonPowerupType::EMP), 0.5f * Flicker);
		}
		BodyMID->SetVectorParameterValue(TEXT("Color"), Body);
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

	BodyMID = Tint(BodyMesh, VehicleColor);
	if (TrailRenderer)
	{
		TrailRenderer->SetTrailColor(VehicleColor);
	}
	// Brighter nose so the front reads from above.
	NoseColor = FLinearColor(FMath::Min(VehicleColor.R * 2.f + 0.5f, 1.f), FMath::Min(VehicleColor.G * 2.f + 0.5f, 1.f), FMath::Min(VehicleColor.B * 2.f + 0.5f, 1.f));
	NoseMID = Tint(NoseMesh, NoseColor);
	Tint(ShieldDisc, NeonPowerups::Color(ENeonPowerupType::Shield));
}
