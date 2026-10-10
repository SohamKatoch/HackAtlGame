#include "Core/NeonGameMode.h"

#include "AI/NeonAIController.h"
#include "Arena/NeonArena.h"
#include "Core/NeonGameInstance.h"
#include "Core/NeonGameState.h"
#include "Core/NeonPlayerController.h"
#include "Core/NeonPlayerState.h"
#include "EngineUtils.h"
#include "NeonGrid.h"
#include "Powerups/NeonPowerupComponent.h"
#include "Powerups/NeonPowerupConfig.h"
#include "Powerups/NeonPowerupSpawner.h"
#include "TimerManager.h"
#include "Trail/NeonCollisionSystem.h"
#include "Vehicle/NeonVehicleInput.h"
#include "Vehicle/NeonVehiclePawn.h"

ANeonGameMode::ANeonGameMode()
{
	DefaultPawnClass = ANeonVehiclePawn::StaticClass();
	PlayerControllerClass = ANeonPlayerController::StaticClass();
	PlayerStateClass = ANeonPlayerState::StaticClass();
	GameStateClass = ANeonGameState::StaticClass();
	ArenaClass = ANeonArena::StaticClass();
	AIControllerClass = ANeonAIController::StaticClass();
	PowerupSpawner = CreateDefaultSubobject<UNeonPowerupSpawner>(TEXT("PowerupSpawner"));

	SlotColors = {
		FLinearColor(0.10f, 0.90f, 1.00f), // cyan
		FLinearColor(1.00f, 0.25f, 0.55f), // magenta
		FLinearColor(0.40f, 1.00f, 0.30f), // green
		FLinearColor(1.00f, 0.75f, 0.15f)  // amber
	};
}

ANeonGameState* ANeonGameMode::GetNeonGameState() const
{
	return GetGameState<ANeonGameState>();
}

// ---------------------------------------------------------------------------
// Startup / login
// ---------------------------------------------------------------------------

void ANeonGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	EnsureArena();

#if !UE_BUILD_SHIPPING
	bRoundEndsAtZero = FParse::Param(FCommandLine::Get(), TEXT("NeonRoundEndsAtZero"));
#endif
}

void ANeonGameMode::StartPlay()
{
	Super::StartPlay();

	bool bAutoStart = !bStartInMainMenu;
	FNeonMatchSettings Settings;
	if (const UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		Settings = GI->MatchSettings;
	}

#if !UE_BUILD_SHIPPING
	// Headless test hooks: -NeonAutoStart -NeonAICount=N -NeonAIDifficulty=0..3 -NeonScoreLimit=N
	if (FParse::Param(FCommandLine::Get(), TEXT("NeonAutoStart")))
	{
		bAutoStart = true;
	}
	FParse::Value(FCommandLine::Get(), TEXT("NeonAICount="), Settings.AIPlayerCount);
	int32 LevelInt = static_cast<int32>(Settings.AIDifficulty);
	if (FParse::Value(FCommandLine::Get(), TEXT("NeonAIDifficulty="), LevelInt))
	{
		Settings.AIDifficulty = static_cast<ENeonAIDifficulty>(FMath::Clamp(LevelInt, 0, 3));
	}
	FParse::Value(FCommandLine::Get(), TEXT("NeonScoreLimit="), Settings.ScoreLimit);
	if (FParse::Param(FCommandLine::Get(), TEXT("NeonNoPowerups")))
	{
		Settings.bPowerupsEnabled = false;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("NeonNoCollapse")))
	{
		Settings.bArenaCollapse = false;
	}
#endif

	if (bAutoStart)
	{
		StartSoloMatch(Settings);
	}
	else if (ANeonGameState* GS = GetNeonGameState())
	{
		GS->SetPhase(ENeonMatchPhase::MainMenu, 0.f);
	}
}

void ANeonGameMode::PostLogin(APlayerController* NewPlayer)
{
	// Assign slot and color before Super spawns anything (PlayerCanRestart gates the spawn by phase).
	if (ANeonPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<ANeonPlayerState>() : nullptr)
	{
		const int32 Slot = FMath::Clamp(CountHumans() - 1, 0, 3);
		PS->SetSlot(Slot);
		PS->SetIsAI(false);
		PS->SetPlayerName(SlotLabel(Slot));
		if (SlotColors.IsValidIndex(Slot))
		{
			PS->SetColor(SlotColors[Slot]);
		}
	}

	Super::PostLogin(NewPlayer);
}

void ANeonGameMode::Logout(AController* Exiting)
{
	if (APawn* Pawn = Exiting ? Exiting->GetPawn() : nullptr)
	{
		Vehicles.Remove(Cast<ANeonVehiclePawn>(Pawn));
	}
	Super::Logout(Exiting);
}

bool ANeonGameMode::PlayerCanRestart_Implementation(APlayerController* Player)
{
	const ANeonGameState* GS = GetNeonGameState();
	if (GS && GS->GetPhase() == ENeonMatchPhase::MainMenu)
	{
		return false; // no vehicles in the menu; StartSoloMatch spawns them
	}
	return Super::PlayerCanRestart_Implementation(Player);
}

void ANeonGameMode::EnsureArena()
{
	if (Arena)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ANeonArena> It(World); It; ++It)
	{
		Arena = *It;
		break;
	}

	if (!Arena && ArenaClass)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Arena = World->SpawnActor<ANeonArena>(ArenaClass, FTransform::Identity, Params);
		UE_LOG(LogNeonGrid, Log, TEXT("No arena in level; spawned %s at origin."), *GetNameSafe(Arena));
	}
}

// ---------------------------------------------------------------------------
// Players and vehicles
// ---------------------------------------------------------------------------

int32 ANeonGameMode::CountHumans() const
{
	int32 Count = 0;
	if (GameState)
	{
		for (const APlayerState* PS : GameState->PlayerArray)
		{
			if (PS && !PS->IsABot())
			{
				++Count;
			}
		}
	}
	return Count;
}

int32 ANeonGameMode::GetPlayerSlot(const AController* Controller) const
{
	if (const ANeonPlayerState* PS = Controller ? Controller->GetPlayerState<ANeonPlayerState>() : nullptr)
	{
		return PS->GetSlot();
	}
	return 0;
}

FString ANeonGameMode::SlotLabel(int32 Slot) const
{
	return FString::Printf(TEXT("P%d"), Slot + 1);
}

ANeonPlayerState* ANeonGameMode::FindPlayerStateForVehicle(const ANeonVehiclePawn* Vehicle) const
{
	if (!Vehicle)
	{
		return nullptr;
	}
	if (ANeonPlayerState* PS = Vehicle->GetPlayerState<ANeonPlayerState>())
	{
		return PS;
	}
	// Unpossessed (e.g. mid-destroy): fall back to the slot.
	const ANeonGameState* GS = GetNeonGameState();
	return GS ? GS->FindPlayerStateBySlot(Vehicle->GetPlayerSlot()) : nullptr;
}

FTransform ANeonGameMode::MakeSpawnTransform(int32 Slot, bool bAI, FRandomStream& Rng, TArray<FVector2D>& Occupied) const
{
	const FTransform Spawn = (bAI && Arena) ? Arena->GetRandomSpawnTransform(Rng, Occupied) : (Arena ? Arena->GetSpawnTransform(Slot) : FTransform::Identity);
	Occupied.Add(FVector2D(Spawn.GetLocation().X, Spawn.GetLocation().Y));
	return Spawn;
}

void ANeonGameMode::InitVehicle(ANeonVehiclePawn* Vehicle, int32 Slot, const FTransform* SpawnOverride)
{
	if (!Vehicle || !Arena)
	{
		return;
	}

	Vehicle->SetPlayerSlot(Slot);
	if (SlotColors.IsValidIndex(Slot))
	{
		Vehicle->SetVehicleColor(SlotColors[Slot]);
	}

	if (UNeonMovementComponent* Movement = Vehicle->GetMovement())
	{
		Movement->SetArena(Arena); // bounds for the sim, floor height for presentation (D25)
		Movement->OnEliminated.RemoveDynamic(this, &ANeonGameMode::HandleVehicleEliminated);
		Movement->OnEliminated.AddDynamic(this, &ANeonGameMode::HandleVehicleEliminated);
		// Nothing moves until BeginPlaying (a late joiner mid-round drives immediately).
		const ANeonGameState* GS = GetNeonGameState();
		Movement->SetFrozen(!GS || GS->GetPhase() != ENeonMatchPhase::Playing);
	}
	if (UNeonPowerupComponent* Inventory = Vehicle->GetPowerups())
	{
		Inventory->Config = PowerupConfig;
	}

	Vehicles.AddUnique(Vehicle);
	Vehicle->ResetToSpawn(SpawnOverride ? *SpawnOverride : Arena->GetSpawnTransform(Slot));
	RoundStartTicks.Add(Vehicle, Vehicle->GetMovement() ? Vehicle->GetMovement()->GetSimulationTick() : 0);
}

void ANeonGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->IsPendingKillPending())
	{
		return;
	}

	EnsureArena();

	if (!Arena)
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("No arena available; falling back to default restart."));
		Super::RestartPlayer(NewPlayer);
		return;
	}

	const int32 Slot = GetPlayerSlot(NewPlayer);
	RestartPlayerAtTransform(NewPlayer, Arena->GetSpawnTransform(Slot));

	if (ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(NewPlayer->GetPawn()))
	{
		InitVehicle(Vehicle, Slot);
	}
}

void ANeonGameMode::SpawnAIPlayers(int32 Count, ENeonAIDifficulty Level)
{
	UWorld* World = GetWorld();
	if (!World || !Arena || !DefaultPawnClass || !AIControllerClass || Count <= 0)
	{
		return;
	}

	// AI takes the slots after the human players. Their spots are random; the humans stay on the corners.
	const int32 HumanCount = FMath::Max(CountHumans(), 1);
	TArray<FVector2D> Occupied;
	for (const ANeonVehiclePawn* Existing : Vehicles)
	{
		if (IsValid(Existing))
		{
			const FVector L = Existing->GetActorLocation();
			Occupied.Add(FVector2D(L.X, L.Y));
		}
	}
	const int32 Round = GetNeonGameState() ? GetNeonGameState()->GetRoundNumber() : 0;
	FRandomStream Rng(AISeed + Round * 97 + 11);

	for (int32 i = 0; i < Count; ++i)
	{
		const int32 Slot = HumanCount + i;
		if (Slot > 3)
		{
			break;
		}

		const FTransform Spawn = MakeSpawnTransform(Slot, true, Rng, Occupied);
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ANeonVehiclePawn* Vehicle = World->SpawnActor<ANeonVehiclePawn>(DefaultPawnClass, Spawn, Params);
		if (!Vehicle)
		{
			continue;
		}
		InitVehicle(Vehicle, Slot, &Spawn);

		ANeonAIController* AI = World->SpawnActor<ANeonAIController>(AIControllerClass, Params);
		if (!AI)
		{
			UE_LOG(LogNeonGrid, Error, TEXT("Failed to spawn AI controller for slot %d."), Slot);
			Vehicle->Destroy();
			Vehicles.Remove(Vehicle);
			continue;
		}
		AI->ConfigureAI(AIConfig, Level, AISeed + Slot);
		AI->Possess(Vehicle);
		AIControllers.Add(AI);

		if (ANeonPlayerState* PS = AI->GetPlayerState<ANeonPlayerState>())
		{
			PS->SetSlot(Slot);
			PS->SetIsAI(true);
			PS->SetPlayerName(SlotLabel(Slot));
			if (SlotColors.IsValidIndex(Slot))
			{
				PS->SetColor(SlotColors[Slot]);
			}
		}

		UE_LOG(LogNeonGrid, Log, TEXT("Spawned AI %s (%s) in slot %d."), *GetNameSafe(Vehicle), *UEnum::GetValueAsString(Level), Slot);
	}
}

void ANeonGameMode::DestroyAllVehicles()
{
	for (ANeonAIController* AI : AIControllers)
	{
		if (IsValid(AI))
		{
			APawn* Pawn = AI->GetPawn();
			AI->UnPossess();
			if (IsValid(Pawn))
			{
				Pawn->Destroy();
			}
			AI->Destroy(); // also removes its player state from the game state
		}
	}
	AIControllers.Reset();

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}
		APawn* Pawn = PC->GetPawn();
		PC->UnPossess();
		if (IsValid(Pawn))
		{
			Pawn->Destroy();
		}
	}

	Vehicles.Reset();
	RoundStartTicks.Reset();

	if (UNeonCollisionSystem* Collision = GetWorld() ? GetWorld()->GetSubsystem<UNeonCollisionSystem>() : nullptr)
	{
		Collision->ClearAllTrails();
	}
	if (PowerupSpawner)
	{
		PowerupSpawner->SetSpawning(false);
		PowerupSpawner->ClearPickups();
	}
}

void ANeonGameMode::ResetVehiclesToSpawn()
{
	if (!Arena)
	{
		return;
	}

	if (UNeonCollisionSystem* Collision = GetWorld() ? GetWorld()->GetSubsystem<UNeonCollisionSystem>() : nullptr)
	{
		Collision->ClearAllTrails();
	}

	TArray<FVector2D> Occupied;
	const int32 Round = GetNeonGameState() ? GetNeonGameState()->GetRoundNumber() : 0;
	FRandomStream Rng(AISeed + Round * 97 + 53);
	for (ANeonVehiclePawn* Vehicle : Vehicles)
	{
		if (!IsValid(Vehicle))
		{
			continue;
		}
		ANeonPlayerState* PS = FindPlayerStateForVehicle(Vehicle);
		const bool bAI = PS && PS->IsAI();
		Vehicle->ResetToSpawn(MakeSpawnTransform(Vehicle->GetPlayerSlot(), bAI, Rng, Occupied));
		RoundStartTicks.Add(Vehicle, Vehicle->GetMovement() ? Vehicle->GetMovement()->GetSimulationTick() : 0);
		if (PS)
		{
			PS->SetAlive(true);
		}
	}

	for (ANeonAIController* AI : AIControllers)
	{
		if (IsValid(AI))
		{
			AI->ResetBrain();
		}
	}
}

void ANeonGameMode::SetAllFrozen(bool bFrozen)
{
	for (ANeonVehiclePawn* Vehicle : Vehicles)
	{
		if (IsValid(Vehicle) && Vehicle->GetMovement())
		{
			Vehicle->GetMovement()->SetFrozen(bFrozen);
		}
	}
}

void ANeonGameMode::ResetScores()
{
	if (!GameState)
	{
		return;
	}
	for (APlayerState* PS : GameState->PlayerArray)
	{
		if (ANeonPlayerState* NPS = Cast<ANeonPlayerState>(PS))
		{
			NPS->SetRoundWins(0);
			NPS->SetAlive(true);
		}
	}
	if (ANeonGameState* GS = GetNeonGameState())
	{
		GS->SetRoundNumber(0);
		GS->SetRoundWinnerSlot(-1);
		GS->SetMatchWinnerSlot(-1);
	}
}

// ---------------------------------------------------------------------------
// Match flow
// ---------------------------------------------------------------------------

void ANeonGameMode::ClearTimers()
{
	GetWorldTimerManager().ClearTimer(PhaseTimer);
}

void ANeonGameMode::StartSoloMatch(const FNeonMatchSettings& Settings)
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS || !Arena)
	{
		UE_LOG(LogNeonGrid, Error, TEXT("StartSoloMatch: missing game state or arena."));
		return;
	}
	if (GS->GetPhase() != ENeonMatchPhase::MainMenu)
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("StartSoloMatch ignored: phase is %s."), *UEnum::GetValueAsString(GS->GetPhase()));
		return;
	}

	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->MatchSettings = Settings;
	}

	ClearTimers();
	DestroyAllVehicles();
	GS->SetScoreLimit(FMath::Max(Settings.ScoreLimit, 1));
	GS->SetArenaCollapseEnabled(Settings.bArenaCollapse);
	ResetScores();
	if (PowerupSpawner)
	{
		PowerupSpawner->Configure(Arena, PowerupConfig, Settings.bPowerupsEnabled, AISeed);
	}

	// Leave MainMenu first so PlayerCanRestart allows the spawn.
	GS->SetPhase(ENeonMatchPhase::Countdown, 0.f);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			RestartPlayer(PC);
		}
	}

	const int32 MaxAI = FMath::Clamp(4 - CountHumans(), 0, 3);
	SpawnAIPlayers(FMath::Clamp(Settings.AIPlayerCount, 0, MaxAI), Settings.AIDifficulty);

	UE_LOG(LogNeonGrid, Log, TEXT("Match started: %d humans, %d AI (%s), first to %d, powerups %s, collapse %s."),
		CountHumans(), AIControllers.Num(), *UEnum::GetValueAsString(Settings.AIDifficulty), GS->GetScoreLimit(),
		Settings.bPowerupsEnabled ? TEXT("on") : TEXT("off"), Settings.bArenaCollapse ? TEXT("on") : TEXT("off"));

	BeginCountdown();
}

void ANeonGameMode::Rematch()
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS || GS->GetPhase() != ENeonMatchPhase::MatchResults)
	{
		return;
	}
	ClearTimers();
	ResetScores();
	UE_LOG(LogNeonGrid, Log, TEXT("Rematch."));
	BeginCountdown();
}

void ANeonGameMode::ReturnToMainMenu()
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS)
	{
		return;
	}
	ClearTimers();
	DestroyAllVehicles();
	ResetScores();
	GS->SetPhase(ENeonMatchPhase::MainMenu, 0.f);
	UE_LOG(LogNeonGrid, Log, TEXT("Returned to main menu."));
}

void ANeonGameMode::BeginCountdown()
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS)
	{
		return;
	}

	GS->SetRoundNumber(GS->GetRoundNumber() + 1);
	GS->SetRoundWinnerSlot(-1);
	if (Arena)
	{
		// A placed arena can still be carrying an older collapse. Match the snappier defaults (D37).
		Arena->CollapseStartDelay = 10.f;
		Arena->CollapseDuration = 45.f;
		Arena->LaunchRampHeight = 420.f;
		Arena->LaunchRampLength = 2800.f;
		Arena->LaunchRampHalfWidth = 1200.f;
		Arena->LaunchRampLipFraction = 0.5f;
		Arena->SetLayout(1);
	}
	ResetVehiclesToSpawn();
	SetAllFrozen(true);
	if (PowerupSpawner)
	{
		PowerupSpawner->SetSpawning(false);
		PowerupSpawner->ClearPickups();
	}

	const float Duration = FMath::Max(CountdownDuration, 0.f);
	GS->SetPhase(ENeonMatchPhase::Countdown, Duration);
	UE_LOG(LogNeonGrid, Log, TEXT("Round %d (%s): countdown %.1f s."), GS->GetRoundNumber(), Arena ? *Arena->GetLayoutName().ToString() : TEXT("no arena"), Duration);

	if (Duration > 0.f)
	{
		GetWorldTimerManager().SetTimer(PhaseTimer, this, &ANeonGameMode::BeginPlaying, Duration, false);
	}
	else
	{
		BeginPlaying();
	}
}

void ANeonGameMode::BeginPlaying()
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS)
	{
		return;
	}
	SetAllFrozen(false);
	if (PowerupSpawner)
	{
		PowerupSpawner->SetSpawning(true);
	}
	GS->MarkRoundStart();
	GS->SetPhase(ENeonMatchPhase::Playing, 0.f);
	UE_LOG(LogNeonGrid, Log, TEXT("Round %d: GO."), GS->GetRoundNumber());
}

void ANeonGameMode::HandleVehicleEliminated(AActor* VehicleActor, ENeonEliminationCause Cause)
{
	UNeonCollisionSystem* Collision = GetWorld() ? GetWorld()->GetSubsystem<UNeonCollisionSystem>() : nullptr;
	const int32 Alive = Collision ? Collision->CountAliveVehicles() : 0;

	ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(VehicleActor);
	int32 SurvivedTicks = -1;
	if (Vehicle)
	{
		if (const int32* Start = RoundStartTicks.Find(Vehicle))
		{
			SurvivedTicks = Vehicle->GetMovement() ? Vehicle->GetMovement()->GetSimulationTick() - *Start : -1;
		}
		if (ANeonPlayerState* PS = FindPlayerStateForVehicle(Vehicle))
		{
			PS->SetAlive(false);
		}
	}

	UE_LOG(LogNeonGrid, Log, TEXT("%s out (%s) after %d ticks. %d alive."), *GetNameSafe(VehicleActor), *UEnum::GetValueAsString(Cause), SurvivedTicks, Alive);

	ANeonGameState* GS = GetNeonGameState();
	if (!GS || GS->GetPhase() != ENeonMatchPhase::Playing)
	{
		return;
	}

	// Survival rule: the round ends when one (or none, on a simultaneous hit) remains.
	const int32 EndAtAlive = bRoundEndsAtZero ? 0 : 1;
	if (Alive > EndAtAlive)
	{
		return;
	}

	int32 WinnerSlot = -1;
	if (Alive == 1)
	{
		for (const ANeonVehiclePawn* V : Vehicles)
		{
			if (IsValid(V) && V->GetMovement() && V->GetMovement()->IsAlive())
			{
				WinnerSlot = V->GetPlayerSlot();
				break;
			}
		}
	}
	EndRound(WinnerSlot);
}

void ANeonGameMode::EndRound(int32 WinnerSlot)
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS)
	{
		return;
	}

	SetAllFrozen(true);
	if (PowerupSpawner)
	{
		PowerupSpawner->SetSpawning(false);
	}
	GS->SetRoundWinnerSlot(WinnerSlot);

	int32 WinnerWins = 0;
	if (ANeonPlayerState* Winner = GS->FindPlayerStateBySlot(WinnerSlot))
	{
		Winner->SetRoundWins(Winner->GetRoundWins() + 1);
		WinnerWins = Winner->GetRoundWins();
	}

	FString Scores;
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		if (const ANeonPlayerState* PS = GS->FindPlayerStateBySlot(Slot))
		{
			Scores += FString::Printf(TEXT("%s %d  "), *PS->GetPlayerName(), PS->GetRoundWins());
		}
	}
	UE_LOG(LogNeonGrid, Log, TEXT("Round %d over. Winner: %s. Scores: %s"),
		GS->GetRoundNumber(), WinnerSlot >= 0 ? *SlotLabel(WinnerSlot) : TEXT("none"), *Scores);

	const float Duration = FMath::Max(RoundResultsDuration, 0.f);
	GS->SetPhase(ENeonMatchPhase::RoundResults, Duration);

	if (WinnerWins >= GS->GetScoreLimit())
	{
		FTimerDelegate Delegate = FTimerDelegate::CreateUObject(this, &ANeonGameMode::BeginMatchResults, WinnerSlot);
		if (Duration > 0.f)
		{
			GetWorldTimerManager().SetTimer(PhaseTimer, Delegate, Duration, false);
		}
		else
		{
			BeginMatchResults(WinnerSlot);
		}
	}
	else if (Duration > 0.f)
	{
		GetWorldTimerManager().SetTimer(PhaseTimer, this, &ANeonGameMode::BeginCountdown, Duration, false);
	}
	else
	{
		BeginCountdown();
	}
}

void ANeonGameMode::BeginMatchResults(int32 WinnerSlot)
{
	ANeonGameState* GS = GetNeonGameState();
	if (!GS)
	{
		return;
	}
	GS->SetMatchWinnerSlot(WinnerSlot);
	GS->SetPhase(ENeonMatchPhase::MatchResults, 0.f);
	UE_LOG(LogNeonGrid, Log, TEXT("Match over. %s wins %d rounds."), *SlotLabel(WinnerSlot), GS->GetScoreLimit());
}
