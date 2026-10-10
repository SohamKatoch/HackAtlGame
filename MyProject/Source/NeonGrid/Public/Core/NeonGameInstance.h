#pragma once

#include "Core/NeonMatchTypes.h"
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "NeonGameInstance.generated.h"

class FOnlineSessionSearch;

/**
 * Process-lifetime state: match settings and the LAN session (host / browse / join).
 * Docs/TECHNICAL_SPEC.md section 8, DECISIONS.md D28 and D34.
 */
UCLASS()
class NEONGRID_API UNeonGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

	UPROPERTY(BlueprintReadWrite, Category = "Neon|Match")
	FNeonMatchSettings MatchSettings;

	/** Last address typed in the LAN join box. */
	UPROPERTY(BlueprintReadWrite, Category = "Neon|Lan")
	FString LastJoinAddress = TEXT("127.0.0.1");

	/** Advertise this machine on the LAN, then reload L_TheGrid as a listen server. */
	void HostListen();

	/** Browse the local network for advertised Neon Grid hosts. */
	void FindLanGames();

	/** Join a result from the last FindLanGames call. */
	void JoinLanGame(int32 SearchIndex);

	/** Travel this client to a host at Address (IP or IP:port). Fallback when browse fails. */
	void JoinListen(const FString& Address);

	void DestroyLanSession();

	const TArray<FNeonLanGame>& GetLanGames() const { return LanGames; }
	FText GetLanStatus() const { return LanStatus; }
	bool IsSearchingLan() const { return bSearching; }
	bool IsHostingLan() const { return bHosting; }

protected:
	void HandleCreateSession(FName SessionName, bool bSuccess);
	void HandleFindSessions(bool bSuccess);
	void HandleJoinSession(FName SessionName, bool bSuccess);

	void BindSessionDelegates();
	void UnbindSessionDelegates();
	void TravelListen();

	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	UPROPERTY(Transient)
	TArray<FNeonLanGame> LanGames;

	FText LanStatus;
	bool bSearching = false;
	bool bHosting = false;
	bool bTravelAfterCreate = false;
	bool bDelegatesBound = false;

	FDelegateHandle CreateHandle;
	FDelegateHandle FindHandle;
	FDelegateHandle JoinHandle;
};
