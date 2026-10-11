#pragma once

#include "AI/NeonAIConfig.h"
#include "CoreMinimal.h"
#include "NeonMatchTypes.generated.h"

/**
 * Match flow phases. Docs/GAME_DESIGN.md section 15, Docs/TECHNICAL_SPEC.md section 8.
 *
 * MainMenu covers the guide's MAIN_MENU and SOLO (solo lobby) states: both are menu screens shown
 * over the arena in the same level. LAN menu and lobby phases are added with Milestone 6.
 */
UENUM(BlueprintType)
enum class ENeonMatchPhase : uint8
{
	MainMenu,
	Countdown,
	Playing,
	RoundResults,
	MatchResults
};

/**
 * Settings chosen in the Solo menu (later also the LAN host lobby). Held by UNeonGameInstance so they
 * survive the menu -> match transition. Only exposes what is implemented (guide section 63).
 */
USTRUCT(BlueprintType)
struct NEONGRID_API FNeonMatchSettings
{
	GENERATED_BODY()

	/** Solo only. 1-3 AI opponents. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match", meta = (ClampMin = "0", ClampMax = "3"))
	int32 AIPlayerCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match")
	ENeonAIDifficulty AIDifficulty = ENeonAIDifficulty::Normal;

	/** Round wins needed to take the match. Guide default 5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match", meta = (ClampMin = "1", ClampMax = "20"))
	int32 ScoreLimit = 5;

	/** Spawn powerups during rounds (Milestone 4). Off gives a pure classic light-cycle match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match")
	bool bPowerupsEnabled = true;

	/** The walls close in during each round (DECISIONS.md D27). Off keeps the full arena all round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match")
	bool bArenaCollapse = true;

	/** The only arena that exists. Kept as data so the menu can list more later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neon|Match")
	FName ArenaId = TEXT("TheGrid");
};

/** One advertised listen host from a LAN session browse. */
USTRUCT(BlueprintType)
struct NEONGRID_API FNeonLanGame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	FString HostName;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	int32 Players = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	int32 MaxPlayers = 4;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	int32 PingMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	int32 SearchIndex = 0;

	/** IP or IP:port. Empty when the browse result did not resolve. */
	UPROPERTY(BlueprintReadOnly, Category = "Neon|Lan")
	FString Address;
};
