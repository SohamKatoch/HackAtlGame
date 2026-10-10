#pragma once

#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "NeonGameInstance.generated.h"

/**
 * Process-lifetime state: the match settings chosen in the menus. Docs/TECHNICAL_SPEC.md section 8.
 * The game mode reads MatchSettings when a match starts. Nothing here is replicated; on LAN the
 * host's settings are pushed into ANeonGameState.
 */
UCLASS()
class NEONGRID_API UNeonGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "Neon|Match")
	FNeonMatchSettings MatchSettings;
};
