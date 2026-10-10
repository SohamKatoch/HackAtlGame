// Copyright 2026 Nwiro. All Rights Reserved.
//
// FAcpPermissionCoordinator — the single owner of an ACP permission request's lifetime.
//
// WHY THIS EXISTS
//
// A permission request is an asynchronous request with exactly one terminal outcome, and
// before this class there was no single place that guaranteed it. The bridge minted a
// card, stored two loose maps, and every terminal path did its own thing: the click path
// responded, retirement merely called Remove (leaving the agent waiting forever and the
// card visible), supersession happened in the renderer with no wire response at all, and
// nothing expired. Three different code paths could each believe they owned the same
// request.
//
// It also exists so the decision can be TESTED on the production route. The predicates it
// replaced were unit-tested with local maps that imitated the bridge; those tests passed
// while the wiring around them was wrong. The bridge and the tests now call this same
// object, with the side effects injected:
//
//   * RespondToAcp  — what goes back on the wire
//   * EmitUiEvent   — what the renderer is told
//   * NowSeconds    — the clock (a test supplies a fake one)
//
// A test therefore asserts on exactly what production emits, not on a re-implementation.
//
// INVARIANTS
//
//   1. Every request reaches exactly ONE terminal outcome. Settle() removes the record
//      BEFORE emitting anything, so a second attempt finds nothing and does nothing.
//   2. Authority is bound to the IMMUTABLE origin context — adapter, process generation,
//      ACP session, chat, turn generation and the permission mode as it was when the
//      request arrived. Nothing is read from mutable global state at settle time.
//   3. A positive outcome (allow) additionally requires that context to still be live.
//      A negative outcome never does: denying is always safe, and a retired request must
//      still be denied on the wire if anyone is listening.
//   4. Every terminal outcome emits a UI event, including the ones with no wire response.
//      The renderer is a projection; it must never have to infer that a card is dead.

#pragma once

#include "CoreMinimal.h"
#include "NwiroIKAcpTypes.h"

// How a request ended. Distinct values rather than a bool because the wire response and
// the UI copy differ, and because "why did this card vanish" is the question a support
// report actually asks.
enum class EAcpPermissionOutcome : uint8
{
	Allowed,      // user (or an auto-approve mode) approved it
	Denied,       // user (or an auto-deny mode) rejected it
	ShellBlocked, // refused by shell classification — never reaches the user
	Superseded,   // a newer request replaced it
	Retired,      // its chat/session/adapter went away
	Expired,      // deadline passed with no answer
};

// WHY the outcome was what it was. Distinct from the outcome because several very
// different causes collapse to the same one — `Denied` is produced by a user pressing
// Decline, by an option id that was never offered, and by an option whose kind we cannot
// classify. Those are indistinguishable in a log that records only the outcome, and the
// difference is exactly what a support report needs: "the user said no" versus "the
// adapter changed its vocabulary and we failed closed".
enum class EAcpPermissionReason : uint8
{
	UserChoice,        // a click on an option we understood
	OptionNotOffered,  // clicked id absent from the captured vocabulary
	UnknownOptionKind, // offered, but its kind is not in the ACP enum
	NoAllowOption,     // wanted to allow, agent offered nothing we could send
	ModeAutoApprove,   // bypassPermissions / acceptEdits
	ModeAutoDeny,      // plan / dontAsk
	ShellPolicy,       // shell classification refused it
	OriginRetired,     // positive outcome refused: origin context no longer live
	Deadline,          // TTL elapsed with no answer
	ContextRetired,    // chat/session/adapter went away
	Superseded,        // replaced by a newer request in the same chat
};

const TCHAR* LexToString(EAcpPermissionOutcome Outcome);
const TCHAR* LexToString(EAcpPermissionReason Reason);

// True when this outcome grants the agent something. Used to decide whether liveness must
// be revalidated before responding.
bool IsPositiveAcpPermissionOutcome(EAcpPermissionOutcome Outcome);

// One option as offered by the agent.
struct FAcpPermissionOption
{
	FString OptionId;
	FString Name;
	FString Kind;
};

// What an option MEANS, derived by the coordinator from the agent's own kind string.
// Unknown is a real answer, not a synonym for allow.
enum class EAcpOptionSemantics : uint8
{
	Allow,
	Deny,
	Unknown,
};

// The immutable facts about where a permission request came from. Captured once, never
// refreshed — that is the entire point. Anything read later from bridge state can have
// changed underneath the request.
struct FAcpPermissionRequest
{
	Acp::FRequestId AcpReqId;
	FString AdapterId;
	uint64  ProcessGeneration = 0;
	FString AcpSessionId;
	FString ChatId;
	// Monotonic per chat, bumped on every prompt send AND every cancel/close. Session
	// identity is too broad: cancelling a Claude turn can KEEP the ACP session, so a card
	// from the cancelled turn would otherwise still validate during the next turn.
	uint64  TurnGeneration = 0;
	// The mode as it was for THIS chat when the request arrived. Not the bridge-global
	// CurrentMode, which the frontend overwrites before every send — a delayed request
	// from a background chat would otherwise be judged under a foreground chat's mode.
	FString ModeSnapshot;
	FString ToolCallId;
	FString ToolTitle;
	// Absolute time from the injected clock. 0 disables expiry for this request.
	double  ExpiresAtSeconds = 0.0;
	TArray<FAcpPermissionOption> Options;
};

// What the coordinator needs to know about the world RIGHT NOW in order to revalidate.
// Injected as callbacks so a test can present any world it likes without a bridge.
//
// ONE predicate for ownership, not three independent facts. The previous shape asked
// "is this generation current", "does this session exist" and "is this chat's turn N"
// separately — all three could be true of a request whose session belongs to a DIFFERENT
// chat, or whose chat has since moved to another adapter. Only the bridge can answer the
// relationship atomically, so it does.
struct FAcpPermissionLiveContext
{
	// Full ownership: the process is running at the captured generation, the session maps
	// to exactly this chat, the chat is on this adapter, and the captured turn is still
	// the active, processing turn. Gates every POSITIVE outcome.
	TFunction<bool(const FAcpPermissionRequest&)> IsOriginLive;
	// Narrower: is the process that issued this request still the live one. Gates whether
	// a wire response is sent at all — a denial should still reach a live agent even when
	// the turn has moved on, but must never reach a REPLACEMENT process, whose ACP id
	// space overlaps.
	TFunction<bool(const FAcpPermissionRequest&)> IsOriginProcessLive;
};

// Everything the coordinator does to the outside world.
struct FAcpPermissionSinks
{
	TFunction<void(const FString& AdapterId, const Acp::FRequestId&, const FString& OptionId)> RespondToAcp;
	TFunction<void(const FString& EventType, const FString& PayloadJson, const FString& ChatId)> EmitUiEvent;
	TFunction<double()> NowSeconds;
};

class FAcpPermissionCoordinator
{
public:
	void Configure(FAcpPermissionSinks InSinks, FAcpPermissionLiveContext InLive);

	// Register a request that is about to be shown to the user. Returns the local id the
	// renderer will echo back on click. Supersedes any older request for the same chat:
	// the previous card is settled as Superseded, which both answers the agent and tells
	// the renderer the old card is dead — the renderer used to do this locally and answer
	// nobody.
	int32 AddPending(const FAcpPermissionRequest& Request);

	// The user clicked. The coordinator derives the meaning itself from the vocabulary it
	// captured with the request — the caller passes only the raw id.
	//
	// An id that is not in that vocabulary FAILS CLOSED. It previously arrived as a
	// caller-computed `bIsDeny` that defaulted to false, so an unknown or malformed
	// option id was treated as an approval.
	void SettleByClick(int32 LocalPermId, const FString& OptionId);

	static EAcpOptionSemantics ClassifyOption(const FAcpPermissionOption& Option);

	// Terminal settlement for everything that is not a click. Matching is by adapter
	// and/or session and/or chat; empty means "do not filter on this".
	//
	// All-empty is REFUSED, not treated as "match everything". Closing a chat that never
	// got a session used to pass empty adapter AND empty session, which silently retired
	// every other chat's cards. A caller that really means everything says so via
	// RetireAll, where the intent is visible at the call site.
	//
	// TurnGeneration 0 means "any turn" and is right for callers acting NOW (Stop, close).
	// A caller whose decision was made earlier and may arrive late — the stall watchdog —
	// passes the generation it decided about, so it cannot retire a turn that started since.
	int32 RetireMatching(
		const FString& AdapterId, const FString& AcpSessionId, const FString& ChatId,
		EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason = EAcpPermissionReason::ContextRetired,
		uint64 TurnGeneration = 0);

	// Explicit global retirement — bridge shutdown, adapter teardown, process kill.
	int32 RetireAll(EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason = EAcpPermissionReason::ContextRetired);

	// Settle everything whose deadline has passed. Call from the existing ticker.
	int32 SettleExpired();

	// Immediate settlement for a request that never becomes a card (auto-allow, auto-deny,
	// shell rejection). Routed through the same function so those paths also emit a UI
	// event and are subject to the same liveness rule.
	void SettleImmediate(const FAcpPermissionRequest& Request, EAcpPermissionOutcome Outcome,
		EAcpPermissionReason Reason, const FString& OptionId);

	int32 NumPending() const { return Pending.Num(); }
	bool  HasPending(int32 LocalPermId) const { return Pending.Contains(LocalPermId); }
	const FAcpPermissionRequest* Find(int32 LocalPermId) const { return Pending.Find(LocalPermId); }

	// Exposed for the bridge's own diagnostics and for tests asserting on chat routing.
	static bool IsOriginStillLive(const FAcpPermissionRequest& Request, const FAcpPermissionLiveContext& Live);

	// Pick the option matching a kind, e.g. "allow" / "reject". Empty when the agent
	// offered nothing usable — callers MUST treat that as fail-closed rather than
	// inventing an option id.
	//
	// bAllowPersistent gates the *_always kinds. Leave it false on any AUTOMATIC path:
	// granting allow_always creates a standing agent-side permission the user never
	// chose, which would keep applying after the mode is switched back to default.
	static FString FindOptionByKind(
		const TArray<FAcpPermissionOption>& Options, const FString& KindMatch,
		bool bAllowPersistent = false);

	static bool IsPersistentOption(const FAcpPermissionOption& Option);

private:
	// The one terminal path. Removes first, then emits at most one wire response and
	// exactly one UI event.
	void SettleInternal(int32 LocalPermId, EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason, const FString& OptionId);
	// LocalPermId 0 means "this request never became a card" (auto-allow, auto-deny,
	// shell rejection). Those still get the wire response and the liveness rule, but no
	// card-settled event — there is no card for the renderer to close.
	void Emit(int32 LocalPermId, const FAcpPermissionRequest& Request,
		EAcpPermissionOutcome Outcome, EAcpPermissionReason Reason, const FString& OptionId);

	FAcpPermissionSinks Sinks;
	FAcpPermissionLiveContext Live;
	TMap<int32, FAcpPermissionRequest> Pending;
	// Negative range, matching the bridge's existing convention: ACP local ids must stay
	// disjoint from the MCP server's >= 1000 range.
	int32 NextLocalPermId = -1;
};
