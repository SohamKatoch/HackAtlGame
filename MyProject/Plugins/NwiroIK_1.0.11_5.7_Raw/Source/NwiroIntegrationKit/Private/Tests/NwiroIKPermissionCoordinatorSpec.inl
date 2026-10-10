// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroIKPermissionCoordinatorSpec.inl — the production-route permission tests.
//
// These drive the REAL FAcpPermissionCoordinator that NwiroIKBridge drives. The sinks and
// the clock are injected, so a test observes exactly the wire responses and UI events
// production emits; nothing here re-implements the decision.
//
// This distinction is the whole reason the file exists. The previous round of tests called
// extracted predicates and used local TMaps that imitated the coordinator. They were green
// while the wiring around them was wrong in four separate ways — mode read from a mutable
// global, retirement that dropped instead of settling, stored identity fields never
// validated, and a renderer acting as a second authority. Predicate coverage cannot see
// any of that.
//
// Included at FILE SCOPE from NwiroIKBridge.cpp under WITH_DEV_AUTOMATION_TESTS.

#include "Misc/AutomationTest.h"

// A world the coordinator can be pointed at: records what was emitted, and answers
// liveness questions from values the test controls.
struct FPermissionHarness
{
	struct FWireResponse { FString AdapterId; FString OptionId; };
	struct FUiEvent      { FString Type; FString Payload; FString ChatId; };

	TArray<FWireResponse> WireResponses;
	TArray<FUiEvent>      UiEvents;

	// The world. Mutate these between calls to simulate restarts, closes and cancels.
	TMap<FString, uint64> ProcessGenerations;   // adapterId  -> generation
	TSet<FString>         LiveSessions;         // acpSessionId
	TMap<FString, uint64> TurnGenerations;      // chatId     -> turn
	TMap<FString, FString> SessionOwners;       // acpSessionId -> owning chatId
	TSet<FString>         ProcessingChats;      // chats with a turn in flight
	double                Now = 1000.0;

	FAcpPermissionCoordinator Coordinator;

	FPermissionHarness()
	{
		FAcpPermissionSinks Sinks;
		Sinks.RespondToAcp = [this](const FString& AdapterId, const Acp::FRequestId&, const FString& OptionId)
			{ WireResponses.Add({ AdapterId, OptionId }); };
		Sinks.EmitUiEvent = [this](const FString& Type, const FString& Payload, const FString& ChatId)
			{ UiEvents.Add({ Type, Payload, ChatId }); };
		Sinks.NowSeconds = [this]() { return Now; };

		FAcpPermissionLiveContext Live;
		// Mirrors UNwiroIKBridge::IsPermissionOriginLive: the full ownership tuple,
		// answered as one predicate rather than three independent facts.
		Live.IsOriginLive = [this](const FAcpPermissionRequest& R)
			{
				const uint64* Gen = ProcessGenerations.Find(R.AdapterId);
				if (!Gen || *Gen == 0 || R.ProcessGeneration == 0 || *Gen != R.ProcessGeneration) return false;
				if (!LiveSessions.Contains(R.AcpSessionId)) return false;
				const FString* Owner = SessionOwners.Find(R.AcpSessionId);
				if (!Owner || *Owner != R.ChatId) return false;
				const uint64* Turn = TurnGenerations.Find(R.ChatId);
				if (!Turn || *Turn == 0 || R.TurnGeneration == 0 || *Turn != R.TurnGeneration) return false;
				return ProcessingChats.Contains(R.ChatId);
			};
		Live.IsOriginProcessLive = [this](const FAcpPermissionRequest& R)
			{
				const uint64* Gen = ProcessGenerations.Find(R.AdapterId);
				return Gen && *Gen != 0 && R.ProcessGeneration != 0 && *Gen == R.ProcessGeneration;
			};

		Coordinator.Configure(MoveTemp(Sinks), MoveTemp(Live));
	}

	// A live codex chat, ready to raise permissions.
	void GoLive(const FString& ChatId, const FString& SessionId, uint64 Generation = 7, uint64 Turn = 1)
	{
		ProcessGenerations.Add(TEXT("codex"), Generation);
		LiveSessions.Add(SessionId);
		SessionOwners.Add(SessionId, ChatId);
		TurnGenerations.Add(ChatId, Turn);
		ProcessingChats.Add(ChatId);
	}

	FAcpPermissionRequest MakeRequest(
		const FString& ChatId, const FString& SessionId, const FString& Title,
		const FString& Mode = TEXT("default"), uint64 Turn = 1, uint64 Generation = 7)
	{
		FAcpPermissionRequest Request;
		Request.AdapterId         = TEXT("codex");
		Request.ProcessGeneration = Generation;
		Request.AcpSessionId      = SessionId;
		Request.ChatId            = ChatId;
		Request.TurnGeneration    = Turn;
		Request.ModeSnapshot      = Mode;
		Request.ToolCallId        = TEXT("call_") + Title;
		Request.ToolTitle         = Title;
		Request.Options.Add({ TEXT("allow_once"),  TEXT("Allow"),  TEXT("allow_once") });
		Request.Options.Add({ TEXT("reject_once"), TEXT("Reject"), TEXT("reject_once") });
		return Request;
	}

	FString LastOption() const { return WireResponses.Num() ? WireResponses.Last().OptionId : FString(); }
	FString LastUiChat() const { return UiEvents.Num() ? UiEvents.Last().ChatId : FString(); }
	bool LastUiSaysDenied() const
	{
		if (UiEvents.Num() == 0) return false;
		return UiEvents.Last().Payload.Contains(TEXT("\"denied\":true"));
	}
	bool LastUiOutcomeIs(const TCHAR* Outcome) const
	{
		if (UiEvents.Num() == 0) return false;
		return UiEvents.Last().Payload.Contains(FString::Printf(TEXT("\"outcome\":\"%s\""), Outcome));
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKPermissionCoordinatorSpec,
	"Nwiro.IntegrationKit.Shell.PermissionCoordinator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKPermissionCoordinatorSpec::RunTest(const FString& Parameters)
{
	// ── 1. A correlated nwiro approval allows exactly once ───────────────────────
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("mcp.nwiro.inspect_material_graph")));

		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));

		TestEqual(TEXT("1: allow produces exactly one wire response"), H.WireResponses.Num(), 1);
		TestEqual(TEXT("1: allow sends the agent's own allow option"), H.LastOption(), FString(TEXT("allow_once")));
		TestTrue(TEXT("1: UI is told it was allowed"), H.LastUiOutcomeIs(TEXT("allowed")));
		TestEqual(TEXT("1: nothing is left pending"), H.Coordinator.NumPending(), 0);
	}

	// ── 2. Auto-allow is refused once its origin is gone ─────────────────────────
	// Real shell and third-party MCP never reach the coordinator as Allowed — the bridge
	// classifies them and settles ShellBlocked/Denied. What the coordinator must add is
	// that even a legitimate ALLOW is refused when the context that earned it is gone.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest Shell = H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("bash"));
		H.Coordinator.SettleImmediate(Shell, EAcpPermissionOutcome::ShellBlocked,
			EAcpPermissionReason::ShellPolicy, TEXT("reject_once"));

		TestEqual(TEXT("2: shell rejection answers on the wire"), H.WireResponses.Num(), 1);
		TestEqual(TEXT("2: shell rejection sends reject"), H.LastOption(), FString(TEXT("reject_once")));
		TestEqual(TEXT("2: a request that never became a card emits no card event"), H.UiEvents.Num(), 0);
	}

	// ── 3. Chat A (default) is isolated from chat B (bypassPermissions) ──────────
	// The mode travels with the request. A delayed request from chat A must be judged by
	// the snapshot it captured, no matter what chat B set the global to afterwards.
	{
		TestTrue(TEXT("3: chat A's own snapshot still shows a card"),
			DecidePermissionDisposition(false, TEXT("default")) == ENwiroPermissionDisposition::ShowCard);
		TestTrue(TEXT("3: chat B's bypass applies only to chat B's snapshot"),
			DecidePermissionDisposition(false, TEXT("bypassPermissions")) == ENwiroPermissionDisposition::AutoAllow);

		// And the snapshot is what the request carries, not a global read at settle time.
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest FromA = H.MakeRequest(
			TEXT("chat-A"), TEXT("sess-A"), TEXT("write_file"), TEXT("default"));
		TestEqual(TEXT("3: the request carries chat A's mode"), FromA.ModeSnapshot, FString(TEXT("default")));
	}

	// ── 4. A default-mode card routes to its exact background chat ───────────────
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-BG"), TEXT("sess-BG"));
		H.GoLive(TEXT("chat-FG"), TEXT("sess-FG"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-BG"), TEXT("sess-BG"), TEXT("find_assets")));
		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));

		TestEqual(TEXT("4: the outcome lands in the originating background chat"),
			H.LastUiChat(), FString(TEXT("chat-BG")));
	}

	// ── 5. Click and double-click produce exactly one response ───────────────────
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("spawn_actor")));

		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));
		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));   // the double-click
		H.Coordinator.SettleByClick(Card, TEXT("reject_once"));   // and a late reject

		TestEqual(TEXT("5: exactly one wire response survives three clicks"), H.WireResponses.Num(), 1);
		TestEqual(TEXT("5: exactly one UI outcome"), H.UiEvents.Num(), 1);
	}

	// ── 6. Cancel, close, timeout, restart and expiry leave zero pending ─────────
	{
		// (a) retirement by chat — cancel / close
		{
			FPermissionHarness H;
			H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
			H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("t1")));
			H.Coordinator.RetireMatching(FString(), FString(), TEXT("chat-A"), EAcpPermissionOutcome::Retired);

			TestEqual(TEXT("6a: nothing pending after the chat is retired"), H.Coordinator.NumPending(), 0);
			TestEqual(TEXT("6a: the agent is answered while the process is live"), H.WireResponses.Num(), 1);
			TestEqual(TEXT("6a: and it is answered with a REJECT, never an allow"),
				H.LastOption(), FString(TEXT("reject_once")));
			TestTrue(TEXT("6a: the UI is told the card is dead"), H.LastUiOutcomeIs(TEXT("retired")));
		}

		// (b) retirement by session — first-token timeout / unknown-session recovery
		{
			FPermissionHarness H;
			H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
			H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("t2")));
			H.Coordinator.RetireMatching(FString(), TEXT("sess-A"), FString(), EAcpPermissionOutcome::Retired);
			TestEqual(TEXT("6b: nothing pending after the session is retired"), H.Coordinator.NumPending(), 0);
			TestEqual(TEXT("6b: still exactly one terminal UI event"), H.UiEvents.Num(), 1);
		}

		// (c) adapter restart — the process that would answer is gone, so there is NO wire
		//     response, but the UI must still be closed out.
		{
			FPermissionHarness H;
			H.GoLive(TEXT("chat-A"), TEXT("sess-A"), /*Generation*/ 7);
			H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("t3")));
			H.ProcessGenerations.Add(TEXT("codex"), 8);   // replacement process
			H.Coordinator.RetireMatching(TEXT("codex"), FString(), FString(), EAcpPermissionOutcome::Retired);

			TestEqual(TEXT("6c: nothing pending after an adapter restart"), H.Coordinator.NumPending(), 0);
			TestEqual(TEXT("6c: no wire response to a replaced process"), H.WireResponses.Num(), 0);
			TestEqual(TEXT("6c: the UI is still closed out"), H.UiEvents.Num(), 1);
		}

		// (d) expiry — a card nobody ever answers
		{
			FPermissionHarness H;
			H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
			FAcpPermissionRequest Request = H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("t4"));
			Request.ExpiresAtSeconds = H.Now + 60.0;
			H.Coordinator.AddPending(Request);

			H.Now += 30.0;
			TestEqual(TEXT("6d: nothing expires before its deadline"), H.Coordinator.SettleExpired(), 0);
			TestEqual(TEXT("6d: and it is still pending"), H.Coordinator.NumPending(), 1);

			H.Now += 31.0;
			TestEqual(TEXT("6d: it expires once past the deadline"), H.Coordinator.SettleExpired(), 1);
			TestEqual(TEXT("6d: nothing pending afterwards"), H.Coordinator.NumPending(), 0);
			TestTrue(TEXT("6d: the UI is told it expired"), H.LastUiOutcomeIs(TEXT("expired")));
		}
	}

	// ── 7. Claude cancellation invalidates the turn despite preserving the session ─
	// Claude does NOT retire its ACP session on cancel, so session liveness alone still
	// says "live". Only the turn generation distinguishes the cancelled turn from the
	// next one — this is the case that motivated adding it.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"), /*Generation*/ 7, /*Turn*/ 4);
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("edit_blueprint"),
				TEXT("default"), /*Turn*/ 4));

		// Cancel: the session stays live (Claude), only the turn moves on.
		H.TurnGenerations.Add(TEXT("chat-A"), 5);
		TestTrue(TEXT("7: the session really is still live"), H.LiveSessions.Contains(TEXT("sess-A")));

		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));

		TestFalse(TEXT("7: a click from the cancelled turn is NOT granted"),
			H.LastOption() == TEXT("allow_once"));
		TestTrue(TEXT("7: it is downgraded to retired"), H.LastUiOutcomeIs(TEXT("retired")));
		TestTrue(TEXT("7: and the UI shows it as denied, not accepted"), H.LastUiSaysDenied());
	}

	// ── 8. A backend rejection cannot appear accepted in the UI ──────────────────
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("delete_file")));
		H.Coordinator.SettleByClick(Card, TEXT("reject_once"));

		TestTrue(TEXT("8: the UI event carries denied=true"), H.LastUiSaysDenied());
		TestTrue(TEXT("8: and names the outcome as denied"), H.LastUiOutcomeIs(TEXT("denied")));
		TestEqual(TEXT("8: the wire got the reject option"), H.LastOption(), FString(TEXT("reject_once")));
	}

	// ── 9. Supersession settles the older card instead of orphaning it ───────────
	// The renderer used to expire older cards locally, answering nobody — the agent was
	// left waiting on a request that had visually disappeared.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("first")));
		H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("second")));

		TestEqual(TEXT("9: only the newest card is pending"), H.Coordinator.NumPending(), 1);
		TestEqual(TEXT("9: the superseded one was answered on the wire"), H.WireResponses.Num(), 1);
		TestTrue(TEXT("9: and reported to the UI as superseded"), H.LastUiOutcomeIs(TEXT("superseded")));

		// A card in ANOTHER chat is untouched by this chat's supersession.
		H.GoLive(TEXT("chat-B"), TEXT("sess-B"));
		H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-B"), TEXT("sess-B"), TEXT("other")));
		TestEqual(TEXT("9: supersession is per chat"), H.Coordinator.NumPending(), 2);
	}

	// ── 10. Missing / malformed options fail closed ──────────────────────────────
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest NoOptions = H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("weird"));
		NoOptions.Options.Empty();
		const int32 Card = H.Coordinator.AddPending(NoOptions);
		H.Coordinator.SettleByClick(Card, FString());

		// The agent IS answered, with an EMPTY option id — the bridge turns that into an
		// ACP `cancelled` outcome, which needs no vocabulary. Sending nothing (the old
		// behaviour) was authorization-safe but left the adapter blocked forever on a
		// request we had already discarded.
		TestEqual(TEXT("10: the agent is still answered"), H.WireResponses.Num(), 1);
		TestEqual(TEXT("10: with no invented option id (bridge sends ACP cancelled)"),
			H.LastOption(), FString());
		TestEqual(TEXT("10: the card is still closed out"), H.Coordinator.NumPending(), 0);
		TestEqual(TEXT("10: and the UI is still told"), H.UiEvents.Num(), 1);

		// An agent offering only a reject option must never have an allow synthesised.
		FPermissionHarness H2;
		H2.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest RejectOnly = H2.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("rejectonly"));
		RejectOnly.Options.Empty();
		RejectOnly.Options.Add({ TEXT("reject_once"), TEXT("Reject"), TEXT("reject_once") });
		const int32 Card2 = H2.Coordinator.AddPending(RejectOnly);
		H2.Coordinator.SettleByClick(Card2, FString());
		TestFalse(TEXT("10: a reject option is never sent as an allow"),
			H2.LastOption() == TEXT("allow_once"));

		TestEqual(TEXT("10: FindOptionByKind returns empty rather than guessing"),
			FAcpPermissionCoordinator::FindOptionByKind({}, TEXT("allow")), FString());
	}

	// ── Settling an unknown id is a no-op, not a crash ───────────────────────────
	{
		FPermissionHarness H;
		H.Coordinator.SettleByClick(-999, TEXT("allow_once"));
		TestEqual(TEXT("an unknown permission id emits nothing"), H.WireResponses.Num(), 0);
		TestEqual(TEXT("an unknown permission id emits no UI event"), H.UiEvents.Num(), 0);
	}

	// ── 11. Unknown / unofferered option ids fail CLOSED ─────────────────────────
	// The click's meaning is derived by the coordinator from the vocabulary it captured.
	// Previously the bridge computed it and defaulted to false (= allow), so an option id
	// that was never offered authorized the call.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("execute_python")));

		H.Coordinator.SettleByClick(Card, TEXT("totally_made_up_option"));

		TestTrue(TEXT("11: an unoffered option id is denied, not allowed"), H.LastUiSaysDenied());
		TestTrue(TEXT("11: and reported as denied"), H.LastUiOutcomeIs(TEXT("denied")));
		TestEqual(TEXT("11: the wire gets the agent's reject option"),
			H.LastOption(), FString(TEXT("reject_once")));
	}

	// An OFFERED option whose kind we cannot classify is also not an approval.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest Odd = H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("odd"));
		Odd.Options.Add({ TEXT("maybe"), TEXT("Maybe"), TEXT("something_unrecognised") });
		const int32 Card = H.Coordinator.AddPending(Odd);
		H.Coordinator.SettleByClick(Card, TEXT("maybe"));
		TestTrue(TEXT("11: an unclassifiable option kind is denied"), H.LastUiSaysDenied());
	}

	// ── 12. Option preference: a one-shot grant is never upgraded to a standing one ─
	{
		FPermissionHarness H;
		TArray<FAcpPermissionOption> Options;
		// Deliberately listing the persistent option FIRST — plain Contains() matching
		// picked it purely on array order.
		Options.Add({ TEXT("allow_always"), TEXT("Always allow"), TEXT("allow_always") });
		Options.Add({ TEXT("allow_once"),   TEXT("Allow once"),   TEXT("allow_once") });
		Options.Add({ TEXT("reject_once"),  TEXT("Reject"),       TEXT("reject_once") });

		TestEqual(TEXT("12: allow resolves to the once option, not always"),
			FAcpPermissionCoordinator::FindOptionByKind(Options, TEXT("allow")),
			FString(TEXT("allow_once")));

		// A persistent option is a DIFFERENT decision, not a fallback. An automatic path
		// must never select allow_always: that grants the agent a standing permission the
		// user never chose, which keeps applying after the mode returns to default.
		TArray<FAcpPermissionOption> OnlyAlways;
		OnlyAlways.Add({ TEXT("allow_always"), TEXT("Always"), TEXT("allow_always") });
		TestEqual(TEXT("12: an automatic path refuses to select a persistent allow"),
			FAcpPermissionCoordinator::FindOptionByKind(OnlyAlways, TEXT("allow")),
			FString());
		TestEqual(TEXT("12: it is reachable only when persistent is explicitly permitted"),
			FAcpPermissionCoordinator::FindOptionByKind(OnlyAlways, TEXT("allow"), /*bAllowPersistent*/ true),
			FString(TEXT("allow_always")));

		// Kinds are an EXACT enum, not free text. Substring matching read `not_allow_once`
		// as an approval.
		TestEqual(TEXT("12: a kind merely CONTAINING allow is Unknown, not Allow"),
			static_cast<int32>(FAcpPermissionCoordinator::ClassifyOption({ TEXT("x"), TEXT("x"), TEXT("not_allow_once") })),
			static_cast<int32>(EAcpOptionSemantics::Unknown));
		TestEqual(TEXT("12: reject_always classifies as Deny"),
			static_cast<int32>(FAcpPermissionCoordinator::ClassifyOption({ TEXT("x"), TEXT("x"), TEXT("reject_always") })),
			static_cast<int32>(EAcpOptionSemantics::Deny));

		TestEqual(TEXT("12: semantics are derived, not guessed from the id"),
			static_cast<int32>(FAcpPermissionCoordinator::ClassifyOption({ TEXT("x"), TEXT("x"), TEXT("reject_once") })),
			static_cast<int32>(EAcpOptionSemantics::Deny));
		TestEqual(TEXT("12: an unreadable kind classifies as Unknown, not Allow"),
			static_cast<int32>(FAcpPermissionCoordinator::ClassifyOption({ TEXT("x"), TEXT("x"), TEXT("mystery") })),
			static_cast<int32>(EAcpOptionSemantics::Unknown));
	}

	// ── 13. An allow that cannot be expressed is reported as denied ──────────────
	// Emitting outcome "Allowed" with no wire response would show the user an approval
	// that the agent never received.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		FAcpPermissionRequest RejectOnly = H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("x"));
		RejectOnly.Options.Empty();
		RejectOnly.Options.Add({ TEXT("reject_once"), TEXT("Reject"), TEXT("reject_once") });
		H.Coordinator.SettleImmediate(RejectOnly, EAcpPermissionOutcome::Allowed,
			EAcpPermissionReason::ModeAutoApprove, FString());

		TestFalse(TEXT("13: no allow was sent"), H.LastOption() == TEXT("allow_once"));
		TestEqual(TEXT("13: the reject option was sent instead"),
			H.LastOption(), FString(TEXT("reject_once")));
	}

	// ── 14. Retirement scope must be explicit ───────────────────────────────────
	// An all-empty RetireMatching is a caller that lost its scope, not a request to
	// retire everything. CloseChat on a session-less chat did exactly this and wiped
	// every other chat's cards.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		H.GoLive(TEXT("chat-B"), TEXT("sess-B"));
		H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("a")));
		H.Coordinator.AddPending(H.MakeRequest(TEXT("chat-B"), TEXT("sess-B"), TEXT("b")));

		const int32 Retired = H.Coordinator.RetireMatching(
			FString(), FString(), FString(), EAcpPermissionOutcome::Retired);
		TestEqual(TEXT("14: an unscoped retire matches nothing"), Retired, 0);
		TestEqual(TEXT("14: both chats keep their cards"), H.Coordinator.NumPending(), 2);

		// Scoped retirement touches only its own chat.
		H.Coordinator.RetireMatching(FString(), FString(), TEXT("chat-A"), EAcpPermissionOutcome::Retired);
		TestEqual(TEXT("14: only chat A's card was retired"), H.Coordinator.NumPending(), 1);

		// And RetireAll says so out loud.
		TestEqual(TEXT("14: RetireAll clears the rest"),
			H.Coordinator.RetireAll(EAcpPermissionOutcome::Retired), 1);
		TestEqual(TEXT("14: nothing pending after RetireAll"), H.Coordinator.NumPending(), 0);
	}

	// ── 15. Ownership is a relationship, not three separate facts ───────────────
	{
		// A session that is live, at the right generation and turn — but owned by a
		// DIFFERENT chat. Each individual fact checks out; the ownership does not.
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		H.GoLive(TEXT("chat-B"), TEXT("sess-B"));
		FAcpPermissionRequest Crossed = H.MakeRequest(TEXT("chat-B"), TEXT("sess-A"), TEXT("crossed"));
		const int32 Card = H.Coordinator.AddPending(Crossed);
		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));
		TestTrue(TEXT("15: a session owned by another chat cannot authorize"),
			H.LastUiOutcomeIs(TEXT("retired")));
	}
	{
		// The turn's generation still matches, but the turn is no longer processing.
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"));
		const int32 Card = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("finished")));
		H.ProcessingChats.Remove(TEXT("chat-A"));
		H.Coordinator.SettleByClick(Card, TEXT("allow_once"));
		TestTrue(TEXT("15: a turn that already ended cannot authorize"),
			H.LastUiOutcomeIs(TEXT("retired")));
	}

	// ── 16. Retirement can be scoped to ONE TURN ────────────────────────────────
	// For a caller whose decision and its arrival are separated in time — the stall
	// watchdog bounds its bridge call at 5s and unlocks the composer on timeout, so a new
	// turn can begin before the cancel lands. Such a caller may retire only the cards of
	// the turn it decided about.
	//
	// Two turns of the same chat cannot both be pending (AddPending supersedes per chat),
	// so the filter is exercised here against two chats' cards — the level at which it is
	// actually reachable — rather than through the bridge.
	{
		FPermissionHarness H;
		H.GoLive(TEXT("chat-A"), TEXT("sess-A"), 7, /*Turn*/ 3);
		H.GoLive(TEXT("chat-B"), TEXT("sess-B"), 7, /*Turn*/ 9);
		const int32 CardA = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-A"), TEXT("sess-A"), TEXT("older"), TEXT("default"), /*Turn*/ 3));
		const int32 CardB = H.Coordinator.AddPending(
			H.MakeRequest(TEXT("chat-B"), TEXT("sess-B"), TEXT("newer"), TEXT("default"), /*Turn*/ 9));

		// A turn nobody has: matches on adapter, but on no card's turn.
		TestEqual(TEXT("16: a generation that matches no card retires nothing"),
			H.Coordinator.RetireMatching(TEXT("codex"), FString(), FString(),
				EAcpPermissionOutcome::Retired, EAcpPermissionReason::ContextRetired, /*Turn*/ 5), 0);
		TestTrue(TEXT("16: both cards survive an unmatched generation"),
			H.Coordinator.HasPending(CardA) && H.Coordinator.HasPending(CardB));

		TestEqual(TEXT("16: only the named turn's card is retired"),
			H.Coordinator.RetireMatching(TEXT("codex"), FString(), FString(),
				EAcpPermissionOutcome::Retired, EAcpPermissionReason::ContextRetired, /*Turn*/ 3), 1);
		TestFalse(TEXT("16: the matching turn's card is gone"), H.Coordinator.HasPending(CardA));
		TestTrue(TEXT("16: the other turn's card is untouched"), H.Coordinator.HasPending(CardB));

		// 0 keeps the old meaning — "any turn" — for callers acting now (Stop, close).
		TestEqual(TEXT("16: generation 0 still means any turn"),
			H.Coordinator.RetireMatching(TEXT("codex"), FString(), FString(),
				EAcpPermissionOutcome::Retired), 1);
		TestEqual(TEXT("16: nothing left pending"), H.Coordinator.NumPending(), 0);
	}

	// ── 17. The option vocabulary stays an EXACT, fail-closed enum ─────────────
	// A normalisation pass was proposed here and REJECTED. ClassifyOption and
	// IsPersistentOption both read Option.Kind, and only one of them would have been
	// normalised — so a malformed ` allow_always ` would classify as Allow while
	// IsPersistentOption reported false, letting an AUTOMATIC path select a standing grant
	// the user never chose (see FindOptionByKind's bAllowPersistent gate). Two predicates
	// over the same field must agree; changing one is how authorization bugs are made.
	//
	// A genuine future vocabulary change gets an explicit, tested alias for that adapter and
	// version — not a blanket loosening.
	{
		auto Classify = [](const TCHAR* Kind)
		{
			FAcpPermissionOption Option;
			Option.Kind = Kind;
			return FAcpPermissionCoordinator::ClassifyOption(Option);
		};
		TestTrue(TEXT("17: canonical spellings classify"),
			Classify(TEXT("allow_once")) == EAcpOptionSemantics::Allow
			&& Classify(TEXT("reject_once")) == EAcpOptionSemantics::Deny);
		TestTrue(TEXT("17: casing is tolerated (already, via IgnoreCase)"),
			Classify(TEXT("Allow_Once")) == EAcpOptionSemantics::Allow);
		TestTrue(TEXT("17: not_allow_once is NOT an approval"),
			Classify(TEXT("not_allow_once")) == EAcpOptionSemantics::Unknown);
		TestTrue(TEXT("17: bare 'allow' is unknown — that is an option ID, not a kind"),
			Classify(TEXT("allow")) == EAcpOptionSemantics::Unknown);
		TestTrue(TEXT("17: padded kinds are NOT silently accepted"),
			Classify(TEXT(" allow_always ")) == EAcpOptionSemantics::Unknown);
		// The paired predicate must agree with the classifier on the same input.
		FAcpPermissionOption Padded; Padded.Kind = TEXT(" allow_always ");
		TestFalse(TEXT("17: IsPersistentOption agrees — padded is not a recognised persistent kind"),
			FAcpPermissionCoordinator::IsPersistentOption(Padded));
	}

	return true;
}
