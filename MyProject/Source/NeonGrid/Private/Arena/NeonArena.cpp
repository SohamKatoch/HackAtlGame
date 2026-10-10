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

		/** Same rectangle, facing down so the underside of a deck reads as a ceiling. */
		void AddCeilingRect(float X0, float Y0, float X1, float Y1, float Z)
		{
			const FVector Down = FVector(0.f, 0.f, -1.f);
			const int32 A = AddVertex(FVector(X0, Y0, Z), Down, FVector2D(0.f, 0.f));
			const int32 B = AddVertex(FVector(X1, Y0, Z), Down, FVector2D(1.f, 0.f));
			const int32 C = AddVertex(FVector(X0, Y1, Z), Down, FVector2D(0.f, 1.f));
			const int32 D = AddVertex(FVector(X1, Y1, Z), Down, FVector2D(1.f, 1.f));
			Triangles.Append({ A, B, D, A, D, C });
		}

		void AddVerticalQuad(const FVector& BottomA, const FVector& BottomB, float ZTop)
		{
			const FVector Delta = BottomB - BottomA;
			FVector N(-Delta.Y, Delta.X, 0.f);
			N.Normalize();
			const int32 A = AddVertex(BottomA, N, FVector2D(0.f, 0.f));
			const int32 B = AddVertex(BottomB, N, FVector2D(1.f, 0.f));
			const int32 C = AddVertex(FVector(BottomA.X, BottomA.Y, ZTop), N, FVector2D(0.f, 1.f));
			const int32 D = AddVertex(FVector(BottomB.X, BottomB.Y, ZTop), N, FVector2D(1.f, 1.f));
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

	// Fill from the opposite side and slightly below so ramp undersides, skirts, and the far side of every deck stay lit.
	DevFillLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("DevFillLight"));
	DevFillLight->SetupAttachment(Root);
	DevFillLight->SetRelativeRotation(FRotator(20.f, 215.f, 0.f));
	DevFillLight->SetIntensity(2.2f);
	DevFillLight->SetLightColor(FLinearColor(0.55f, 0.75f, 1.0f));
	DevFillLight->SetCastShadows(false);
}

void ANeonArena::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ActiveLayout = 1;
	BuildTerrainMesh();
	LayoutCosmetics();
	LayoutCamera();
}

void ANeonArena::BeginPlay()
{
	Super::BeginPlay();
	ActiveLayout = 1;
	BuildTerrainMesh();
	LayoutCosmetics();
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

float ANeonArena::GetHexWallRadius(FVector2D Half) const
{
	// Pointy at ±X, so the flat-to-flat height is R * √3. Fit that inside the rectangle, then the six walls are the boundary.
	constexpr float Sqrt3Over2 = 0.86602540378f;
	const float ByX = FMath::Max(Half.X, 400.f);
	const float ByY = FMath::Max(Half.Y, 400.f) / Sqrt3Over2;
	return FMath::Max(FMath::Min(ByX, ByY) - 40.f, 400.f);
}

bool ANeonArena::IsInsideBounds(FVector2D WorldXY, float Padding) const
{
	return IsInsideBoundsAt(WorldXY, Padding, 0.f);
}

bool ANeonArena::IsInsideBoundsAt(FVector2D WorldXY, float Padding, float SecondsAhead) const
{
	const float Elapsed = RoundElapsed < 0.f ? RoundElapsed : RoundElapsed + FMath::Max(SecondsAhead, 0.f);
	const FVector2D Half = HalfExtentsAt(Elapsed);
	const FVector Center = GetActorLocation();
	const FVector2D Local(WorldXY.X - Center.X, WorldXY.Y - Center.Y);
	const float Pad = FMath::Max(Padding, 0.f);
	if (!IsHexLayout())
	{
		return FMath::Abs(Local.X) <= Half.X - Pad && FMath::Abs(Local.Y) <= Half.Y - Pad;
	}
	constexpr float Sqrt3Over2 = 0.86602540378f;
	const float R = FMath::Max(GetHexWallRadius(Half) - Pad, 50.f);
	const float X = FMath::Abs(Local.X);
	const float Y = FMath::Abs(Local.Y);
	return (X <= R) && (X * 0.5f + Y * Sqrt3Over2 <= R);
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

	float LocalX = 0.f;
	float LocalY = 0.f;
	float Heading = 0.f;

	if (IsHexLayout())
	{
		// Open ground: outside the deck above (0.70 of the floor) and clear of the three ramps at 30, 150, and 270 degrees.
		// Three open faces plus the east point, each facing the middle.
		const float FloorR = FMath::Max(GetHexWallRadius(HalfExtents) - 30.f, 400.f);
		const float Apothem = FloorR * 0.86602540378f;
		const float Angles[4] = { PI * 0.5f, PI * 7.f / 6.f, PI * 11.f / 6.f, 0.f };
		const float Reach[4] = { Apothem, Apothem, Apothem, FloorR };
		const float Ang = Angles[Index];
		const float Dist = Reach[Index] * 0.80f;
		LocalX = Dist * FMath::Cos(Ang);
		LocalY = Dist * FMath::Sin(Ang);
		Heading = FMath::RadiansToDegrees(FMath::Atan2(-LocalY, -LocalX));
	}
	else
	{
		// 0: left-bottom facing +X   1: right-bottom facing -X
		// 2: left-top facing +X      3: right-top facing -X
		const float SignX = (Index % 2 == 0) ? -1.f : 1.f;
		const float SignY = (Index < 2) ? -1.f : 1.f;
		Heading = (Index % 2 == 0) ? 0.f : 180.f;
		LocalX = SignX * HalfExtents.X * SpawnInsetFraction.X;
		LocalY = SignY * HalfExtents.Y * SpawnInsetFraction.Y;
	}

	const FVector Center = GetActorLocation();
	const FVector2D WorldXY(Center.X + LocalX, Center.Y + LocalY);
	const FVector Location(WorldXY.X, WorldXY.Y, GetFloorHeightOnLayer(WorldXY, 0) + VehicleSpawnHeight);

	return FTransform(FRotator(0.f, Heading, 0.f), Location, FVector::OneVector);
}

FTransform ANeonArena::GetRandomSpawnTransform(FRandomStream& Rng, const TArray<FVector2D>& Avoid) const
{
	const float Margin = 1600.f;
	const float MinSep = 2200.f;
	const float MinSepSq = MinSep * MinSep;
	const float HalfX = FMath::Max(HalfExtents.X - Margin, 400.f);
	const float HalfY = FMath::Max(HalfExtents.Y - Margin, 400.f);

	const FVector Center = GetActorLocation();
	const float FloorR = IsHexLayout() ? FMath::Max(GetHexWallRadius(HalfExtents) - 30.f, 400.f) : 0.f;
	FVector2D Pick(0.f, 0.f);
	bool bFound = false;
	for (int32 Attempt = 0; Attempt < 40 && !bFound; ++Attempt)
	{
		FVector2D Candidate;
		if (IsHexLayout())
		{
			// The ring of open ground between the first upper deck and the hex wall.
			const float Ang = Rng.FRandRange(0.f, 2.f * PI);
			const float Dist = Rng.FRandRange(FloorR * 0.74f, FloorR * 0.90f);
			const float RampAngles[3] = { PI / 6.f, PI / 6.f + 2.f * PI / 3.f, PI / 6.f + 4.f * PI / 3.f };
			bool bOnRamp = false;
			for (float RampAng : RampAngles)
			{
				const float Delta = FMath::Abs(FMath::FindDeltaAngleRadians(Ang, RampAng));
				if (Delta < FMath::DegreesToRadians(14.f))
				{
					bOnRamp = true;
					break;
				}
			}
			if (bOnRamp)
			{
				continue;
			}
			Candidate = FVector2D(Dist * FMath::Cos(Ang), Dist * FMath::Sin(Ang));
		}
		else
		{
			Candidate = FVector2D(Rng.FRandRange(-HalfX, HalfX), Rng.FRandRange(-HalfY, HalfY));
		}
		const FVector2D World(Center.X + Candidate.X, Center.Y + Candidate.Y);
		if (!HasFloorOnLayer(World, 0) || !IsInsideBounds(World, 200.f))
		{
			continue;
		}
		bool bClear = true;
		for (const FVector2D& Other : Avoid)
		{
			if ((Other - World).SizeSquared() < MinSepSq)
			{
				bClear = false;
				break;
			}
		}
		if (bClear)
		{
			Pick = Candidate;
			bFound = true;
		}
	}
	if (!bFound)
	{
		if (IsHexLayout())
		{
			return GetSpawnTransform(Rng.RandRange(0, 3));
		}
		Pick = FVector2D(Rng.FRandRange(-HalfX, HalfX), Rng.FRandRange(-HalfY, HalfY));
	}

	// Face the middle, with a wide jitter so the opening line isn't a scripted cross.
	const float Inward = FMath::RadiansToDegrees(FMath::Atan2(-Pick.Y, -Pick.X));
	const float Heading = Inward + Rng.FRandRange(-75.f, 75.f);

	const FVector2D WorldXY(Center.X + Pick.X, Center.Y + Pick.Y);
	const FVector Location(WorldXY.X, WorldXY.Y, GetFloorHeightOnLayer(WorldXY, 0) + VehicleSpawnHeight);

	return FTransform(FRotator(0.f, Heading, 0.f), Location, FVector::OneVector);
}

// ---------------------------------------------------------------------------
// Terrain: concentric tiers (D25), one-way hills, and stacked decks (D30).
// ---------------------------------------------------------------------------

namespace
{
	enum class ENeonRampKind : uint8 { Hill, Connector };

	enum class ENeonDeckShape : uint8 { Rect, Hex };

	struct FNeonAuthoredRamp
	{
		FVector2D Origin;
		FVector2D Dir;
		float Length = 0.f;
		float HalfWidth = 0.f;
		ENeonRampKind Kind = ENeonRampKind::Hill;
		int32 FromLayer = 0;
		int32 ToLayer = 0;
	};

	struct FNeonDeck
	{
		int32 Layer = 0;
		float Height = 0.f;
		ENeonDeckShape Shape = ENeonDeckShape::Rect;
		FVector2D Center = FVector2D::ZeroVector;
		FVector2D Half = FVector2D::ZeroVector;
		float HexRadius = 0.f;
	};

	constexpr float HexSqrt3Over2 = 0.86602540378f;

	bool PointInFlatHex(const FVector2D& P, const FVector2D& C, float R)
	{
		if (R <= 1.f)
		{
			return false;
		}
		const FVector2D D = P - C;
		const float X = FMath::Abs(D.X);
		const float Y = FMath::Abs(D.Y);
		return (X <= R) && (X * 0.5f + Y * HexSqrt3Over2 <= R);
	}

	bool DeckContains(const FVector2D& LocalXY, const FNeonDeck& D)
	{
		const FVector2D P = LocalXY - D.Center;
		if (D.Shape == ENeonDeckShape::Hex)
		{
			return PointInFlatHex(LocalXY, D.Center, D.HexRadius);
		}
		return FMath::Abs(P.X) <= D.Half.X && FMath::Abs(P.Y) <= D.Half.Y;
	}

	bool RampCoords(const FVector2D& LocalXY, const FNeonAuthoredRamp& R, float& Along, float& Across)
	{
		const float DX = LocalXY.X - R.Origin.X;
		const float DY = LocalXY.Y - R.Origin.Y;
		Along = DX * R.Dir.X + DY * R.Dir.Y;
		Across = DX * -R.Dir.Y + DY * R.Dir.X;
		return Along >= 0.f && Along <= R.Length && FMath::Abs(Across) <= R.HalfWidth;
	}

	void CollectDecks(const ANeonArena& A, TArray<FNeonDeck>& Out)
	{
		// No stacked floors. Every layout plays on the ground.
		(void)A;
		Out.Reset();
	}

	void AddHill(TArray<FNeonAuthoredRamp>& Out, const FVector2D& Origin, const FVector2D& Dir, const ANeonArena& A)
	{
		FNeonAuthoredRamp R;
		R.Origin = Origin;
		R.Dir = Dir;
		R.Length = A.LaunchRampLength;
		R.HalfWidth = A.LaunchRampHalfWidth;
		R.Kind = ENeonRampKind::Hill;
		Out.Add(R);
	}

	void CollectRamps(const ANeonArena& A, TArray<FNeonAuthoredRamp>& Out)
	{
		Out.Reset();
		const FVector2D H = A.HalfExtents;

		if (!A.IsHexLayout() && A.bLaunchRamps && A.LaunchRampHeight > 0.f)
		{
			switch (A.ActiveLayout)
			{
			case 2:
				AddHill(Out, FVector2D(-0.06f * H.X,  0.f),        FVector2D( 1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.06f * H.X,  0.f),        FVector2D(-1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.f,         0.08f * H.Y), FVector2D( 0.f, -1.f), A);
				AddHill(Out, FVector2D( 0.f,        -0.08f * H.Y), FVector2D( 0.f,  1.f), A);
				break;
			case 3:
				AddHill(Out, FVector2D(-0.62f * H.X,  0.55f * H.Y), FVector2D( 1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.62f * H.X,  0.55f * H.Y), FVector2D(-1.f,  0.f), A);
				AddHill(Out, FVector2D(-0.62f * H.X, -0.55f * H.Y), FVector2D( 1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.62f * H.X, -0.55f * H.Y), FVector2D(-1.f,  0.f), A);
				break;
			default:
				AddHill(Out, FVector2D(-0.40f * H.X,  0.30f * H.Y), FVector2D( 1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.40f * H.X, -0.30f * H.Y), FVector2D(-1.f,  0.f), A);
				AddHill(Out, FVector2D( 0.18f * H.X,  0.38f * H.Y), FVector2D( 0.f, -1.f), A);
				AddHill(Out, FVector2D(-0.18f * H.X, -0.38f * H.Y), FVector2D( 0.f,  1.f), A);
				break;
			}
		}
	}

	float HillHeightAt(const ANeonArena& Arena, float X, float Y)
	{
		if (!Arena.bLaunchRamps || Arena.LaunchRampHeight <= 0.f || Arena.IsHexLayout())
		{
			return 0.f;
		}

		TArray<FNeonAuthoredRamp> Ramps;
		CollectRamps(Arena, Ramps);

		const float Lip = FMath::Clamp(Arena.LaunchRampLipFraction, 0.05f, 0.5f);
		float Height = 0.f;
		for (const FNeonAuthoredRamp& R : Ramps)
		{
			if (R.Kind != ENeonRampKind::Hill)
			{
				continue;
			}
			const float Edge = FMath::Clamp(Arena.LaunchRampHeight * 2.4f, 400.f, R.HalfWidth * 0.85f);
			const float Climb = FMath::Max(R.Length * (1.f - Lip), 1.f);
			const float DX = X - R.Origin.X;
			const float DY = Y - R.Origin.Y;
			const float Along = DX * R.Dir.X + DY * R.Dir.Y;
			const float Across = DX * -R.Dir.Y + DY * R.Dir.X;
			if (Along < -Edge || Along > R.Length + Edge)
			{
				continue;
			}
			const float AW = FMath::Abs(Across);
			if (AW > R.HalfWidth)
			{
				continue;
			}

			float Profile = 0.f;
			if (Along > 0.f && Along < Climb)
			{
				Profile = FMath::SmoothStep(0.f, 1.f, Along / Climb);
			}
			else if (Along >= Climb && Along < R.Length)
			{
				Profile = 1.f - FMath::SmoothStep(0.f, 1.f, (Along - Climb) / FMath::Max(R.Length - Climb, 1.f));
			}

			float Side = 1.f;
			if (AW > R.HalfWidth - Edge)
			{
				Side = 1.f - FMath::SmoothStep(0.f, 1.f, (AW - (R.HalfWidth - Edge)) / FMath::Max(Edge, 1.f));
			}

			float End = 1.f;
			if (Along < 0.f)
			{
				End = 1.f - FMath::SmoothStep(0.f, 1.f, -Along / Edge);
			}
			else if (Along > R.Length)
			{
				End = 1.f - FMath::SmoothStep(0.f, 1.f, (Along - R.Length) / Edge);
			}

			Height = FMath::Max(Height, Arena.LaunchRampHeight * Profile * Side * End);
		}
		return Height;
	}

	float PyramidHeightAt(const ANeonArena& Arena, float LocalX, float LocalY)
	{
		const int32 Tiers = Arena.GetActiveTierCount();
		if (Tiers <= 0 || Arena.TierHeight <= 0.f)
		{
			return 0.f;
		}
		const float AbsX = FMath::Clamp(FMath::Abs(LocalX), 0.f, Arena.HalfExtents.X);
		const float AbsY = FMath::Clamp(FMath::Abs(LocalY), 0.f, Arena.HalfExtents.Y);
		const float Inset = FMath::Min(Arena.HalfExtents.X - AbsX, Arena.HalfExtents.Y - AbsY);
		const float Ramp = FMath::Max(Arena.RampWidth, 1.f);
		float Height = 0.f;
		for (int32 Tier = 1; Tier <= Tiers; ++Tier)
		{
			const float Top = Tier * Arena.TierSpacing;
			const float Foot = Top - Ramp;
			const float T = FMath::Clamp((Inset - Foot) / Ramp, 0.f, 1.f);
			Height += Arena.TierHeight * FMath::SmoothStep(0.f, 1.f, T);
		}
		return Height;
	}

	float DeckHeightForLayer(const TArray<FNeonDeck>& Decks, int32 Layer)
	{
		for (const FNeonDeck& D : Decks)
		{
			if (D.Layer == Layer)
			{
				return D.Height;
			}
		}
		return 0.f;
	}

	float ConnectorHeightAt(const ANeonArena& Arena, const FNeonAuthoredRamp& R, float Along, const TArray<FNeonDeck>& Decks)
	{
		const float T = FMath::SmoothStep(0.f, 1.f, FMath::Clamp(Along / FMath::Max(R.Length, 1.f), 0.f, 1.f));
		const float Z0 = DeckHeightForLayer(Decks, R.FromLayer) + (R.FromLayer == 0 ? PyramidHeightAt(Arena, R.Origin.X, R.Origin.Y) : 0.f);
		const float Z1 = DeckHeightForLayer(Decks, R.ToLayer);
		return FMath::Lerp(Z0, Z1, T);
	}

	/** Deck surface, solid sides down to BelowZ, and two glowing rails. The sides keep a bridge from reading as a floating sheet. */
	void AddRampMesh(FSectionBuilder& Deck, FSectionBuilder& Sides, FSectionBuilder& Rails, const FNeonAuthoredRamp& R, float Z0, float Z1, float BelowZ)
	{
		const FVector2D Perp(-R.Dir.Y, R.Dir.X);
		constexpr int32 Steps = 10;
		constexpr float RailWidth = 70.f;
		constexpr float RailLift = 14.f;
		for (int32 i = 0; i < Steps; ++i)
		{
			const float T0 = static_cast<float>(i) / Steps;
			const float T1 = static_cast<float>(i + 1) / Steps;
			const float H0 = FMath::Lerp(Z0, Z1, FMath::SmoothStep(0.f, 1.f, T0));
			const float H1 = FMath::Lerp(Z0, Z1, FMath::SmoothStep(0.f, 1.f, T1));
			const FVector2D P0 = R.Origin + R.Dir * (R.Length * T0);
			const FVector2D P1 = R.Origin + R.Dir * (R.Length * T1);
			const FVector A(P0.X - Perp.X * R.HalfWidth, P0.Y - Perp.Y * R.HalfWidth, H0);
			const FVector C(P0.X + Perp.X * R.HalfWidth, P0.Y + Perp.Y * R.HalfWidth, H0);
			const FVector BB(P1.X - Perp.X * R.HalfWidth, P1.Y - Perp.Y * R.HalfWidth, H1);
			const FVector D(P1.X + Perp.X * R.HalfWidth, P1.Y + Perp.Y * R.HalfWidth, H1);
			const FVector N = FVector::CrossProduct(BB - A, C - A).GetSafeNormal();
			const int32 I0 = Deck.AddVertex(A, N, FVector2D(T0, 0.f));
			const int32 I1 = Deck.AddVertex(BB, N, FVector2D(T1, 0.f));
			const int32 I2 = Deck.AddVertex(C, N, FVector2D(T0, 1.f));
			const int32 I3 = Deck.AddVertex(D, N, FVector2D(T1, 1.f));
			Deck.AddQuad(I0, I1, I2, I3);

			// Sides: from the deck edge straight down. Left side faces -Perp, right side faces +Perp.
			{
				const FVector LN = FVector(-Perp.X, -Perp.Y, 0.f);
				const int32 L0 = Sides.AddVertex(FVector(A.X, A.Y, BelowZ), LN, FVector2D(T0, 0.f));
				const int32 L1 = Sides.AddVertex(FVector(BB.X, BB.Y, BelowZ), LN, FVector2D(T1, 0.f));
				const int32 L2 = Sides.AddVertex(A, LN, FVector2D(T0, 1.f));
				const int32 L3 = Sides.AddVertex(BB, LN, FVector2D(T1, 1.f));
				Sides.Triangles.Append({ L0, L2, L3, L0, L3, L1 });

				const FVector RN = FVector(Perp.X, Perp.Y, 0.f);
				const int32 R0 = Sides.AddVertex(FVector(C.X, C.Y, BelowZ), RN, FVector2D(T0, 0.f));
				const int32 R1 = Sides.AddVertex(FVector(D.X, D.Y, BelowZ), RN, FVector2D(T1, 0.f));
				const int32 R2 = Sides.AddVertex(C, RN, FVector2D(T0, 1.f));
				const int32 R3 = Sides.AddVertex(D, RN, FVector2D(T1, 1.f));
				Sides.Triangles.Append({ R0, R3, R2, R0, R1, R3 });
			}

			// Rails.
			for (float Side : { -1.f, 1.f })
			{
				const float Outer = R.HalfWidth * Side;
				const float Inner = (R.HalfWidth - RailWidth) * Side;
				const FVector Q0(P0.X + Perp.X * Inner, P0.Y + Perp.Y * Inner, H0 + RailLift);
				const FVector Q1(P0.X + Perp.X * Outer, P0.Y + Perp.Y * Outer, H0 + RailLift);
				const FVector Q2(P1.X + Perp.X * Inner, P1.Y + Perp.Y * Inner, H1 + RailLift);
				const FVector Q3(P1.X + Perp.X * Outer, P1.Y + Perp.Y * Outer, H1 + RailLift);
				const int32 J0 = Rails.AddVertex(Q0, N, FVector2D(T0, 0.f));
				const int32 J1 = Rails.AddVertex(Q1, N, FVector2D(T0, 1.f));
				const int32 J2 = Rails.AddVertex(Q2, N, FVector2D(T1, 0.f));
				const int32 J3 = Rails.AddVertex(Q3, N, FVector2D(T1, 1.f));
				if (Side < 0.f)
				{
					Rails.Triangles.Append({ J0, J2, J3, J0, J3, J1 });
				}
				else
				{
					Rails.Triangles.Append({ J0, J3, J2, J0, J1, J3 });
				}
			}
		}
	}

	/** Where a connector's top end crosses the edge E0-E1, as a parameter interval along the edge. */
	bool RampGapOnEdge(const FNeonAuthoredRamp& R, int32 DeckLayer, const FVector2D& E0, const FVector2D& E1, float& OutT0, float& OutT1)
	{
		if (R.Kind != ENeonRampKind::Connector || R.ToLayer != DeckLayer)
		{
			return false;
		}
		const FVector2D Edge = E1 - E0;
		const float LenSq = FMath::Max(Edge.SizeSquared(), 1.f);
		const FVector2D Top = R.Origin + R.Dir * R.Length;
		const FVector2D Perp(-R.Dir.Y, R.Dir.X);
		// The ramp must actually touch this edge.
		const float Along = FVector2D::DotProduct(Top - E0, Edge) / LenSq;
		const FVector2D Closest = E0 + Edge * FMath::Clamp(Along, 0.f, 1.f);
		if ((Closest - Top).SizeSquared() > 350.f * 350.f)
		{
			return false;
		}
		const FVector2D CapA = Top - Perp * (R.HalfWidth + 40.f);
		const FVector2D CapB = Top + Perp * (R.HalfWidth + 40.f);
		const float TA = FVector2D::DotProduct(CapA - E0, Edge) / LenSq;
		const float TB = FVector2D::DotProduct(CapB - E0, Edge) / LenSq;
		OutT0 = FMath::Clamp(FMath::Min(TA, TB), 0.f, 1.f);
		OutT1 = FMath::Clamp(FMath::Max(TA, TB), 0.f, 1.f);
		return OutT1 > OutT0;
	}

	/** Flat band just inside an edge, skipping the stretch a ramp arrives through. Inward points into the deck. */
	void AddEdgeBand(FSectionBuilder& Rim, const FVector2D& E0, const FVector2D& E1, const FVector2D& Inward, float Band, float Z,
		const TArray<FNeonAuthoredRamp>& Ramps, int32 DeckLayer)
	{
		TArray<TPair<float, float>> Spans;
		Spans.Add(TPair<float, float>(0.f, 1.f));
		for (const FNeonAuthoredRamp& R : Ramps)
		{
			float G0 = 0.f, G1 = 0.f;
			if (!RampGapOnEdge(R, DeckLayer, E0, E1, G0, G1))
			{
				continue;
			}
			TArray<TPair<float, float>> Next;
			for (const TPair<float, float>& S : Spans)
			{
				if (G1 <= S.Key || G0 >= S.Value)
				{
					Next.Add(S);
					continue;
				}
				if (G0 > S.Key)
				{
					Next.Add(TPair<float, float>(S.Key, G0));
				}
				if (G1 < S.Value)
				{
					Next.Add(TPair<float, float>(G1, S.Value));
				}
			}
			Spans = MoveTemp(Next);
		}

		const FVector Up = FVector::UpVector;
		for (const TPair<float, float>& S : Spans)
		{
			const FVector2D A = FMath::Lerp(E0, E1, S.Key);
			const FVector2D B = FMath::Lerp(E0, E1, S.Value);
			const FVector2D AI = A + Inward * Band;
			const FVector2D BI = B + Inward * Band;
			const int32 V0 = Rim.AddVertex(FVector(A.X, A.Y, Z), Up, FVector2D(0.f, 0.f));
			const int32 V1 = Rim.AddVertex(FVector(B.X, B.Y, Z), Up, FVector2D(1.f, 0.f));
			const int32 V2 = Rim.AddVertex(FVector(AI.X, AI.Y, Z), Up, FVector2D(0.f, 1.f));
			const int32 V3 = Rim.AddVertex(FVector(BI.X, BI.Y, Z), Up, FVector2D(1.f, 1.f));
			// Two triangles, wound so they face up regardless of edge direction.
			const FVector N = FVector::CrossProduct(FVector(B.X - A.X, B.Y - A.Y, 0.f), FVector(Inward.X, Inward.Y, 0.f));
			if (N.Z >= 0.f)
			{
				Rim.Triangles.Append({ V0, V2, V3, V0, V3, V1 });
			}
			else
			{
				Rim.Triangles.Append({ V0, V3, V2, V0, V1, V3 });
			}
		}
	}

	void AddHexDeck(FSectionBuilder& Top, FSectionBuilder& Skirt, FSectionBuilder& Ceiling, const FNeonDeck& D, float Res, float BelowZ,
		const TArray<FNeonAuthoredRamp>& Ramps)
	{
		const float Bound = D.HexRadius + Res;
		const int32 Count = FMath::Max(4, FMath::CeilToInt(Bound * 2.f / Res) + 1);
		const float Step = Bound * 2.f / (Count - 1);
		TArray<int32> Index;
		Index.Init(INDEX_NONE, Count * Count);
		const FVector Up = FVector::UpVector;
		for (int32 J = 0; J < Count; ++J)
		{
			const float Y = D.Center.Y - Bound + J * Step;
			for (int32 I = 0; I < Count; ++I)
			{
				const float X = D.Center.X - Bound + I * Step;
				if (PointInFlatHex(FVector2D(X, Y), D.Center, D.HexRadius))
				{
					Index[J * Count + I] = Top.AddVertex(FVector(X, Y, D.Height), Up, FVector2D(I / float(Count - 1), J / float(Count - 1)));
				}
			}
		}
		for (int32 J = 0; J < Count - 1; ++J)
		{
			for (int32 I = 0; I < Count - 1; ++I)
			{
				const int32 A = Index[J * Count + I];
				const int32 B = Index[J * Count + I + 1];
				const int32 C = Index[(J + 1) * Count + I];
				const int32 Dd = Index[(J + 1) * Count + I + 1];
				if (A != INDEX_NONE && B != INDEX_NONE && C != INDEX_NONE && Dd != INDEX_NONE)
				{
					Top.AddQuad(A, B, C, Dd);
				}
			}
		}

		const float ZBot = BelowZ;
		const FVector Down = FVector(0.f, 0.f, -1.f);
		const float CeilingZ = D.Height - 12.f;
		for (int32 J = 0; J < Count; ++J)
		{
			const float Y = D.Center.Y - Bound + J * Step;
			for (int32 I = 0; I < Count; ++I)
			{
				const float X = D.Center.X - Bound + I * Step;
				if (PointInFlatHex(FVector2D(X, Y), D.Center, D.HexRadius - 8.f))
				{
					Index[J * Count + I] = Ceiling.AddVertex(FVector(X, Y, CeilingZ), Down, FVector2D(I / float(Count - 1), J / float(Count - 1)));
				}
				else
				{
					Index[J * Count + I] = INDEX_NONE;
				}
			}
		}
		for (int32 J = 0; J < Count - 1; ++J)
		{
			for (int32 I = 0; I < Count - 1; ++I)
			{
				const int32 A = Index[J * Count + I];
				const int32 B = Index[J * Count + I + 1];
				const int32 C = Index[(J + 1) * Count + I];
				const int32 Dd = Index[(J + 1) * Count + I + 1];
				if (A != INDEX_NONE && B != INDEX_NONE && C != INDEX_NONE && Dd != INDEX_NONE)
				{
					Ceiling.Triangles.Append({ A, B, Dd, A, Dd, C });
				}
			}
		}
		for (int32 i = 0; i < 6; ++i)
		{
			const float A0 = FMath::DegreesToRadians(i * 60.f);
			const float A1 = FMath::DegreesToRadians((i + 1) * 60.f);
			const FVector2D E0(D.Center.X + D.HexRadius * FMath::Cos(A0), D.Center.Y + D.HexRadius * FMath::Sin(A0));
			const FVector2D E1(D.Center.X + D.HexRadius * FMath::Cos(A1), D.Center.Y + D.HexRadius * FMath::Sin(A1));
			TArray<TPair<float, float>> Spans;
			Spans.Add(TPair<float, float>(0.f, 1.f));
			for (const FNeonAuthoredRamp& R : Ramps)
			{
				float G0 = 0.f, G1 = 0.f;
				if (!RampGapOnEdge(R, D.Layer, E0, E1, G0, G1))
				{
					continue;
				}
				TArray<TPair<float, float>> Next;
				for (const TPair<float, float>& S : Spans)
				{
					if (G1 <= S.Key || G0 >= S.Value)
					{
						Next.Add(S);
						continue;
					}
					if (G0 > S.Key)
					{
						Next.Add(TPair<float, float>(S.Key, G0));
					}
					if (G1 < S.Value)
					{
						Next.Add(TPair<float, float>(G1, S.Value));
					}
				}
				Spans = MoveTemp(Next);
			}
			for (const TPair<float, float>& S : Spans)
			{
				const FVector2D A = FMath::Lerp(E0, E1, S.Key);
				const FVector2D B = FMath::Lerp(E0, E1, S.Value);
				Skirt.AddVerticalQuad(FVector(A.X, A.Y, ZBot), FVector(B.X, B.Y, ZBot), D.Height);
			}
		}
	}

	/** Glowing band around the top of a hex deck. Flat on the floor, with a gap wherever a ramp arrives. */
	void AddHexRim(FSectionBuilder& Rim, const FNeonDeck& D, const TArray<FNeonAuthoredRamp>& Ramps)
	{
		const float Z = D.Height + 16.f;
		for (int32 i = 0; i < 6; ++i)
		{
			const float A0 = FMath::DegreesToRadians(static_cast<float>(i) * 60.f);
			const float A1 = FMath::DegreesToRadians(static_cast<float>(i + 1) * 60.f);
			const FVector2D E0(D.Center.X + D.HexRadius * FMath::Cos(A0), D.Center.Y + D.HexRadius * FMath::Sin(A0));
			const FVector2D E1(D.Center.X + D.HexRadius * FMath::Cos(A1), D.Center.Y + D.HexRadius * FMath::Sin(A1));
			const FVector2D Mid = (E0 + E1) * 0.5f;
			const FVector2D Inward = (D.Center - Mid).GetSafeNormal();
			AddEdgeBand(Rim, E0, E1, Inward, 110.f, Z, Ramps, D.Layer);
		}
	}

	void AddRectDeck(FSectionBuilder& Top, FSectionBuilder& Skirt, FSectionBuilder& Ceiling, FSectionBuilder& Rim, const FNeonDeck& D, float BelowZ,
		const TArray<FNeonAuthoredRamp>& Ramps)
	{
		const float X0 = D.Center.X - D.Half.X;
		const float X1 = D.Center.X + D.Half.X;
		const float Y0 = D.Center.Y - D.Half.Y;
		const float Y1 = D.Center.Y + D.Half.Y;
		Top.AddFlatRect(X0, Y0, X1, Y1, D.Height);
		if (D.Layer == 0)
		{
			return;
		}
		Ceiling.AddCeilingRect(X0 + 8.f, Y0 + 8.f, X1 - 8.f, Y1 - 8.f, D.Height - 12.f);
		Skirt.AddVerticalQuad(FVector(X0, Y0, BelowZ), FVector(X1, Y0, BelowZ), D.Height);
		Skirt.AddVerticalQuad(FVector(X1, Y0, BelowZ), FVector(X1, Y1, BelowZ), D.Height);
		Skirt.AddVerticalQuad(FVector(X1, Y1, BelowZ), FVector(X0, Y1, BelowZ), D.Height);
		Skirt.AddVerticalQuad(FVector(X0, Y1, BelowZ), FVector(X0, Y0, BelowZ), D.Height);
		constexpr float Band = 90.f;
		const float Z = D.Height + 16.f;
		AddEdgeBand(Rim, FVector2D(X0, Y1), FVector2D(X1, Y1), FVector2D(0.f, -1.f), Band, Z, Ramps, D.Layer); // north
		AddEdgeBand(Rim, FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(0.f,  1.f), Band, Z, Ramps, D.Layer); // south
		AddEdgeBand(Rim, FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(-1.f, 0.f), Band, Z, Ramps, D.Layer); // east
		AddEdgeBand(Rim, FVector2D(X0, Y0), FVector2D(X0, Y1), FVector2D( 1.f, 0.f), Band, Z, Ramps, D.Layer); // west
	}

	void AddChevrons(FSectionBuilder& Marks, const FNeonAuthoredRamp& R, float Z0, float Z1)
	{
		const FVector2D Perp(-R.Dir.Y, R.Dir.X);
		constexpr int32 StripeCount = 6;
		const FVector Up = FVector::UpVector;
		for (int32 i = 0; i < StripeCount; ++i)
		{
			const float T0 = (static_cast<float>(i) + 0.15f) / StripeCount;
			const float T1 = (static_cast<float>(i) + 0.55f) / StripeCount;
			const FVector2D A = R.Origin + R.Dir * (R.Length * T0);
			const FVector2D B = R.Origin + R.Dir * (R.Length * T1);
			const float H0 = FMath::Lerp(Z0, Z1, FMath::SmoothStep(0.f, 1.f, T0)) + 16.f;
			const float H1 = FMath::Lerp(Z0, Z1, FMath::SmoothStep(0.f, 1.f, T1)) + 16.f;
			constexpr float Half = 70.f;
			const int32 I0 = Marks.AddVertex(FVector(A.X - Perp.X * Half, A.Y - Perp.Y * Half, H0), Up, FVector2D(0.f, 0.f));
			const int32 I1 = Marks.AddVertex(FVector(B.X - Perp.X * Half, B.Y - Perp.Y * Half, H1), Up, FVector2D(1.f, 0.f));
			const int32 I2 = Marks.AddVertex(FVector(A.X + Perp.X * Half, A.Y + Perp.Y * Half, H0), Up, FVector2D(0.f, 1.f));
			const int32 I3 = Marks.AddVertex(FVector(B.X + Perp.X * Half, B.Y + Perp.Y * Half, H1), Up, FVector2D(1.f, 1.f));
			Marks.AddQuad(I0, I1, I2, I3);
		}
	}
}

int32 ANeonArena::GetActiveTierCount() const
{
	// One flat floor. The stepped pyramid and the stacked decks are both off.
	(void)ActiveLayout;
	return 0;
}

void ANeonArena::SetLayout(int32 Index)
{
	// One map. The round index is ignored; The Hex, The Cross, and The Corners stay unbuilt.
	(void)Index;
	ActiveLayout = 1;
	BuildTerrainMesh();
	LayoutCosmetics();
}

FText ANeonArena::GetLayoutName() const
{
	switch (ActiveLayout)
	{
	case 0: return NSLOCTEXT("NeonArena", "Hex", "THE HEX");
	case 2: return NSLOCTEXT("NeonArena", "Cross", "THE CROSS");
	case 3: return NSLOCTEXT("NeonArena", "Corners", "THE CORNERS");
	default: return NSLOCTEXT("NeonArena", "Grid", "THE GRID");
	}
}

float ANeonArena::LocalFloorHeight(float LocalX, float LocalY) const
{
	if (IsHexLayout())
	{
		TArray<FNeonDeck> Decks;
		CollectDecks(*this, Decks);
		for (const FNeonDeck& D : Decks)
		{
			if (D.Layer == 0 && DeckContains(FVector2D(LocalX, LocalY), D))
			{
				return D.Height;
			}
		}
		return 0.f;
	}
	return PyramidHeightAt(*this, LocalX, LocalY) + HillHeightAt(*this, LocalX, LocalY);
}

float ANeonArena::GetFloorHeight(FVector2D WorldXY) const
{
	return GetFloorHeightOnLayer(WorldXY, 0);
}

float ANeonArena::GetFloorHeightOnLayer(FVector2D WorldXY, int32 Layer) const
{
	float Z = 0.f;
	if (TryGetFloorHeightOnLayer(WorldXY, Layer, Z))
	{
		return Z;
	}
	return GetActorLocation().Z;
}

bool ANeonArena::HasFloorOnLayer(FVector2D WorldXY, int32 Layer) const
{
	float Unused = 0.f;
	return TryGetFloorHeightOnLayer(WorldXY, Layer, Unused);
}

bool ANeonArena::TryGetFloorHeightOnLayer(FVector2D WorldXY, int32 Layer, float& OutWorldZ) const
{
	const FVector Center = GetActorLocation();
	const FVector2D Local(WorldXY.X - Center.X, WorldXY.Y - Center.Y);

	TArray<FNeonAuthoredRamp> Ramps;
	CollectRamps(*this, Ramps);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);

	float Best = -1.e12f;
	bool bFound = false;

	for (const FNeonAuthoredRamp& R : Ramps)
	{
		float Along = 0.f;
		float Across = 0.f;
		if (!RampCoords(Local, R, Along, Across))
		{
			continue;
		}
		if (R.Kind == ENeonRampKind::Hill)
		{
			if (Layer != 0)
			{
				continue;
			}
			Best = FMath::Max(Best, LocalFloorHeight(Local.X, Local.Y));
			bFound = true;
			continue;
		}
		if (R.FromLayer != Layer && R.ToLayer != Layer)
		{
			continue;
		}
		const float T = Along / FMath::Max(R.Length, 1.f);
		const float ConnectorZ = ConnectorHeightAt(*this, R, Along, Decks);
		// Stay on the slope until the ramp actually meets the deck, so the bike does not pop through it.
		if (T < 0.94f)
		{
			OutWorldZ = Center.Z + ConnectorZ;
			return true;
		}
		Best = FMath::Max(Best, ConnectorZ);
		bFound = true;
	}

	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer != Layer || !DeckContains(Local, D))
		{
			continue;
		}
		Best = FMath::Max(Best, D.Height);
		bFound = true;
	}

	if (Layer == 0 && !IsHexLayout())
	{
		Best = FMath::Max(Best, LocalFloorHeight(Local.X, Local.Y));
		bFound = true;
	}

	if (!bFound)
	{
		return false;
	}
	OutWorldZ = Center.Z + Best;
	return true;
}

FVector ANeonArena::GetFloorNormal(FVector2D WorldXY) const
{
	return GetFloorNormalOnLayer(WorldXY, 0);
}

FVector ANeonArena::GetFloorNormalOnLayer(FVector2D WorldXY, int32 Layer) const
{
	constexpr float Eps = 25.f;
	float HX0 = 0.f, HX1 = 0.f, HY0 = 0.f, HY1 = 0.f;
	const bool bX0 = TryGetFloorHeightOnLayer(WorldXY + FVector2D(Eps, 0.f), Layer, HX1);
	const bool bX1 = TryGetFloorHeightOnLayer(WorldXY - FVector2D(Eps, 0.f), Layer, HX0);
	const bool bY0 = TryGetFloorHeightOnLayer(WorldXY + FVector2D(0.f, Eps), Layer, HY1);
	const bool bY1 = TryGetFloorHeightOnLayer(WorldXY - FVector2D(0.f, Eps), Layer, HY0);
	float CenterZ = 0.f;
	TryGetFloorHeightOnLayer(WorldXY, Layer, CenterZ);
	const float DhDx = ((bX0 ? HX1 : CenterZ) - (bX1 ? HX0 : CenterZ)) / (2.f * Eps);
	const float DhDy = ((bY0 ? HY1 : CenterZ) - (bY1 ? HY0 : CenterZ)) / (2.f * Eps);
	return FVector(-DhDx, -DhDy, 1.f).GetSafeNormal();
}

bool ANeonArena::IsWrongWayApproach(FVector2D FromWorld, FVector2D ToWorld, float HeadingDegrees, int32 Layer, float WorldZ) const
{
	const FVector Center = GetActorLocation();
	const FVector2D From(FromWorld.X - Center.X, FromWorld.Y - Center.Y);
	const FVector2D To(ToWorld.X - Center.X, ToWorld.Y - Center.Y);
	const float Rad = FMath::DegreesToRadians(HeadingDegrees);
	const FVector2D Head(FMath::Cos(Rad), FMath::Sin(Rad));

	TArray<FNeonAuthoredRamp> Ramps;
	CollectRamps(*this, Ramps);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);

	for (const FNeonAuthoredRamp& R : Ramps)
	{
		float Along0 = 0.f, Across0 = 0.f, Along1 = 0.f, Across1 = 0.f;
		const bool bOn0 = RampCoords(From, R, Along0, Across0);
		const bool bOn1 = RampCoords(To, R, Along1, Across1);
		if (!bOn1 || bOn0)
		{
			continue;
		}

		const float Align = FVector2D::DotProduct(Head, R.Dir);
		// Only a head-on wrong way is a wall. Crossing the side of a ramp is a ride.
		const bool bAgainstClimb = (R.Kind == ENeonRampKind::Hill || Layer == R.FromLayer) && Align < -0.55f;
		const bool bAgainstDescent = R.Kind == ENeonRampKind::Connector && Layer == R.ToLayer && Align > 0.55f;
		if (bAgainstClimb || bAgainstDescent)
		{
			return true;
		}
	}

	const float LocalZ = WorldZ - Center.Z;
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= Layer || !DeckContains(To, D) || DeckContains(From, D))
		{
			continue;
		}
		if (LocalZ < D.Height - 80.f || LocalZ > D.Height + 80.f)
		{
			continue;
		}
		bool bViaRamp = false;
		for (const FNeonAuthoredRamp& R : Ramps)
		{
			if (R.Kind != ENeonRampKind::Connector || R.ToLayer != D.Layer)
			{
				continue;
			}
			float Along = 0.f, Across = 0.f;
			const bool bTo = RampCoords(To, R, Along, Across);
			const bool bFrom = RampCoords(From, R, Along, Across);
			if (bTo || bFrom)
			{
				bViaRamp = true;
				break;
			}
		}
		if (!bViaRamp)
		{
			return true;
		}
	}
	return false;
}

int32 ANeonArena::AdvanceLayer(FVector2D WorldXY, int32 Layer) const
{
	const FVector Center = GetActorLocation();
	const FVector2D Local(WorldXY.X - Center.X, WorldXY.Y - Center.Y);
	TArray<FNeonAuthoredRamp> Ramps;
	CollectRamps(*this, Ramps);
	for (const FNeonAuthoredRamp& R : Ramps)
	{
		if (R.Kind != ENeonRampKind::Connector)
		{
			continue;
		}
		if (R.FromLayer != Layer && R.ToLayer != Layer)
		{
			continue;
		}
		float Along = 0.f, Across = 0.f;
		if (!RampCoords(Local, R, Along, Across))
		{
			continue;
		}
		const float T = Along / FMath::Max(R.Length, 1.f);
		if (T >= 0.94f)
		{
			return R.ToLayer;
		}
		if (T <= 0.06f)
		{
			return R.FromLayer;
		}
	}
	return Layer;
}

int32 ANeonArena::ResolveLayerAt(FVector2D WorldXY, float WorldZ, int32 HintLayer) const
{
	int32 Best = -1;
	float BestD = 1.e12f;
	const int32 MaxL = GetHighestFloorLayer();
	for (int32 L = 0; L <= MaxL; ++L)
	{
		float H = 0.f;
		if (!TryGetFloorHeightOnLayer(WorldXY, L, H))
		{
			continue;
		}
		const float D = WorldZ - H;
		if (D < -80.f)
		{
			continue;
		}
		if (D < BestD)
		{
			BestD = D;
			Best = L;
		}
	}
	return Best >= 0 ? Best : HintLayer;
}

int32 ANeonArena::GetHighestFloorLayer() const
{
	return IsHexLayout() ? 3 : 1;
}

bool ANeonArena::HitsCeiling(FVector2D WorldXY, float BelowZ, float AboveZ, float& OutCeilingZ) const
{
	const FVector Center = GetActorLocation();
	const FVector2D Local(WorldXY.X - Center.X, WorldXY.Y - Center.Y);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);
	float Lowest = 1.e12f;
	bool bHit = false;
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= 0 || !DeckContains(Local, D))
		{
			continue;
		}
		const float H = Center.Z + D.Height;
		if (BelowZ < H && AboveZ >= H && H < Lowest)
		{
			Lowest = H;
			bHit = true;
		}
	}
	if (bHit)
	{
		OutCeilingZ = Lowest;
	}
	return bHit;
}

void ANeonArena::BuildTerrainMesh()
{
	if (!Terrain)
	{
		return;
	}

	Terrain->ClearAllMeshSections();

	const float Res = FMath::Max(TerrainResolution, 25.f);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);
	TArray<FNeonAuthoredRamp> Ramps;
	CollectRamps(*this, Ramps);

	auto Commit = [this](int32 Section, FSectionBuilder& B, const FLinearColor& Color)
	{
		if (B.Vertices.Num() == 0)
		{
			return;
		}
		Terrain->CreateMeshSection_LinearColor(Section, B.Vertices, B.Triangles, B.Normals, B.UVs, B.Colors, B.Tangents, false);
		if (PlaceholderMaterial)
		{
			Terrain->SetMaterial(Section, PlaceholderMaterial);
			if (UMaterialInstanceDynamic* MID = Terrain->CreateAndSetMaterialInstanceDynamic(Section))
			{
				MID->SetVectorParameterValue(TEXT("Color"), Color);
			}
		}
	};

	// ---- Section 0: ground ----
	FSectionBuilder Floor;
	FSectionBuilder Skirts;
	FSectionBuilder Ceilings;
	FSectionBuilder HexRims;
	if (IsHexLayout())
	{
		for (const FNeonDeck& D : Decks)
		{
			const float Below = D.Layer == 0 ? -80.f : (D.Layer - 1) * FMath::Max(DeckRise, 200.f);
			if (D.Layer == 0)
			{
				AddHexDeck(Floor, Skirts, Ceilings, D, Res, Below, Ramps);
			}
			AddHexRim(HexRims, D, Ramps);
		}
	}
	else
	{
		const int32 CountX = FMath::Max(2, FMath::CeilToInt(HalfExtents.X * 2.f / Res) + 1);
		const int32 CountY = FMath::Max(2, FMath::CeilToInt(HalfExtents.Y * 2.f / Res) + 1);
		const float StepX = HalfExtents.X * 2.f / (CountX - 1);
		const float StepY = HalfExtents.Y * 2.f / (CountY - 1);
		Floor.Vertices.Reserve(CountX * CountY);
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
				Floor.AddVertex(FVector(X, Y, Z), FVector(-DhDx, -DhDy, 1.f).GetSafeNormal(), FVector2D(X / 400.f, Y / 400.f));
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
	}
	Commit(0, Floor, IsHexLayout() ? FLinearColor(0.05f, 0.16f, 0.28f) : FloorColor);

	// ---- Section 1: pyramid lips ----
	FSectionBuilder Lips;
	constexpr float LipWidth = 16.f;
	constexpr float LipLift = 2.f;
	const float MinHalf = FMath::Min(HalfExtents.X, HalfExtents.Y);
	for (int32 Tier = 1; Tier <= GetActiveTierCount(); ++Tier)
	{
		const float Inset = Tier * TierSpacing;
		if (Inset + LipWidth >= MinHalf)
		{
			break;
		}
		const float HX = HalfExtents.X - Inset;
		const float HY = HalfExtents.Y - Inset;
		const float Z = LocalFloorHeight(0.f, HY) + LipLift;
		Lips.AddFlatRect(-HX, HY - LipWidth, HX, HY, Z);
		Lips.AddFlatRect(-HX, -HY, HX, -HY + LipWidth, Z);
		Lips.AddFlatRect(HX - LipWidth, -HY, HX, HY, Z);
		Lips.AddFlatRect(-HX, -HY, -HX + LipWidth, HY, Z);
	}
	Commit(1, Lips, WallColor);

	// ---- Sections 2-3: stacked decks, solid edges, and the ceiling you see from below ----
	FSectionBuilder DeckTops;
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= 0)
		{
			continue;
		}
		const float Below = (D.Layer - 1) * FMath::Max(DeckRise, 200.f);
		if (D.Shape == ENeonDeckShape::Hex)
		{
			AddHexDeck(DeckTops, Skirts, Ceilings, D, Res, Below, Ramps);
		}
		else
		{
			AddRectDeck(DeckTops, Skirts, Ceilings, HexRims, D, Below, Ramps);
		}
	}
	if (IsHexLayout())
	{
		FSectionBuilder Void;
		Void.AddFlatRect(-HalfExtents.X, -HalfExtents.Y, HalfExtents.X, HalfExtents.Y, -60.f);
		Commit(7, Void, FLinearColor(0.12f, 0.01f, 0.03f));
	}
	const FLinearColor DeckColor = FLinearColor(0.10f, 0.04f, 0.16f);
	Commit(2, DeckTops, DeckColor);
	Commit(3, Skirts, FLinearColor(0.05f, 0.55f, 0.85f));
	Commit(6, HexRims, LaunchRampColor);
	Commit(8, Ceilings, FLinearColor(0.02f, 0.35f, 0.55f));

	// ---- Sections 4, 5, 9: connector ramps. Dark deck, cyan stripes and rails, solid sides ----
	FSectionBuilder Connectors;
	FSectionBuilder Marks;
	FSectionBuilder RampSides;
	for (const FNeonAuthoredRamp& R : Ramps)
	{
		const float Z0 = (R.Kind == ENeonRampKind::Hill)
			? PyramidHeightAt(*this, R.Origin.X, R.Origin.Y)
			: DeckHeightForLayer(Decks, R.FromLayer) + (R.FromLayer == 0 ? PyramidHeightAt(*this, R.Origin.X, R.Origin.Y) : 0.f);
		const float Z1 = (R.Kind == ENeonRampKind::Hill)
			? Z0 + LaunchRampHeight
			: DeckHeightForLayer(Decks, R.ToLayer);
		if (R.Kind == ENeonRampKind::Connector)
		{
			// Sides drop to the floor the ramp starts from, so a bridge reads as a solid block instead of a floating sheet.
			float SideBottom = Z0 - 6.f;
			if (R.FromLayer == 0)
			{
				const FVector2D TopXY = R.Origin + R.Dir * R.Length;
				SideBottom = FMath::Min(PyramidHeightAt(*this, R.Origin.X, R.Origin.Y), PyramidHeightAt(*this, TopXY.X, TopXY.Y)) - 10.f;
			}
			AddRampMesh(Connectors, RampSides, Marks, R, Z0, Z1, SideBottom);
		}
		AddChevrons(Marks, R, Z0, Z1);
	}
	Commit(4, Connectors, FLinearColor(0.07f, 0.03f, 0.12f));
	Commit(5, Marks, LaunchRampColor);
	Commit(9, RampSides, FLinearColor(0.05f, 0.55f, 0.85f));

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
	if (DevFillLight)
	{
		DevFillLight->SetVisibility(bEnableDevLight);
	}
}

void ANeonArena::LayoutWalls(const FVector2D& Half)
{
	LastWallHalf = Half;

	const bool bHex = IsHexLayout();
	for (UStaticMeshComponent* Wall : { WallNorth.Get(), WallSouth.Get(), WallEast.Get(), WallWest.Get() })
	{
		if (Wall)
		{
			Wall->SetVisibility(!bHex);
		}
	}

	// Engine cube is 100 cm. The walls stand on Z = 0 and are tall enough to clear every tier, so a wall
	// that has moved onto the pyramid never floats above the raised floor.
	float Highest = FMath::Max(GetActiveTierCount(), 0) * FMath::Max(TierHeight, 0.f);
	Highest = FMath::Max(Highest, LaunchRampHeight);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);
	for (const FNeonDeck& D : Decks)
	{
		Highest = FMath::Max(Highest, D.Height);
	}
	const float Height = WallHeight + Highest + 600.f;

	if (bHex && Terrain)
	{
		// Six walls on the hex itself. They close in with the collapse, and the rectangle stays hidden.
		FSectionBuilder HexWall;
		const float Radius = GetHexWallRadius(Half);
		for (int32 i = 0; i < 6; ++i)
		{
			const float A0 = FMath::DegreesToRadians(static_cast<float>(i) * 60.f);
			const float A1 = FMath::DegreesToRadians(static_cast<float>(i + 1) * 60.f);
			const FVector P0(Radius * FMath::Cos(A0), Radius * FMath::Sin(A0), 0.f);
			const FVector P1(Radius * FMath::Cos(A1), Radius * FMath::Sin(A1), 0.f);
			HexWall.AddVerticalQuad(P0, P1, Height);
		}
		Terrain->CreateMeshSection_LinearColor(10, HexWall.Vertices, HexWall.Triangles, HexWall.Normals, HexWall.UVs, HexWall.Colors, HexWall.Tangents, false);
		if (PlaceholderMaterial)
		{
			Terrain->SetMaterial(10, PlaceholderMaterial);
			HexWallMID = Terrain->CreateAndSetMaterialInstanceDynamic(10);
			if (HexWallMID)
			{
				HexWallMID->SetVectorParameterValue(TEXT("Color"), WallColor);
			}
		}
		return;
	}
	if (Terrain)
	{
		Terrain->ClearMeshSection(10);
	}
	HexWallMID = nullptr;

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
	if (HexWallMID)
	{
		HexWallMID->SetVectorParameterValue(TEXT("Color"), Color);
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
