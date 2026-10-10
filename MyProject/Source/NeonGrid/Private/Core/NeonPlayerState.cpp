#include "Core/NeonPlayerState.h"

#include "Net/UnrealNetwork.h"

ANeonPlayerState::ANeonPlayerState()
{
	// Scores change a few times a match; no need for a high update rate.
	SetNetUpdateFrequency(10.f);
}

void ANeonPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANeonPlayerState, Slot);
	DOREPLIFETIME(ANeonPlayerState, Color);
	DOREPLIFETIME(ANeonPlayerState, RoundWins);
	DOREPLIFETIME(ANeonPlayerState, bAlive);
	DOREPLIFETIME(ANeonPlayerState, bIsAI);
}
