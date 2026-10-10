#include "Arena/NeonArena.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/NeonGameState.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NeonGrid.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	UStaticMeshComponent* MakeCosmeticMesh(AActor* Owner, USceneComponent* Parent, const TCHAR* Name)
	{
		UStaticMeshComponent* Mesh = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Mesh->SetupAttachment(Parent);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCastShadow(false);
		return Mesh;
	}

	/** Mesh section builder. Winding follows UE's convention: (0,0) -> (0,+Y) -> (+X,+Y) faces +Z. */
	struct FSectionBuilder
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;
		TArray<FProcMeshTangent> Tangents;

		int32 AddVertex(const FVector& P, const FVector& N, const FVector2D& UV)
		{
			Vertices.Add(P);
			Normals.Add(N);
			UVs.Add(UV);
			Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
			return Vertices.Num() - 1;
		}

		/** A = (x0,y0)  B = (x1,y0)  C = (x0,y1)  D = (x1,y1). Two upward-facing triangles. */
		void AddQuad(int32 A, int32 B, int32 C, int32 D)
		{
			Triangles.Append({ A, C, D, A, D, B });
		}

		/** Flat horizontal rectangle at height Z. */
		void AddFlatRect(float X0, float Y0, float X1, float Y1, float Z)
		{
			const FVector Up = FVector::UpVector;
			const int32 A = AddVertex(FVector(X0, Y0, Z), Up, FVector2D(0.f, 0.f));
			const int32 B = AddVertex(FVector(X1, Y0, Z), Up, FVector2D(1.f, 0.f));
			const int32 C = AddVertex(FVector(X0, Y1, Z), Up, FVector2D(0.f, 1.f));
			const int32 D = AddVertex(FVector(X1, Y1, Z), Up, FVector2D(1.f, 1.f));
			AddQuad(A, B, C, D);
		}
	};
}

ANeonArena::ANeonArena()
{
	// Tick only moves the cosmetic walls while the collapse runs.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Terrain = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Terrain"));
	Terrain->SetupAttachment(Root);
	Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Terrain->SetGenerateOverlapEvents(false);
	Terrain->SetCastShadow(false);
	Terrain->bUseAsyncCooking = false;
	Terrain->bUseComplexAsSimpleCollision = false;

	WallNorth = MakeCosmeticMesh(this, Root, TEXT("WallNorth"));
	WallSouth = MakeCosmeticMesh(this, Root, TEXT("WallSouth"));
	WallEast = MakeCosmeticMesh(this, Root, TEXT("WallEast"));
	WallWest = MakeCosmeticMesh(this, Root, TEXT("WallWest"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	if (CubeFinder.Succeeded())
	{
		for (UStaticMeshComponent* Wall : { WallNorth.Get(), WallSouth.Get(), WallEast.Get(), WallWest.Get() })
		{
			Wall->SetStaticMesh(CubeFinder.Object);
		}
	}
	if (MaterialFinder.Succeeded())
	{
		PlaceholderMaterial = MaterialFinder.Object;
		for (UStaticMeshComponent* Mesh : { WallNorth.Get(), WallSouth.Get(), WallEast.Get(), WallWest.Get() })
		{
			Mesh->SetMaterial(0, PlaceholderMaterial);
		}
	}

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->SetConstraintAspectRatio(true);
	Camera->SetAspectRatio(16.f / 9.f);

	DevLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("DevLight"));
	DevLight->SetupAttachment(Root);
	DevLight->SetRelativeRotation(FRotator(-60.f, 35.f, 0.f));
	DevLight->SetIntensity(4.f);
	DevLight->SetCastShadows(false);
}

void ANeonArena::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildTerrainMesh();
	LayoutCosmetics();
	LayoutCamera();
}

ANeonArena* ANeonArena::FindArena(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ANeonArena> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Gameplay queries
// ---------------------------------------------------------------------------

FBox2D ANeonArena::GetFullBounds() const
{
	const FVector Center = GetActorLocation();
	const FVector2D C(Center.X, Center.Y);
	return FBox2D(C - HalfExtents, C + HalfExtents);
}

FBox2D ANeonArena::GetBounds() const
{
	const FVector Center = GetActorLocation();
	const FVector2D C(Center.X, Center.Y);
	const FVector2D Half = HalfExtentsAt(RoundElapsed);
	return FBox2D(C - Half, C + Half);
}

FBox2D ANeonArena::GetBoundsAt(float SecondsAhead) const
{
	const FVector Center = GetActorLocation();
	const FVector2D C(Center.X, Center.Y);
	const FVector2D Half = HalfExtentsAt(RoundElapsed < 0.f ? RoundElapsed : RoundElapsed + FMath::Max(SecondsAhead, 0.f));
	return FBox2D(C - Half, C + Half);
}

// ---------------------------------------------------------------------------
// Collapse (D27)
// ---------------------------------------------------------------------------

float ANeonArena::CollapseProgressAt(float Elapsed) const
{
	if (!IsCollapseActive() || Elapsed < 0.f)
	{
		return 0.f;
	}
	return FMath::Clamp((Elapsed - CollapseStartDelay) / FMath::Max(CollapseDuration, 1.f), 0.f, 1.f);
}

FVector2D ANeonArena::HalfExtentsAt(float Elapsed) const
{
	const float T = CollapseProgressAt(Elapsed);
	if (T <= 0.f)
	{
		return HalfExtents;
	}
	const FVector2D Min(FMath::Min(MinHalfExtents.X, HalfExtents.X), FMath::Min(MinHalfExtents.Y, HalfExtents.Y));
	return FMath::Lerp(HalfExtents, Min, T);
}

void ANeonArena::SetRoundElapsed(float Seconds)
{
	RoundElapsed = Seconds;
}

float ANeonArena::GetSecondsUntilCollapse() const
{
	if (!IsCollapseActive() || RoundElapsed < 0.f)
	{
		return -1.f;
	}
	return FMath::Max(CollapseStartDelay - RoundElapsed, 0.f);
}

bool ANeonArena::IsCollapsing() const
{
	const float T = GetCollapseProgress();
	return T > 0.f && T < 1.f;
}

void ANeonArena::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Round timing comes from the replicated game state, so server and clients agree on the wall.
	// Results phases keep the last value so the walls stay where the round ended.
	if (const ANeonGameState* GS = GetWorld() ? GetWorld()->GetGameState<ANeonGameState>() : nullptr)
	{
		bCollapseActiveForMatch = GS->IsArenaCollapseEnabled();
		switch (GS->GetPhase())
		{
		case ENeonMatchPhase::Playing:
			RoundElapsed = GS->GetRoundElapsed();
			break;
		case ENeonMatchPhase::MainMenu:
		case ENeonMatchPhase::Countdown:
			RoundElapsed = -1.f;
			break;
		default:
			break;
		}
	}

	const FVector2D Half = HalfExtentsAt(RoundElapsed);
	if (!Half.Equals(LastWallHalf, 0.5f))
	{
		LayoutWalls(Half);
	}

	// Wall tint: pulse toward the collapse color while the walls move; steady accent otherwise.
	if (IsCollapsing())
	{
		const float Pulse = 0.55f + 0.45f * FMath::Sin(GetWorld()->GetTimeSeconds() * 6.f);
		TintWalls(FMath::Lerp(WallColor, CollapseWallColor, Pulse));
	}
	else if (GetCollapseProgress() >= 1.f)
	{
		TintWalls(CollapseWallColor);
	}
	else if (RoundElapsed >= 0.f && GetSecondsUntilCollapse() <= 5.f && GetSecondsUntilCollapse() > 0.f)
	{
		// Warning flicker in the last seconds before the walls start.
		const float Pulse = 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 10.f);
		TintWalls(FMath::Lerp(WallColor, CollapseWallColor, 0.6f * Pulse));
	}
	else
	{
		TintWalls(WallColor);
	}
}

FTransform ANeonArena::GetSpawnTransform(int32 PlayerIndex) const
{
	const int32 Index = ((PlayerIndex % 4) + 4) % 4;

	// 0: left-bottom facing +X   1: right-bottom facing -X
	// 2: left-top facing +X      3: right-top facing -X
	const float SignX = (Index % 2 == 0) ? -1.f : 1.f;
	const float SignY = (Index < 2) ? -1.f : 1.f;
	const float Heading = (Index % 2 == 0) ? 0.f : 180.f;

	const float LocalX = SignX * HalfExtents.X * SpawnInsetFraction.X;
	const float LocalY = SignY * HalfExtents.Y * SpawnInsetFraction.Y;

	const FVector Center = GetActorLocation();
	const FVector Location(
		Center.X + LocalX,
		Center.Y + LocalY,
		Center.Z + LocalFloorHeight(LocalX, LocalY) + VehicleSpawnHeight);

	return FTransform(FRotator(0.f, Heading, 0.f), Location, FVector::OneVector);
}

// ---------------------------------------------------------------------------
// Terrain (D25): concentric rectangular tiers, each reached by a smooth ramp band.
// Continuous everywhere, so Z is a pure function of XY and never affects the 2D simulation.
// ---------------------------------------------------------------------------

float ANeonArena::LocalFloorHeight(float LocalX, float LocalY) const
{
	if (TierCount <= 0 || TierHeight <= 0.f)
	{
		return 0.f;
	}

	const float AbsX = FMath::Clamp(FMath::Abs(LocalX), 0.f, HalfExtents.X);
	const float AbsY = FMath::Clamp(FMath::Abs(LocalY), 0.f, HalfExtents.Y);

	// Distance inward from the nearest wall. Iso-lines are concentric rectangles.
	const float Inset = FMath::Min(HalfExtents.X - AbsX, HalfExtents.Y - AbsY);

	const float Ramp = FMath::Max(RampWidth, 1.f);
	float Height = 0.f;
	for (int32 Tier = 1; Tier <= TierCount; ++Tier)
	{
		const float Top = Tier * TierSpacing;   // where the ramp reaches the plateau
		const float Foot = Top - Ramp;           // where the ramp leaves the lower plateau
		const float T = FMath::Clamp((Inset - Foot) / Ramp, 0.f, 1.f);
		Height += TierHeight * FMath::SmoothStep(0.f, 1.f, T);
	}
	return Height;
}

float ANeonArena::GetFloorHeight(FVector2D WorldXY) const
{
	const FVector Center = GetActorLocation();
	return Center.Z + LocalFloorHeight(WorldXY.X - Center.X, WorldXY.Y - Center.Y);
}

FVector ANeonArena::GetFloorNormal(FVector2D WorldXY) const
{
	const FVector Center = GetActorLocation();
	const float LX = WorldXY.X - Center.X;
	const float LY = WorldXY.Y - Center.Y;

	constexpr float Eps = 25.f;
	const float DhDx = (LocalFloorHeight(LX + Eps, LY) - LocalFloorHeight(LX - Eps, LY)) / (2.f * Eps);
	const float DhDy = (LocalFloorHeight(LX, LY + Eps) - LocalFloorHeight(LX, LY - Eps)) / (2.f * Eps);
	return FVector(-DhDx, -DhDy, 1.f).GetSafeNormal();
}

void ANeonArena::BuildTerrainMesh()
{
	if (!Terrain)
	{
		return;
	}

	Terrain->ClearAllMeshSections();

	const float Res = FMath::Max(TerrainResolution, 25.f);
	const int32 CountX = FMath::Max(2, FMath::CeilToInt(HalfExtents.X * 2.f / Res) + 1);
	const int32 CountY = FMath::Max(2, FMath::CeilToInt(HalfExtents.Y * 2.f / Res) + 1);
	const float StepX = HalfExtents.X * 2.f / (CountX - 1);
	const float StepY = HalfExtents.Y * 2.f / (CountY - 1);

	// ---- Section 0: the heightfield ----
	FSectionBuilder Floor;
	Floor.Vertices.Reserve(CountX * CountY);
	Floor.Triangles.Reserve((CountX - 1) * (CountY - 1) * 6);

	constexpr float Eps = 25.f;
	for (int32 J = 0; J < CountY; ++J)
	{
		const float Y = -HalfExtents.Y + J * StepY;
		for (int32 I = 0; I < CountX; ++I)
		{
			const float X = -HalfExtents.X + I * StepX;
			const float Z = LocalFloorHeight(X, Y);
			const float DhDx = (LocalFloorHeight(X + Eps, Y) - LocalFloorHeight(X - Eps, Y)) / (2.f * Eps);
			const float DhDy = (LocalFloorHeight(X, Y + Eps) - LocalFloorHeight(X, Y - Eps)) / (2.f * Eps);
			const FVector N = FVector(-DhDx, -DhDy, 1.f).GetSafeNormal();
			// 1 UV tile per 400 cm so a gridded material (Milestone 8) can read the floor.
			Floor.AddVertex(FVector(X, Y, Z), N, FVector2D(X / 400.f, Y / 400.f));
		}
	}
	for (int32 J = 0; J < CountY - 1; ++J)
	{
		for (int32 I = 0; I < CountX - 1; ++I)
		{
			const int32 A = J * CountX + I;
			const int32 B = A + 1;
			const int32 C = A + CountX;
			const int32 D = C + 1;
			Floor.AddQuad(A, B, C, D);
		}
	}
	Terrain->CreateMeshSection_LinearColor(0, Floor.Vertices, Floor.Triangles, Floor.Normals, Floor.UVs, Floor.Colors, Floor.Tangents, /*bCreateCollision*/ false);

	// ---- Section 1: a thin lip along the top edge of every ramp so the tiers read from a low camera ----
	FSectionBuilder Lips;
	constexpr float LipWidth = 16.f;
	constexpr float LipLift = 2.f;
	const float MinHalf = FMath::Min(HalfExtents.X, HalfExtents.Y);
	for (int32 Tier = 1; Tier <= TierCount; ++Tier)
	{
		const float Inset = Tier * TierSpacing;
		if (Inset + LipWidth >= MinHalf)
		{
			break; // this tier would be smaller than its own lip; the heightfield already ignores it
		}
		const float HX = HalfExtents.X - Inset;
		const float HY = HalfExtents.Y - Inset;
		const float Z = LocalFloorHeight(0.f, HY) + LipLift; // plateau height for this tier
		// Four strips just inside the plateau edge (inset side is the flat top of the ramp).
		Lips.AddFlatRect(-HX, HY - LipWidth, HX, HY, Z);               // north
		Lips.AddFlatRect(-HX, -HY, HX, -HY + LipWidth, Z);             // south
		Lips.AddFlatRect(HX - LipWidth, -HY, HX, HY, Z);               // east
		Lips.AddFlatRect(-HX, -HY, -HX + LipWidth, HY, Z);             // west
	}
	if (Lips.Vertices.Num() > 0)
	{
		Terrain->CreateMeshSection_LinearColor(1, Lips.Vertices, Lips.Triangles, Lips.Normals, Lips.UVs, Lips.Colors, Lips.Tangents, false);
	}

	if (PlaceholderMaterial)
	{
		Terrain->SetMaterial(0, PlaceholderMaterial);
		Terrain->SetMaterial(1, PlaceholderMaterial);
		if (UMaterialInstanceDynamic* FloorMID = Terrain->CreateAndSetMaterialInstanceDynamic(0))
		{
			FloorMID->SetVectorParameterValue(TEXT("Color"), FloorColor);
		}
		if (UMaterialInstanceDynamic* LipMID = Terrain->CreateAndSetMaterialInstanceDynamic(1))
		{
			LipMID->SetVectorParameterValue(TEXT("Color"), WallColor);
		}
	}

	if (TierCount > 0 && TierCount * TierSpacing >= MinHalf)
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("Arena: %d tiers at %.0f cm spacing do not fit in a half-extent of %.0f cm; the outer tiers are clipped."),
			TierCount, TierSpacing, MinHalf);
	}
}

// ---------------------------------------------------------------------------
// Cosmetics / camera
// ---------------------------------------------------------------------------

void ANeonArena::LayoutCosmetics()
{
	WallMIDs.Reset();
	for (UStaticMeshComponent* Wall : { WallNorth.Get(), WallSouth.Get(), WallEast.Get(), WallWest.Get() })
	{
		if (Wall && Wall->GetMaterial(0))
		{
			if (UMaterialInstanceDynamic* MID = Wall->CreateAndSetMaterialInstanceDynamic(0))
			{
				WallMIDs.Add(MID);
			}
		}
	}
	TintWalls(WallColor);
	LayoutWalls(HalfExtentsAt(RoundElapsed));

	if (DevLight)
	{
		DevLight->SetVisibility(bEnableDevLight);
	}
}

void ANeonArena::LayoutWalls(const FVector2D& Half)
{
	LastWallHalf = Half;

	// Engine cube is 100 cm. The walls stand on Z = 0 and are tall enough to clear every tier, so a wall
	// that has moved onto the pyramid never floats above the raised floor.
	const float TotalTierRise = FMath::Max(TierCount, 0) * FMath::Max(TierHeight, 0.f);
	const float Height = WallHeight + TotalTierRise;
	const float HalfWallZ = Height * 0.5f;
	const float LongX = (Half.X * 2.f + WallThickness) / 100.f;
	const float LongY = (Half.Y * 2.f + WallThickness) / 100.f;
	const float Thick = WallThickness / 100.f;
	const float Tall = Height / 100.f;

	if (WallNorth)
	{
		WallNorth->SetRelativeLocation(FVector(0.f, Half.Y + WallThickness * 0.5f, HalfWallZ));
		WallNorth->SetRelativeScale3D(FVector(LongX, Thick, Tall));
	}
	if (WallSouth)
	{
		WallSouth->SetRelativeLocation(FVector(0.f, -Half.Y - WallThickness * 0.5f, HalfWallZ));
		WallSouth->SetRelativeScale3D(FVector(LongX, Thick, Tall));
	}
	if (WallEast)
	{
		WallEast->SetRelativeLocation(FVector(Half.X + WallThickness * 0.5f, 0.f, HalfWallZ));
		WallEast->SetRelativeScale3D(FVector(Thick, LongY, Tall));
	}
	if (WallWest)
	{
		WallWest->SetRelativeLocation(FVector(-Half.X - WallThickness * 0.5f, 0.f, HalfWallZ));
		WallWest->SetRelativeScale3D(FVector(Thick, LongY, Tall));
	}
}

void ANeonArena::TintWalls(const FLinearColor& Color)
{
	for (UMaterialInstanceDynamic* MID : WallMIDs)
	{
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Color);
		}
	}
}

void ANeonArena::LayoutCamera()
{
	if (!Camera)
	{
		return;
	}

	Camera->SetFieldOfView(CameraFieldOfView);

	const float Pitch = FMath::Clamp(CameraPitch, -89.f, -30.f);
	const float PitchRad = FMath::DegreesToRadians(-Pitch);

	float Distance = CameraDistanceOverride;
	if (Distance <= 0.f)
	{
		// UE FieldOfView is horizontal. Fit the long axis (X) across the screen and the short axis
		// (Y) up the screen, allowing for foreshortening of the tilted view. Approximate; tunable.
		const float Aspect = Camera->AspectRatio > 0.f ? Camera->AspectRatio : 16.f / 9.f;
		const float HalfHFov = FMath::DegreesToRadians(CameraFieldOfView * 0.5f);
		const float HalfVFov = FMath::Atan(FMath::Tan(HalfHFov) / Aspect);

		const float DistForX = (HalfExtents.X * CameraFitPadding) / FMath::Tan(HalfHFov);
		const float DistForY = (HalfExtents.Y * CameraFitPadding) / (FMath::Tan(HalfVFov) * FMath::Max(FMath::Sin(PitchRad), 0.2f));
		Distance = FMath::Max(DistForX, DistForY);
	}

	// Camera sits on the +Y side looking toward -Y (yaw -90), so world +X is screen-right.
	const FVector Offset(0.f, Distance * FMath::Cos(PitchRad), Distance * FMath::Sin(PitchRad));
	Camera->SetRelativeLocation(Offset);
	Camera->SetRelativeRotation(FRotator(Pitch, -90.f, 0.f));
}

void ANeonArena::TintMesh(UMeshComponent* Mesh, const FLinearColor& Color) const
{
	if (!Mesh || !Mesh->GetMaterial(0))
	{
		return;
	}
	if (UMaterialInstanceDynamic* MID = Mesh->CreateAndSetMaterialInstanceDynamic(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), Color);
	}
}
