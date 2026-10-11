#include "Core/NeonGameInstance.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Kismet/GameplayStatics.h"
#include "NeonGrid.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"

#define LOCTEXT_NAMESPACE "NeonGrid"

namespace
{
	const FName NeonSessionName = NAME_GameSession;

	IOnlineSessionPtr SessionsFor(const UWorld* World)
	{
		IOnlineSubsystem* OSS = Online::GetSubsystem(World);
		return OSS ? OSS->GetSessionInterface() : nullptr;
	}
}

void UNeonGameInstance::Init()
{
	Super::Init();
	BindSessionDelegates();
}

void UNeonGameInstance::Shutdown()
{
	DestroyLanSession();
	UnbindSessionDelegates();
	Super::Shutdown();
}

void UNeonGameInstance::BindSessionDelegates()
{
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (!Sessions || bDelegatesBound)
	{
		return;
	}
	CreateHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &UNeonGameInstance::HandleCreateSession));
	FindHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(this, &UNeonGameInstance::HandleFindSessions));
	JoinHandle = Sessions->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateLambda([this](FName Name, EOnJoinSessionCompleteResult::Type Result)
		{
			HandleJoinSession(Name, Result == EOnJoinSessionCompleteResult::Success);
		}));
	bDelegatesBound = true;
}

void UNeonGameInstance::UnbindSessionDelegates()
{
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (Sessions && bDelegatesBound)
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateHandle);
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(JoinHandle);
	}
	bDelegatesBound = false;
}

void UNeonGameInstance::HostListen()
{
	BindSessionDelegates();
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (!Sessions)
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("No online subsystem; hosting listen without a browse listing."));
		LanStatus = LOCTEXT("HostNoOss", "LINK OPEN. BROWSE IS OFFLINE. OTHERS JOIN BY ADDRESS.");
		bHosting = true;
		TravelListen();
		return;
	}

	if (Sessions->GetNamedSession(NeonSessionName))
	{
		Sessions->DestroySession(NeonSessionName);
	}

	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = true;
	Settings.bUsesPresence = false;
	Settings.bAllowJoinInProgress = true;
	Settings.bShouldAdvertise = true;
	Settings.bUseLobbiesIfAvailable = false;
	Settings.NumPublicConnections = 4;
	Settings.Set(FName(TEXT("GAME")), FString(TEXT("NEON GRID")), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(FName(TEXT("HOST")), FString(FPlatformProcess::ComputerName()), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

	bTravelAfterCreate = true;
	LanStatus = LOCTEXT("Hosting", "OPENING LINK...");
	if (!Sessions->CreateSession(0, NeonSessionName, Settings))
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("CreateSession failed; hosting listen without a browse listing."));
		LanStatus = LOCTEXT("HostCreateFail", "LINK OPEN. BROWSE FAILED. OTHERS JOIN BY ADDRESS.");
		bHosting = true;
		TravelListen();
	}
}

void UNeonGameInstance::HandleCreateSession(FName SessionName, bool bSuccess)
{
	if (SessionName != NeonSessionName || !bTravelAfterCreate)
	{
		return;
	}
	bTravelAfterCreate = false;
	if (!bSuccess)
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("CreateSession returned false; hosting listen without a browse listing."));
		LanStatus = LOCTEXT("HostCreateFail", "LINK OPEN. BROWSE FAILED. OTHERS JOIN BY ADDRESS.");
	}
	else
	{
		LanStatus = LOCTEXT("HostReady", "LINK OPEN. WAITING FOR PILOTS ON THE NETWORK.");
	}
	bHosting = true;
	TravelListen();
}

void UNeonGameInstance::TravelListen()
{
	UE_LOG(LogNeonGrid, Log, TEXT("Hosting listen server on L_TheGrid."));
	UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/NeonGrid/Maps/L_TheGrid")), true, TEXT("listen"));
}

void UNeonGameInstance::FindLanGames()
{
	BindSessionDelegates();
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (!Sessions)
	{
		LanStatus = LOCTEXT("ScanNoOss", "BROWSE OFFLINE. USE NODE ADDRESS.");
		return;
	}
	if (bSearching)
	{
		return;
	}

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->bIsLanQuery = true;
	SessionSearch->MaxSearchResults = 16;
	SessionSearch->TimeoutInSeconds = 5.f;

	bSearching = true;
	LanGames.Reset();
	LanStatus = LOCTEXT("Scanning", "SCANNING NETWORK...");
	if (!Sessions->FindSessions(0, SessionSearch.ToSharedRef()))
	{
		bSearching = false;
		LanStatus = LOCTEXT("ScanFail", "SCAN FAILED. USE NODE ADDRESS.");
	}
}

void UNeonGameInstance::HandleFindSessions(bool bSuccess)
{
	bSearching = false;
	LanGames.Reset();
	if (!SessionSearch)
	{
		LanStatus = LOCTEXT("ScanEmpty", "NO GAMES ON THE NETWORK.");
		return;
	}

	int32 Index = 0;
	for (const FOnlineSessionSearchResult& Result : SessionSearch->SearchResults)
	{
		FNeonLanGame Game;
		Game.SearchIndex = Index++;
		FString Host;
		if (!Result.Session.SessionSettings.Get(FName(TEXT("HOST")), Host) || Host.IsEmpty())
		{
			Host = Result.Session.OwningUserName;
		}
		if (Host.IsEmpty())
		{
			Host = TEXT("GRID HOST");
		}
		Game.HostName = Host.ToUpper();
		Game.MaxPlayers = Result.Session.SessionSettings.NumPublicConnections;
		Game.Players = Game.MaxPlayers - Result.Session.NumOpenPublicConnections;
		Game.PingMs = Result.PingInMs;
		if (SessionsFor(GetWorld()))
		{
			FString Connect;
			if (SessionsFor(GetWorld())->GetResolvedConnectString(Result, NAME_GamePort, Connect))
			{
				Game.Address = Connect;
			}
		}
		LanGames.Add(Game);
	}

	if (!bSuccess || LanGames.Num() == 0)
	{
		LanStatus = LOCTEXT("ScanEmpty", "NO GAMES ON THE NETWORK.");
		return;
	}
	LanStatus = FText::Format(LOCTEXT("ScanHits", "{0} GRID{1} ON THE NETWORK."),
		LanGames.Num(), LanGames.Num() == 1 ? FText::GetEmpty() : FText::FromString(TEXT("S")));
}

void UNeonGameInstance::JoinLanGame(int32 SearchIndex)
{
	BindSessionDelegates();
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (!Sessions || !SessionSearch || !SessionSearch->SearchResults.IsValidIndex(SearchIndex))
	{
		LanStatus = LOCTEXT("JoinStale", "SCAN AGAIN, THEN JOIN.");
		return;
	}

	if (LanGames.IsValidIndex(SearchIndex) && !LanGames[SearchIndex].Address.IsEmpty())
	{
		JoinListen(LanGames[SearchIndex].Address);
		return;
	}

	LanStatus = LOCTEXT("JoiningSession", "LINKING TO HOST...");
	if (!Sessions->JoinSession(0, NeonSessionName, SessionSearch->SearchResults[SearchIndex]))
	{
		LanStatus = LOCTEXT("JoinFail", "LINK FAILED.");
	}
}

void UNeonGameInstance::HandleJoinSession(FName SessionName, bool bSuccess)
{
	if (SessionName != NeonSessionName)
	{
		return;
	}
	if (!bSuccess)
	{
		LanStatus = LOCTEXT("JoinFail", "LINK FAILED.");
		return;
	}

	FString ConnectInfo;
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (!Sessions || !Sessions->GetResolvedConnectString(SessionName, ConnectInfo) || ConnectInfo.IsEmpty())
	{
		LanStatus = LOCTEXT("NoConnect", "NO CONNECT STRING. USE NODE ADDRESS.");
		return;
	}
	JoinListen(ConnectInfo);
}

void UNeonGameInstance::JoinListen(const FString& Address)
{
	FString Clean = Address.TrimStartAndEnd();
	if (Clean.IsEmpty())
	{
		UE_LOG(LogNeonGrid, Warning, TEXT("JoinListen: empty address."));
		LanStatus = LOCTEXT("NeedIp", "ENTER THE HOST IPV4 ADDRESS.");
		return;
	}
	LastJoinAddress = Clean;
	LanStatus = FText::Format(LOCTEXT("Joining", "LINKING TO {0}..."), FText::FromString(Clean));
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

void UNeonGameInstance::DestroyLanSession()
{
	bHosting = false;
	bTravelAfterCreate = false;
	const IOnlineSessionPtr Sessions = SessionsFor(GetWorld());
	if (Sessions && Sessions->GetNamedSession(NeonSessionName))
	{
		Sessions->DestroySession(NeonSessionName);
	}
}

#undef LOCTEXT_NAMESPACE
