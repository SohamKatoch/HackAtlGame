#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "NeonPlayerState.generated.h"

/**
 * Per-player match data (guide section 42): slot, color, round wins, alive flag, AI flag.
 * Humans and AI both have one (the AI controller requests a player state), so the HUD and the
 * score system treat every participant the same way. Replicated to clients; written only by the
 * server (game mode).
 */
UCLASS()
class NEONGRID_API ANeonPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	ANeonPlayerState();

	UFUNCTION(BlueprintPure, Category = "Neon|Player")
	int32 GetSlot() const { return Slot; }

	UFUNCTION(BlueprintPure, Category = "Neon|Player")
	FLinearColor GetColor() const { return Color; }

	UFUNCTION(BlueprintPure, Category = "Neon|Player")
	int32 GetRoundWins() const { return RoundWins; }

	UFUNCTION(BlueprintPure, Category = "Neon|Player")
	bool IsAlive() const { return bAlive; }

	UFUNCTION(BlueprintPure, Category = "Neon|Player")
	bool IsAI() const { return bIsAI; }

	// Server-only setters.
	void SetSlot(int32 InSlot) { Slot = InSlot; }
	void SetColor(const FLinearColor& InColor) { Color = InColor; }
	void SetRoundWins(int32 InWins) { RoundWins = InWins; }
	void SetAlive(bool bInAlive) { bAlive = bInAlive; }
	void SetIsAI(bool bInAI) { bIsAI = bInAI; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UPROPERTY(Replicated)
	int32 Slot = 0;

	UPROPERTY(Replicated)
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(Replicated)
	int32 RoundWins = 0;

	UPROPERTY(Replicated)
	bool bAlive = true;

	UPROPERTY(Replicated)
	bool bIsAI = false;
};
