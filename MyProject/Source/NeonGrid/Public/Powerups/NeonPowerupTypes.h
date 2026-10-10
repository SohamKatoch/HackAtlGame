#pragma once

#include "CoreMinimal.h"
#include "NeonPowerupTypes.generated.h"

/** The six v2 powerups (Docs/GAME_DESIGN.md section 9, DECISIONS.md D6). None = empty slot. */
UENUM(BlueprintType)
enum class ENeonPowerupType : uint8
{
	None,
	Boost,
	Shield,
	Phase,
	TrailCutter,
	EMP,
	Overdrive
};

/** What happened, for HUD / VFX / audio listeners. */
UENUM(BlueprintType)
enum class ENeonPowerupEvent : uint8
{
	Collected,
	Activated,
	Expired,
	ShieldBroken,
	EMPHit,
	Cleared
};

namespace NeonPowerups
{
	/** Display label, upper case, as the HUD shows it. */
	NEONGRID_API FText DisplayName(ENeonPowerupType Type);

	/** Identifying color: pickup, HUD, vehicle feedback. */
	NEONGRID_API FLinearColor Color(ENeonPowerupType Type);

	constexpr int32 TypeCount = 6;

	/** Boost, Shield, Phase, TrailCutter, EMP, Overdrive in that order. */
	NEONGRID_API ENeonPowerupType FromIndex(int32 Index);
}
