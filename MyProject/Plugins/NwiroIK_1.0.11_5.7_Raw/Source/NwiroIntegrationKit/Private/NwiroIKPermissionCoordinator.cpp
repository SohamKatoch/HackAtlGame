// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKPermissionCoordinator.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

const TCHAR* LexToString(EAcpPermissionOutcome Outcome)
{
	switch (Outcome)
	{
	case EAcpPermissionOutcome::Allowed:      return TEXT("allowed");
	case EAcpPermissionOutcome::Denied:       return TEXT("denied");
	case EAcpPermissionOutcome::ShellBlocked: return TEXT("shell_blocked");
	case EAcpPermissionOutcome::Superseded:   return TEXT("superseded");
	case EAcpPermissionOutcome::Retired:      return TEXT("retired");
	case EAcpPermissionOutcome::Expired:      return TEXT("expired");
	}
	return TEXT("unknown");
}

const TCHAR* LexToString(EAcpPermissionReason Reason)
{
	switch (Reason)
	{
	case EAcpPermissionReason::UserChoice:        return TEXT("user_choice");
	case EAcpPermissionReason::OptionNotOffered:  return TEXT("option_not_offered");
	case EAcpPermissionReason::UnknownOptionKind: return TEXT("unknown_option_kind");
	case EAcpPermissionReason::NoAllowOption:     return TEXT("no_allow_option");
	case EAcpPermissionReason::ModeAutoApprove:   return TEXT("mode_auto_approve");
	case EAcpPermissionReason::ModeAutoDeny:      return TEXT("mode_auto_deny");
	case EAcpPermissionReason::ShellPolicy:       return TEXT("shell_policy");
	case EAcpPermissionReason::OriginRetired:     return TEXT("origin_retired");
	case EAcpPermissionReason::Deadline:          return TEXT("deadline");
	case EAcpPermissionReason::ContextRetired:    return TEXT("context_retired");
	case EAcpPermissionReason::Superseded:        return TEXT("superseded");
	}
	return TEXT("unknown");
}

bool IsPositiveAcpPermissionOutcome(EAcpPermissionOutcome Outcome)
{
	return Outcome == EAcpPermissionOutcome::Allowed;
}

void FAcpPermissionCoordinator::Configure(FAcpPermissionSinks InSinks, FAcpPermissionLiveContext InLive)
{
	Sinks = MoveTemp(InSinks);
	Live  = MoveTemp(InLive);
}

bool FAcpPermissionCoordinator::IsOriginStillLive(
	const FAcpPermissionRequest& Request, const FAcpPermissionLiveContext& Live)
{
	// Delegated wholesale: only the bridge can prove the RELATIONSHIP between process,
	// session, chat and turn. Checking those as independent facts (the previous shape)
	// passes for a request whose session belongs to another chat, or whose chat has since
	// switched adapters — each fact true, the ownership false.
	//
	// No callback configured = no proof of ownership = refuse.
	if (!Live.IsOriginLive) return false;
	return Live.IsOriginLive(Request);
}

// EXACT match against the ACP option-kind vocabulary, not substring.
//
// `Contains("allow")` classified `not_allow_once` — and anything else containing the
// substring — as an approval. A kind is a closed enum on the wire; treating it as free
// text means any unrecognised or hostile value can be read as permission. Anything not on
// this list is Unknown, and Unknown is never an approval.
EAcpOptionSemantics FAcpPermissionCoordinator::ClassifyOption(const FAcpPermissionOption& Option)
{
	static const TCHAR* AllowKinds[] = { TEXT("allow_once"), TEXT("allow_always") };
	static const TCHAR* DenyKinds[]  = { TEXT("reject_once"), TEXT("reject_always") };

	for (const TCHAR* Kind : DenyKinds)
	{
		if (Option.Kind.Equals(Kind, ESearchCase::IgnoreCase)) return EAcpOptionSemantics::Deny;
	}
	for (const TCHAR* Kind : AllowKinds)
	{
		if (Option.Kind.Equals(Kind, ESearchCase::IgnoreCase)) return EAcpOptionSemantics::Allow;
	}
	return EAcpOptionSemantics::Unknown;
}

bool FAcpPermissionCoordinator::IsPersistentOption(const FAcpPermissionOption& Option)
{
	return Option.Kind.Equals(TEXT("allow_always"), ESearchCase::IgnoreCase)
		|| Option.Kind.Equals(TEXT("reject_always"), ESearchCase::IgnoreCase);
}

FString FAcpPermissionCoordinator::FindOptionByKind(
	const TArray<FAcpPermissionOption>& Options, const FString& KindMatch, bool bAllowPersistent)
{
	const bool bWantAllow = KindMatch.Contains(TEXT("allow"), ESearchCase::IgnoreCase)
		|| KindMatch.Contains(TEXT("approve"), ESearchCase::IgnoreCase);
	const EAcpOptionSemantics Wanted = bWantAllow ? EAcpOptionSemantics::Allow : EAcpOptionSemantics::Deny;

	// Scoped first, so option ORDER can never decide scope.
	for (const FAcpPermissionOption& Option : Options)
	{
		if (ClassifyOption(Option) != Wanted) continue;
		if (!IsPersistentOption(Option)) return Option.OptionId;
	}

	// A persistent option is a DIFFERENT decision, not a fallback for the scoped one.
	// Auto-approval selecting `allow_always` would grant the agent a standing permission
	// the user never chose — and switching back to default mode next turn would then
	// silently stop prompting. Automatic paths pass bAllowPersistent=false and get
	// nothing, which the caller turns into a prompt or a denial.
	if (!bAllowPersistent) return FString();

	for (const FAcpPermissionOption& Option : Options)
	{
		if (ClassifyOption(Option) == Wanted) return Option.OptionId;
	}
	return FString();
}

int32 FAcpPermissionCoordinator::AddPending(const FAcpPermissionRequest& Request)
{
	// Supersede older cards for the same chat HERE, on the backend, so the previous
	// request is answered and the renderer is told. Expiring them in the renderer left
	// the agent waiting on a request nobody would ever respond to.
	TArray<int32> Older;
	for (const TPair<int32, FAcpPermissionRequest>& Pair : Pending)
	{
		if (Pair.Value.ChatId == Request.ChatId) Older.Add(Pair.Key);
	}
	for (int32 OlderId : Older)
	{
		SettleInternal(OlderId, EAcpPermissionOutcome::Superseded, EAcpPermissionReason::Superseded, FString());
	}

	const int32 LocalPermId = NextLocalPermId--;
	Pending.Add(LocalPermId, Request);
	return LocalPermId;
}

void FAcpPermissionCoordinator::SettleByClick(int32 LocalPermId, const FString& OptionId)
{
	const FAcpPermissionRequest* Request = Pending.Find(LocalPermId);
	if (!Request) return;

	// Membership first: the id must be one the agent actually offered for THIS request.
	const FAcpPermissionOption* Clicked = Request->Options.FindByPredicate(
		[&OptionId](const FAcpPermissionOption& Option) { return Option.OptionId == OptionId; });

	if (!Clicked)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: permission click '%s' is not an option this request offered — denying"),
			*OptionId);
		SettleInternal(LocalPermId, EAcpPermissionOutcome::Denied,
			EAcpPermissionReason::OptionNotOffered, FString());
		return;
	}

	const EAcpOptionSemantics Semantics = ClassifyOption(*Clicked);
	if (Semantics == EAcpOptionSemantics::Unknown)
	{
		// An offered option whose kind we cannot read is not an approval. Settle as a
		// denial and let the reject-option lookup pick the wire response.
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: permission option '%s' has unreadable kind '%s' — denying"),
			*Clicked->OptionId, *Clicked->Kind);
		SettleInternal(LocalPermId, EAcpPermissionOutcome::Denied,
			EAcpPermissionReason::UnknownOptionKind, FString());
		return;
	}

	SettleInternal(
		LocalPermId,
		Semantics == EAcpOptionSemantics::Deny ? EAcpPermissionOutcome::Denied : EAcpPermissionOutcome::Allowed,
		EAcpPermissionReason::UserChoice,
		OptionId);
}

int32 FAcpPermissionCoordinator::RetireMatching(
	const FString& AdapterId, const FString& AcpSessionId, const FString& ChatId,
	EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason, uint64 TurnGeneration)
{
	// All-empty is not "everything" — it is a caller that lost its scope. CloseChat on a
	// chat with no session did exactly this and retired every other chat's cards.
	if (AdapterId.IsEmpty() && AcpSessionId.IsEmpty() && ChatId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: RetireMatching called with no scope — refusing. Use RetireAll if that is the intent."));
		return 0;
	}

	TArray<int32> Matches;
	for (const TPair<int32, FAcpPermissionRequest>& Pair : Pending)
	{
		if (!AdapterId.IsEmpty()     && Pair.Value.AdapterId    != AdapterId)     continue;
		if (!AcpSessionId.IsEmpty()  && Pair.Value.AcpSessionId != AcpSessionId)  continue;
		if (!ChatId.IsEmpty()        && Pair.Value.ChatId       != ChatId)        continue;
		// A DEFERRED caller (the stall watchdog) decided to cancel one specific turn some
		// time ago. Its cards are the only ones it may retire — a card raised by a turn
		// that started since then belongs to a decision nobody made.
		if (TurnGeneration != 0      && Pair.Value.TurnGeneration != TurnGeneration) continue;
		Matches.Add(Pair.Key);
	}
	for (int32 Match : Matches)
	{
		SettleInternal(Match, Outcome, Reason, FString());
	}
	return Matches.Num();
}

int32 FAcpPermissionCoordinator::RetireAll(EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason)
{
	TArray<int32> All;
	Pending.GetKeys(All);
	for (int32 Id : All)
	{
		SettleInternal(Id, Outcome, Reason, FString());
	}
	return All.Num();
}

int32 FAcpPermissionCoordinator::SettleExpired()
{
	if (!Sinks.NowSeconds) return 0;
	const double Now = Sinks.NowSeconds();

	TArray<int32> Stale;
	for (const TPair<int32, FAcpPermissionRequest>& Pair : Pending)
	{
		if (Pair.Value.ExpiresAtSeconds > 0.0 && Now >= Pair.Value.ExpiresAtSeconds)
		{
			Stale.Add(Pair.Key);
		}
	}
	for (int32 Id : Stale)
	{
		SettleInternal(Id, EAcpPermissionOutcome::Expired, EAcpPermissionReason::Deadline, FString());
	}
	return Stale.Num();
}

void FAcpPermissionCoordinator::SettleImmediate(
	const FAcpPermissionRequest& Request, EAcpPermissionOutcome Outcome,
	EAcpPermissionReason Reason, const FString& OptionId)
{
	Emit(/*LocalPermId*/ 0, Request, Outcome, Reason, OptionId);
}

void FAcpPermissionCoordinator::SettleInternal(
	int32 LocalPermId, EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason, const FString& OptionId)
{
	FAcpPermissionRequest Request;
	// REMOVE FIRST. A double-click, or a click racing a retirement, must not be able to
	// answer the same ACP request twice — and neither must a supersede racing an expiry.
	if (!Pending.RemoveAndCopyValue(LocalPermId, Request)) return;

	Emit(LocalPermId, Request, Outcome, Reason, OptionId);
}

void FAcpPermissionCoordinator::Emit(
	int32 LocalPermId, const FAcpPermissionRequest& Request,
	EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason, const FString& OptionId)
{
	FString EffectiveOptionId = OptionId;
	EAcpPermissionOutcome EffectiveOutcome = Outcome;
	EAcpPermissionReason  EffectiveReason  = Reason;

	// A positive outcome is the only one that needs the origin to still be live. Granting
	// something to a retired turn is the actual hazard; refusing one is always safe.
	if (IsPositiveAcpPermissionOutcome(Outcome) && !IsOriginStillLive(Request, Live))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: refusing to allow '%s' — origin context retired (chat '%s', turn %llu)"),
			*Request.ToolTitle, *Request.ChatId, Request.TurnGeneration);
		EffectiveOutcome  = EAcpPermissionOutcome::Retired;
		EffectiveReason   = EAcpPermissionReason::OriginRetired;
		EffectiveOptionId = FString();
	}

	if (EffectiveOptionId.IsEmpty())
	{
		// Never invent an option id. If the agent offered nothing we can map to, we send
		// no wire response at all rather than guessing a string it might treat as allow.
		// (The shell and auto-deny paths used to fall back to a literal "deny", which is
		// only correct by coincidence of the vocabularies we happen to have seen.)
		const bool bWantAllow = IsPositiveAcpPermissionOutcome(EffectiveOutcome);
		EffectiveOptionId = FindOptionByKind(Request.Options, bWantAllow ? TEXT("allow") : TEXT("reject"));

		// An ALLOW we cannot express is not an allow. Reporting "Allowed" to the UI while
		// sending nothing on the wire would show the user an approval that never happened.
		if (bWantAllow && EffectiveOptionId.IsEmpty())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: '%s' has no expressible allow option — reporting denied, not allowed"),
				*Request.ToolTitle);
			EffectiveOutcome = EAcpPermissionOutcome::Denied;
			EffectiveReason  = EAcpPermissionReason::NoAllowOption;
			EffectiveOptionId = FindOptionByKind(Request.Options, TEXT("reject"));
		}
	}

	// Wire response: only while the ORIGIN process is still the live one. After a restart
	// the replacement process mints ACP ids from the same space, so a late response can
	// land on an unrelated request.
	const bool bProcessStillLive = Live.IsOriginProcessLive && Live.IsOriginProcessLive(Request);

	if (bProcessStillLive && Sinks.RespondToAcp)
	{
		// Always answer a live process, even with no nameable option. An empty option id
		// is the signal for an ACP `cancelled` outcome (see RespondToAcpPermission) —
		// vocabulary-independent, grants nothing, and ends the request. Staying silent
		// was authorization-safe but lifecycle-broken: nwiro dropped its record while the
		// adapter kept waiting on a request that would never be answered.
		Sinks.RespondToAcp(Request.AdapterId, Request.AcpReqId, EffectiveOptionId);
	}
	else if (!bProcessStillLive)
	{
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: permission '%s' settled as %s with no wire response — origin process gone"),
			*Request.ToolTitle, LexToString(EffectiveOutcome));
	}

	// One line per settlement, at Log.
	//
	// Every terminal outcome was previously silent unless something went wrong, so a
	// correct cancel-retirement and a plain user rejection produced identical logs — the
	// outcome was right and unattributable. (The old "retiring un-clicked" line was lost
	// when RetireAcpPermissionState was rewritten to delegate here.) This is the same
	// observability gap as the decision site, and the same cheap fix: say what happened.
	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK SETTLE: tool='%s' chat='%s' outcome=%s reason=%s card=%d wire=%s"),
		Request.ToolTitle.IsEmpty() ? TEXT("(untitled)") : *Request.ToolTitle,
		*Request.ChatId, LexToString(EffectiveOutcome), LexToString(EffectiveReason), LocalPermId,
		(bProcessStillLive && !EffectiveOptionId.IsEmpty())
			? *EffectiveOptionId : TEXT("(none)"));

	// UI event for every card, including the branches above that send nothing on the wire.
	// The renderer is a projection: it must never be left to infer that a card is dead.
	// LocalPermId 0 means no card was ever shown, so there is nothing to close.
	if (LocalPermId != 0 && Sinks.EmitUiEvent)
	{
		TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
		Payload->SetNumberField(TEXT("permissionId"), LocalPermId);
		Payload->SetStringField(TEXT("toolCallId"), Request.ToolCallId);
		Payload->SetStringField(TEXT("toolName"), Request.ToolTitle);
		Payload->SetStringField(TEXT("outcome"), LexToString(EffectiveOutcome));
		Payload->SetStringField(TEXT("reason"), LexToString(EffectiveReason));
		Payload->SetBoolField(TEXT("denied"), !IsPositiveAcpPermissionOutcome(EffectiveOutcome));

		// Condensed, matching UNwiroIKBridge::JsonToString — every other event on this
		// channel is condensed and the renderer parses them the same way.
		FString PayloadJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
		FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

		// Routed to the request's OWN chat, never ActiveChatId.
		Sinks.EmitUiEvent(TEXT("permission_settled"), PayloadJson, Request.ChatId);
	}
}
