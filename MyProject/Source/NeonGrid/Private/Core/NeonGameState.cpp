#include "Core/NeonGameState.h"

#include "Core/NeonPlayerState.h"
#include "Net/UnrealNetwork.h"

ANeonGameState::ANeonGameState()
{
}

float ANeonGameState::GetPhaseTimeRemaining() const
{
	if (PhaseEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(PhaseEndServerTime - GetServerWorldTimeSeconds(), 0.f);
}

float ANeonGameState::GetRoundElapsed() const
{
	if (Phase != ENeonMatchPhase::Playing || RoundStartServerTime <= 0.f)
	{
		return -1.f;
	}
	return FMath::Max(GetServerWorldTimeSeconds() - RoundStartServerTime, 0.f);
}

ANeonPlayerState* ANeonGameState::FindPlayerStateBySlot(int32 Slot) const
{
	for (APlayerState* PS : PlayerArray)
	{
		ANeonPlayerState* NPS = Cast<ANeonPlayerState>(PS);
		if (NPS && NPS->GetSlot() == Slot)
		{
			return NPS;
		}
	}
	return nullptr;
}

void ANeonGameState::SetPhase(ENeonMatchPhase NewPhase, float Duration)
{
	Phase = NewPhase;
	PhaseEndServerTime = Duration > 0.f ? GetServerWorldTimeSeconds() + Duration : 0.f;
	OnPhaseChanged.Broadcast(Phase);
}

void ANeonGameState::OnRep_Phase()
{
	OnPhaseChanged.Broadcast(Phase);
}

void ANeonGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANeonGameState, Phase);
	DOREPLIFETIME(ANeonGameState, RoundNumber);
	DOREPLIFETIME(ANeonGameState, ScoreLimit);
	DOREPLIFETIME(ANeonGameState, PhaseEndServerTime);
	DOREPLIFETIME(ANeonGameState, RoundWinnerSlot);
	DOREPLIFETIME(ANeonGameState, MatchWinnerSlot);
	DOREPLIFETIME(ANeonGameState, RoundStartServerTime);
	DOREPLIFETIME(ANeonGameState, bArenaCollapseEnabled);
}
