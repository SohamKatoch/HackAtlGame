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
}

void ANeonArena::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildTerrainMesh();
	LayoutCosmetics();
	LayoutCamera();
}

void ANeonArena::BeginPlay()
{
	Super::BeginPlay();
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
		// Four compass points on the ground hex, facing inward.
		const float R = FMath::Min(HalfExtents.X, HalfExtents.Y) * 0.62f;
		const float Ang = PI * 0.25f + Index * PI * 0.5f;
		LocalX = R * FMath::Cos(Ang);
		LocalY = R * FMath::Sin(Ang);
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
	FVector2D Pick(0.f, 0.f);
	bool bFound = false;
	for (int32 Attempt = 0; Attempt < 40 && !bFound; ++Attempt)
	{
		const FVector2D Candidate(Rng.FRandRange(-HalfX, HalfX), Rng.FRandRange(-HalfY, HalfY));
		const FVector2D World(Center.X + Candidate.X, Center.Y + Candidate.Y);
		if (!HasFloorOnLayer(World, 0))
		{
			continue;
		}
		bool bClear = true;
		for (const FVector2D& Other : Avoid)
		{
			if ((Other - Candidate).SizeSquared() < MinSepSq)
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
		Out.Reset();
		const FVector2D H = A.HalfExtents;
		const float Rise = FMath::Max(A.DeckRise, 200.f);

		if (A.IsHexLayout())
		{
			const float R0 = FMath::Min(H.X, H.Y) * 0.88f;
			const float Radii[4] = { R0, R0 * 0.70f, R0 * 0.50f, R0 * 0.34f };
			for (int32 Layer = 0; Layer < 4; ++Layer)
			{
				FNeonDeck D;
				D.Layer = Layer;
				D.Height = Layer * Rise;
				D.Shape = ENeonDeckShape::Hex;
				D.HexRadius = Radii[Layer];
				Out.Add(D);
			}
			return;
		}

		if (A.ActiveLayout == 2)
		{
			FNeonDeck Bar;
			Bar.Layer = 1;
			Bar.Height = Rise;
			Bar.Half = FVector2D(4200.f, 1400.f);
			Out.Add(Bar);
			Bar.Half = FVector2D(1400.f, 3000.f);
			Out.Add(Bar);
			return;
		}

		if (A.ActiveLayout == 3)
		{
			const FVector2D Pad(1600.f, 1200.f);
			const FVector2D Corners[4] = {
				FVector2D(-0.55f * H.X,  0.50f * H.Y),
				FVector2D( 0.55f * H.X,  0.50f * H.Y),
				FVector2D(-0.55f * H.X, -0.50f * H.Y),
				FVector2D( 0.55f * H.X, -0.50f * H.Y)
			};
			for (const FVector2D& C : Corners)
			{
				FNeonDeck D;
				D.Layer = 1;
				D.Height = Rise;
				D.Center = C;
				D.Half = Pad;
				Out.Add(D);
			}
			return;
		}

		FNeonDeck Center;
		Center.Layer = 1;
		Center.Height = Rise;
		Center.Half = FVector2D(3600.f, 2400.f);
		Out.Add(Center);
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

	void AddConnector(TArray<FNeonAuthoredRamp>& Out, const FVector2D& Origin, const FVector2D& Dir, float Length, float HalfW, int32 From, int32 To)
	{
		FNeonAuthoredRamp R;
		R.Origin = Origin;
		R.Dir = Dir;
		R.Length = Length;
		R.HalfWidth = HalfW;
		R.Kind = ENeonRampKind::Connector;
		R.FromLayer = From;
		R.ToLayer = To;
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

		if (A.IsHexLayout())
		{
			const float R0 = FMath::Min(H.X, H.Y) * 0.88f;
			const float Radii[4] = { R0, R0 * 0.70f, R0 * 0.50f, R0 * 0.34f };
			const float Angles[3] = { 0.f, 2.f * PI / 3.f, 4.f * PI / 3.f };
			for (int32 i = 0; i < 3; ++i)
			{
				const FVector2D OutDir(FMath::Cos(Angles[i]), FMath::Sin(Angles[i]));
				const float Length = FMath::Max(Radii[i] - Radii[i + 1], 800.f) * 0.9f;
				const FVector2D Origin = OutDir * (Radii[i + 1] + Length);
				AddConnector(Out, Origin, -OutDir, Length, 720.f, i, i + 1);
			}
			return;
		}

		if (A.ActiveLayout == 2)
		{
			AddConnector(Out, FVector2D(-6400.f, 0.f), FVector2D( 1.f, 0.f), 2200.f, 700.f, 0, 1);
			AddConnector(Out, FVector2D( 0.f, 5200.f), FVector2D( 0.f,-1.f), 2200.f, 700.f, 0, 1);
			return;
		}

		if (A.ActiveLayout == 3)
		{
			const FVector2D Pads[4] = {
				FVector2D(-0.55f * H.X,  0.50f * H.Y),
				FVector2D( 0.55f * H.X,  0.50f * H.Y),
				FVector2D(-0.55f * H.X, -0.50f * H.Y),
				FVector2D( 0.55f * H.X, -0.50f * H.Y)
			};
			for (const FVector2D& Pad : Pads)
			{
				const FVector2D Dir = Pad.GetSafeNormal();
				const FVector2D Origin = Pad - Dir * 2200.f;
				AddConnector(Out, Origin, Dir, 2200.f, 620.f, 0, 1);
			}
			return;
		}

		AddConnector(Out, FVector2D(-5800.f, 0.f), FVector2D( 1.f, 0.f), 2200.f, 700.f, 0, 1);
		AddConnector(Out, FVector2D( 5800.f, 0.f), FVector2D(-1.f, 0.f), 2200.f, 700.f, 0, 1);
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

	void AddRampMesh(FSectionBuilder& B, const FNeonAuthoredRamp& R, float Z0, float Z1)
	{
		const FVector2D Perp(-R.Dir.Y, R.Dir.X);
		constexpr int32 Steps = 10;
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
			const int32 I0 = B.AddVertex(A, N, FVector2D(T0, 0.f));
			const int32 I1 = B.AddVertex(BB, N, FVector2D(T1, 0.f));
			const int32 I2 = B.AddVertex(C, N, FVector2D(T0, 1.f));
			const int32 I3 = B.AddVertex(D, N, FVector2D(T1, 1.f));
			B.AddQuad(I0, I1, I2, I3);
		}
	}

	void AddHexDeck(FSectionBuilder& Top, FSectionBuilder& Skirt, const FNeonDeck& D, float Res)
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

		const float ZBot = D.Height - 80.f;
		for (int32 i = 0; i < 6; ++i)
		{
			const float A0 = FMath::DegreesToRadians(i * 60.f);
			const float A1 = FMath::DegreesToRadians((i + 1) * 60.f);
			const FVector P0(D.Center.X + D.HexRadius * FMath::Cos(A0), D.Center.Y + D.HexRadius * FMath::Sin(A0), ZBot);
			const FVector P1(D.Center.X + D.HexRadius * FMath::Cos(A1), D.Center.Y + D.HexRadius * FMath::Sin(A1), ZBot);
			Skirt.AddVerticalQuad(P0, P1, D.Height);
		}
	}

	void AddHexRim(FSectionBuilder& Rim, const FNeonDeck& D)
	{
		const float Outer = D.HexRadius + 24.f;
		const float Inner = FMath::Max(D.HexRadius - 110.f, 80.f);
		const float Z = D.Height + 18.f;
		const float WallTop = D.Height + 140.f;
		const FVector Up = FVector::UpVector;
		for (int32 i = 0; i < 6; ++i)
		{
			const float A0 = FMath::DegreesToRadians(static_cast<float>(i) * 60.f);
			const float A1 = FMath::DegreesToRadians(static_cast<float>(i + 1) * 60.f);
			const FVector2D O0(D.Center.X + Outer * FMath::Cos(A0), D.Center.Y + Outer * FMath::Sin(A0));
			const FVector2D O1(D.Center.X + Outer * FMath::Cos(A1), D.Center.Y + Outer * FMath::Sin(A1));
			const FVector2D I0(D.Center.X + Inner * FMath::Cos(A0), D.Center.Y + Inner * FMath::Sin(A0));
			const FVector2D I1(D.Center.X + Inner * FMath::Cos(A1), D.Center.Y + Inner * FMath::Sin(A1));
			const int32 V0 = Rim.AddVertex(FVector(I0.X, I0.Y, Z), Up, FVector2D(0.f, 0.f));
			const int32 V1 = Rim.AddVertex(FVector(O0.X, O0.Y, Z), Up, FVector2D(1.f, 0.f));
			const int32 V2 = Rim.AddVertex(FVector(I1.X, I1.Y, Z), Up, FVector2D(0.f, 1.f));
			const int32 V3 = Rim.AddVertex(FVector(O1.X, O1.Y, Z), Up, FVector2D(1.f, 1.f));
			Rim.AddQuad(V0, V1, V2, V3);
			Rim.AddVerticalQuad(FVector(O0.X, O0.Y, D.Height), FVector(O1.X, O1.Y, D.Height), WallTop);
		}
	}

	void AddRectDeck(FSectionBuilder& Top, FSectionBuilder& Skirt, const FNeonDeck& D)
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
		const float ZBot = D.Height - 80.f;
		Skirt.AddVerticalQuad(FVector(X0, Y0, ZBot), FVector(X1, Y0, ZBot), D.Height);
		Skirt.AddVerticalQuad(FVector(X1, Y0, ZBot), FVector(X1, Y1, ZBot), D.Height);
		Skirt.AddVerticalQuad(FVector(X1, Y1, ZBot), FVector(X0, Y1, ZBot), D.Height);
		Skirt.AddVerticalQuad(FVector(X0, Y1, ZBot), FVector(X0, Y0, ZBot), D.Height);
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
	switch (ActiveLayout)
	{
	case 0: return 0;
	case 2: return 1;
	case 3: return FMath::Clamp(TierCount + 1, 1, 5);
	default: return FMath::Max(TierCount, 0);
	}
}

void ANeonArena::SetLayout(int32 Index)
{
	int32 Next = Index % LayoutCount;
	if (Next < 0)
	{
		Next += LayoutCount;
	}
	ActiveLayout = Next;
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
		Best = FMath::Max(Best, ConnectorHeightAt(*this, R, Along, Decks));
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
		if (R.Kind == ENeonRampKind::Hill)
		{
			if (Align < 0.25f)
			{
				return true;
			}
		}
		else
		{
			const bool bClimb = (Layer == R.FromLayer) && Align > 0.25f && Along1 < R.Length * 0.55f;
			const bool bDescend = (Layer == R.ToLayer) && Align < -0.25f && Along1 > R.Length * 0.45f;
			if (!bClimb && !bDescend)
			{
				return true;
			}
		}
	}

	const float LocalZ = WorldZ - Center.Z;
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= Layer || !DeckContains(To, D) || DeckContains(From, D))
		{
			continue;
		}
		if (LocalZ + 40.f < D.Height - 120.f || LocalZ > D.Height + 200.f)
		{
			continue;
		}
		bool bViaRamp = false;
		for (const FNeonAuthoredRamp& R : Ramps)
		{
			float Along = 0.f, Across = 0.f;
			if (R.Kind == ENeonRampKind::Connector && R.ToLayer == D.Layer && RampCoords(To, R, Along, Across))
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
		if (T >= 0.72f)
		{
			return R.ToLayer;
		}
		if (T <= 0.28f)
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

bool ANeonArena::HitsCeiling(FVector2D WorldXY, float BelowZ, float AboveZ) const
{
	const FVector Center = GetActorLocation();
	const FVector2D Local(WorldXY.X - Center.X, WorldXY.Y - Center.Y);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= 0 || !DeckContains(Local, D))
		{
			continue;
		}
		const float H = Center.Z + D.Height;
		if (BelowZ < H && AboveZ >= H)
		{
			return true;
		}
	}
	return false;
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
	FSectionBuilder HexRims;
	if (IsHexLayout())
	{
		for (const FNeonDeck& D : Decks)
		{
			if (D.Layer == 0)
			{
				AddHexDeck(Floor, Skirts, D, Res);
			}
			AddHexRim(HexRims, D);
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

	// ---- Sections 2-3: stacked decks and their skirts ----
	FSectionBuilder DeckTops;
	for (const FNeonDeck& D : Decks)
	{
		if (D.Layer <= 0)
		{
			continue;
		}
		if (D.Shape == ENeonDeckShape::Hex)
		{
			AddHexDeck(DeckTops, Skirts, D, Res);
		}
		else
		{
			AddRectDeck(DeckTops, Skirts, D);
		}
	}
	const FLinearColor DeckColor = FLinearColor(0.04f, 0.08f, 0.16f);
	Commit(2, DeckTops, DeckColor);
	Commit(3, Skirts, FLinearColor(0.03f, 0.35f, 0.55f));
	Commit(6, HexRims, LaunchRampColor);

	// ---- Section 4: connector ramps ----
	FSectionBuilder Connectors;
	FSectionBuilder Marks;
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
			AddRampMesh(Connectors, R, Z0, Z1);
		}
		AddChevrons(Marks, R, Z0, Z1);
	}
	Commit(4, Connectors, LaunchRampColor);
	Commit(5, Marks, LaunchRampColor);

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
	float Highest = FMath::Max(GetActiveTierCount(), 0) * FMath::Max(TierHeight, 0.f);
	Highest = FMath::Max(Highest, LaunchRampHeight);
	TArray<FNeonDeck> Decks;
	CollectDecks(*this, Decks);
	for (const FNeonDeck& D : Decks)
	{
		Highest = FMath::Max(Highest, D.Height);
	}
	const float Height = WallHeight + Highest + 600.f;
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
