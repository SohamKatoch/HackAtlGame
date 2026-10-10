#include "Powerups/NeonPowerupConfig.h"

#define LOCTEXT_NAMESPACE "NeonGrid"

namespace NeonPowerups
{
	FText DisplayName(ENeonPowerupType Type)
	{
		switch (Type)
		{
		case ENeonPowerupType::Boost:       return LOCTEXT("PuBoost", "BOOST");
		case ENeonPowerupType::Shield:      return LOCTEXT("PuShield", "SHIELD");
		case ENeonPowerupType::Phase:       return LOCTEXT("PuPhase", "PHASE");
		case ENeonPowerupType::TrailCutter: return LOCTEXT("PuCutter", "TRAIL CUTTER");
		case ENeonPowerupType::EMP:         return LOCTEXT("PuEMP", "EMP");
		case ENeonPowerupType::Overdrive:   return LOCTEXT("PuOverdrive", "OVERDRIVE");
		default:                            return FText::GetEmpty();
		}
	}

	FLinearColor Color(ENeonPowerupType Type)
	{
		switch (Type)
		{
		case ENeonPowerupType::Boost:       return FLinearColor(1.00f, 0.55f, 0.10f);
		case ENeonPowerupType::Shield:      return FLinearColor(0.20f, 0.60f, 1.00f);
		case ENeonPowerupType::Phase:       return FLinearColor(0.70f, 0.30f, 1.00f);
		case ENeonPowerupType::TrailCutter: return FLinearColor(1.00f, 0.20f, 0.30f);
		case ENeonPowerupType::EMP:         return FLinearColor(1.00f, 0.95f, 0.20f);
		case ENeonPowerupType::Overdrive:   return FLinearColor(1.00f, 0.90f, 0.80f);
		default:                            return FLinearColor::White;
		}
	}

	ENeonPowerupType FromIndex(int32 Index)
	{
		switch (Index)
		{
		case 0: return ENeonPowerupType::Boost;
		case 1: return ENeonPowerupType::Shield;
		case 2: return ENeonPowerupType::Phase;
		case 3: return ENeonPowerupType::TrailCutter;
		case 4: return ENeonPowerupType::EMP;
		case 5: return ENeonPowerupType::Overdrive;
		default: return ENeonPowerupType::None;
		}
	}
}

UNeonPowerupConfig::UNeonPowerupConfig()
{
	for (int32 i = 0; i < NeonPowerups::TypeCount; ++i)
	{
		SpawnWeights.Add(NeonPowerups::FromIndex(i), 1.f);
	}
}

ENeonPowerupType UNeonPowerupConfig::PickRandomType(FRandomStream& Rng) const
{
	float Total = 0.f;
	for (int32 i = 0; i < NeonPowerups::TypeCount; ++i)
	{
		if (const float* W = SpawnWeights.Find(NeonPowerups::FromIndex(i)))
		{
			Total += FMath::Max(*W, 0.f);
		}
	}
	if (Total <= KINDA_SMALL_NUMBER)
	{
		return ENeonPowerupType::None;
	}

	float Roll = Rng.FRand() * Total;
	for (int32 i = 0; i < NeonPowerups::TypeCount; ++i)
	{
		const ENeonPowerupType Type = NeonPowerups::FromIndex(i);
		const float* W = SpawnWeights.Find(Type);
		const float Weight = W ? FMath::Max(*W, 0.f) : 0.f;
		if (Weight <= 0.f)
		{
			continue;
		}
		if (Roll < Weight)
		{
			return Type;
		}
		Roll -= Weight;
	}
	return NeonPowerups::FromIndex(NeonPowerups::TypeCount - 1);
}

#undef LOCTEXT_NAMESPACE
