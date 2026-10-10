#include "Powerups/NeonPowerupPickup.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

ANeonPowerupPickup::ANeonPowerupPickup()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(false); // placed once; never moves

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	auto MakeMesh = [this](const TCHAR* Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* M = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		M->SetupAttachment(Root);
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetGenerateOverlapEvents(false);
		M->SetCastShadow(false);
		return M;
	};

	Orb = MakeMesh(TEXT("Orb"));
	Orb->SetRelativeLocation(FVector(0.f, 0.f, HoverHeight));
	Orb->SetRelativeScale3D(FVector(1.1f)); // engine sphere is 100 cm

	Beacon = MakeMesh(TEXT("Beacon"));
	Beacon->SetRelativeLocation(FVector(0.f, 0.f, 600.f));
	Beacon->SetRelativeScale3D(FVector(0.12f, 0.12f, 12.f)); // thin 1200 cm pillar

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (SphereFinder.Succeeded())
	{
		Orb->SetStaticMesh(SphereFinder.Object);
	}
	if (CubeFinder.Succeeded())
	{
		Beacon->SetStaticMesh(CubeFinder.Object);
	}
	if (MaterialFinder.Succeeded())
	{
		PlaceholderMaterial = MaterialFinder.Object;
		Orb->SetMaterial(0, PlaceholderMaterial);
		Beacon->SetMaterial(0, PlaceholderMaterial);
	}
}

void ANeonPowerupPickup::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANeonPowerupPickup, Type);
}

void ANeonPowerupPickup::BeginPlay()
{
	Super::BeginPlay();
	ApplyColor();
}

void ANeonPowerupPickup::InitPickup(ENeonPowerupType InType)
{
	Type = InType;
	ApplyColor();
}

void ANeonPowerupPickup::OnRep_Type()
{
	ApplyColor();
}

void ANeonPowerupPickup::ApplyColor()
{
	const FLinearColor C = NeonPowerups::Color(Type);
	if (Orb && Orb->GetMaterial(0))
	{
		if (UMaterialInstanceDynamic* MID = Orb->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue(TEXT("Color"), C);
		}
	}
	if (Beacon && Beacon->GetMaterial(0))
	{
		if (UMaterialInstanceDynamic* MID = Beacon->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue(TEXT("Color"), C * 0.35f);
		}
	}
}

void ANeonPowerupPickup::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	Age += DeltaTime;
	if (Orb)
	{
		Orb->SetRelativeLocation(FVector(0.f, 0.f, HoverHeight + FMath::Sin(Age * 2.5f) * BobAmplitude));
		Orb->SetRelativeRotation(FRotator(0.f, Age * SpinDegreesPerSecond, 0.f));
	}
}
