#include "AI/NeonAIConfig.h"

UNeonAIConfig::UNeonAIConfig()
{
	// Survival terms (survive bonus, free fraction, openness + escape) dominate every level. Chase is a
	// tie-breaker that grows with difficulty. Speed/radius/collision never change here (Docs/AI_SPEC.md section 5).
	// Horizons are in seconds and were scaled x1.3 when cruise dropped 2200 -> 1700 cm/s (D27) so the AI still
	// looks the same distance ahead (~2000-3300 cm). Levels differ in horizon and breadth.

	FNeonAIDifficultyParams Easy;
	Easy.DecisionInterval = 0.30f;
	Easy.ReactionTime = 0.30f;
	Easy.LookAheadTime = 1.15f;
	Easy.OpennessProbeDistance = 2000.f;
	Easy.ArcsPerSide = 1;
	Easy.PredictionSteps = 6;
	Easy.TurnAnglesPerArc = 2;
	Easy.OpponentExtrapolationTime = 0.65f;
	Easy.SafetyMargin = 20.f;
	Easy.MistakeChance = 0.20f;
	Easy.OpennessWeight = 0.6f;
	Easy.SpaceWeight = 0.5f;
	Easy.SpaceCellSize = 250.f;
	Easy.ChaseWeight = 0.12f;
	Easy.CutOffLeadTime = 1.05f;
	Easy.StraightBias = 0.15f;
	Easy.CommitBias = 0.3f;
	Easy.LoopAversion = 0.55f;
	Easy.PowerupSkill = 0.2f;
	Easy.CollectWeight = 0.55f;
	Easy.CollectRange = 3200.f;

	FNeonAIDifficultyParams Normal;
	Normal.DecisionInterval = 0.15f;
	Normal.ReactionTime = 0.15f;
	Normal.LookAheadTime = 1.55f;
	Normal.OpennessProbeDistance = 3000.f;
	Normal.ArcsPerSide = 2;
	Normal.PredictionSteps = 8;
	Normal.TurnAnglesPerArc = 3;
	Normal.OpponentExtrapolationTime = 1.3f;
	Normal.SafetyMargin = 30.f;
	Normal.MistakeChance = 0.08f;
	Normal.OpennessWeight = 1.0f;
	Normal.SpaceWeight = 1.5f;
	Normal.SpaceCellSize = 250.f;
	Normal.ChaseWeight = 0.32f;
	Normal.CutOffLeadTime = 1.0f;
	Normal.StraightBias = 0.1f;
	Normal.CommitBias = 0.3f;
	Normal.LoopAversion = 0.75f;
	Normal.PowerupSkill = 0.5f;
	Normal.CollectWeight = 1.15f;
	Normal.CollectRange = 4200.f;

	FNeonAIDifficultyParams Hard;
	Hard.DecisionInterval = 0.10f;
	Hard.ReactionTime = 0.08f;
	Hard.LookAheadTime = 1.9f;
	Hard.OpennessProbeDistance = 3500.f;
	Hard.ArcsPerSide = 2;
	Hard.PredictionSteps = 10;
	Hard.TurnAnglesPerArc = 4;
	Hard.OpponentExtrapolationTime = 1.6f;
	Hard.SafetyMargin = 40.f;
	Hard.MistakeChance = 0.02f;
	Hard.OpennessWeight = 1.2f;
	Hard.SpaceWeight = 2.0f;
	Hard.SpaceCellSize = 200.f;
	Hard.ChaseWeight = 0.5f;
	Hard.CutOffLeadTime = 0.95f;
	Hard.StraightBias = 0.08f;
	Hard.CommitBias = 0.3f;
	Hard.LoopAversion = 0.85f;
	Hard.PowerupSkill = 0.75f;
	Hard.CollectWeight = 1.35f;
	Hard.CollectRange = 4700.f;

	FNeonAIDifficultyParams Expert;
	Expert.DecisionInterval = 0.067f;
	Expert.ReactionTime = 0.05f;
	Expert.LookAheadTime = 2.3f;
	Expert.OpennessProbeDistance = 4000.f;
	Expert.ArcsPerSide = 3;
	Expert.PredictionSteps = 12;
	Expert.TurnAnglesPerArc = 5;
	Expert.OpponentExtrapolationTime = 1.95f;
	Expert.SafetyMargin = 50.f;
	Expert.MistakeChance = 0.0f;
	Expert.OpennessWeight = 1.4f;
	Expert.SpaceWeight = 2.5f;
	Expert.SpaceCellSize = 200.f;
	Expert.ChaseWeight = 0.65f;
	Expert.CutOffLeadTime = 0.9f;
	Expert.StraightBias = 0.05f;
	Expert.CommitBias = 0.3f;
	Expert.LoopAversion = 0.95f;
	Expert.PowerupSkill = 1.0f;
	Expert.CollectWeight = 1.5f;
	Expert.CollectRange = 5200.f;

	Levels.Add(ENeonAIDifficulty::Easy, Easy);
	Levels.Add(ENeonAIDifficulty::Normal, Normal);
	Levels.Add(ENeonAIDifficulty::Hard, Hard);
	Levels.Add(ENeonAIDifficulty::Expert, Expert);
}

const FNeonAIDifficultyParams& UNeonAIConfig::GetParams(ENeonAIDifficulty Difficulty) const
{
	if (const FNeonAIDifficultyParams* Found = Levels.Find(Difficulty))
	{
		return *Found;
	}
	if (const FNeonAIDifficultyParams* Normal = Levels.Find(ENeonAIDifficulty::Normal))
	{
		return *Normal;
	}
	static const FNeonAIDifficultyParams Fallback;
	return Fallback;
}
