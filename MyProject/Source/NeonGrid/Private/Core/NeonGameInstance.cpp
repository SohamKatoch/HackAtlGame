#include "Core/NeonGameInstance.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NeonGrid.h"

void UNeonGameInstance::HostListen()
{
	UE_LOG(LogNeonGrid, Log, TEXT("Hosting listen server on L_TheGrid."));
	UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/NeonGrid/Maps/L_TheGrid")), true, TEXT("listen"));
}

void UNeonGameInstance::JoinListen(const FString& Address)
{
	FString Clean = Address.TrimStartAndEnd();
	if (Clean.IsEmpty())
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("JoinListen: empty address."));
		return;
	}
	LastJoinAddress = Clean;
	UE_LOG(LogNeonGrid, Log, TEXT("Joining listen host at %s."), *Clean);
	if (APlayerController* PC = GetFirstLocalPlayerController())
	{
		PC->ClientTravel(Clean, TRAVEL_Absolute);
	}
	else if (GEngine)
	{
		GEngine->SetClientTravel(GetWorld(), *Clean, TRAVEL_Absolute);
	}
}
