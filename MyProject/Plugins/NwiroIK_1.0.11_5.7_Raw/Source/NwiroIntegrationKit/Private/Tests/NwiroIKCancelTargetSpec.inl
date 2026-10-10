// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroIKCancelTargetSpec.inl — the Stop button must end the turn in the chat the user is
// looking at, and only that one.
//
// Origin: a user report that pressing Stop did nothing while a DIFFERENT chat quietly died.
// CancelMessage() resolved its target from ActiveChatId, which is not "the chat on screen"
// — SetActiveChat is called from sendViaBridge and nowhere else, so it tracks the last chat
// that SENT. Send in A, send in B, switch back to A, press Stop: B was cancelled. The
// renderer had already recorded its user-cancel token against A, so B's "Cancelled." echo
// arrived unpaired and was swallowed, and A kept streaming with no working Stop.
//
// This is the same root cause as the permission bugs in this campaign: a piece of display
// or routing state (ActiveChatId) read as if it were the authoritative target of an action.
//
// Included at FILE SCOPE from NwiroIKBridge.cpp under WITH_DEV_AUTOMATION_TESTS.

#include "Misc/AutomationTest.h"

// ChatSessions and ActiveChatId are private, and no public method reports another chat's
// state (IsProcessing answers for the active chat only), so the interesting assertion is
// unreachable from outside. The class grants this struct friendship in dev builds.
struct FNwiroIKCancelTargetProbe
{
	// A real session id, because the ACP `session/cancel` frame is built from it. Seeding
	// chats WITHOUT one made SendRpcNotification unreachable, so deleting the notification
	// outright would have left the spec green — it proved bookkeeping and nothing else.
	static void Seed(UNwiroIKBridge& Bridge, const FString& ChatId, const FString& SessionId)
	{
		FChatSession& S = Bridge.ChatSessions.FindOrAdd(ChatId);
		S.ChatId = ChatId;
		S.AdapterId = TEXT("claude"); // never retires its session on cancel — keeps this deterministic
		S.SessionId = SessionId;
		S.bProcessing = true;
		S.TurnGeneration = 7;
		Bridge.SessionToChatId.Add(SessionId, ChatId);
	}
	static void SetActive(UNwiroIKBridge& Bridge, const FString& ChatId) { Bridge.ActiveChatId = ChatId; }
	static const FChatSession* Get(UNwiroIKBridge& Bridge, const FString& ChatId)
	{
		return Bridge.ChatSessions.Find(ChatId);
	}
	static FAcpPermissionCoordinator& Coordinator(UNwiroIKBridge& Bridge)
	{
		return Bridge.PermissionCoordinator;
	}
	static const TArray<FString>* Staged(UNwiroIKBridge& Bridge, const FString& ChatId)
	{
		return Bridge.PendingImagesByChat.Find(ChatId);
	}
	static void TapOutbound(UNwiroIKBridge& Bridge, TArray<FString>& Sink)
	{
		Bridge.TestOutboundTap = [&Sink](const FString&, const FString& Json) { Sink.Add(Json); };
	}
	// What a new prompt does to the turn counter, without dragging SendMessage's adapter
	// launch into a test that is only about the staleness refusal.
	static void AdvanceTurn(UNwiroIKBridge& Bridge, const FString& ChatId)
	{
		if (FChatSession* S = Bridge.ChatSessions.Find(ChatId)) { ++S->TurnGeneration; S->bProcessing = true; }
	}
};

// A permission card belonging to one chat, with the option vocabulary the coordinator
// needs. Nothing here is about cancel targeting except the ChatId — that is the field
// RetireMatching filters on.
static FAcpPermissionRequest MakeCancelTargetRequest(
	const FString& ChatId, const FString& Title, uint64 Turn = 7)
{
	FAcpPermissionRequest Request;
	Request.AdapterId         = TEXT("claude");
	Request.ProcessGeneration = 7;
	Request.AcpSessionId      = TEXT("sess-") + ChatId;
	Request.ChatId            = ChatId;
	Request.TurnGeneration    = Turn;
	Request.ModeSnapshot      = TEXT("default");
	Request.ToolCallId        = TEXT("call_") + Title;
	Request.ToolTitle         = Title;
	Request.Options.Add({ TEXT("allow_once"),  TEXT("Allow"),  TEXT("allow_once") });
	Request.Options.Add({ TEXT("reject_once"), TEXT("Reject"), TEXT("reject_once") });
	return Request;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCancelTargetTest,
	"Nwiro.IntegrationKit.Chat.CancelTarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCancelTargetTest::RunTest(const FString& Parameters)
{
	UNwiroIKBridge* Bridge = NewObject<UNwiroIKBridge>();
	Bridge->AddToRoot();

	// No adapter process, so SendRaw drops the frame and PushEvent finds no browser widget.
	// The tap records what the bridge built regardless — that is the point of it.
	TArray<FString> Frames;
	FNwiroIKCancelTargetProbe::TapOutbound(*Bridge, Frames);

	FNwiroIKCancelTargetProbe::Seed(*Bridge, TEXT("chat-a"), TEXT("sess-alpha"));
	FNwiroIKCancelTargetProbe::Seed(*Bridge, TEXT("chat-b"), TEXT("sess-beta"));

	// Cancelling also retires that chat's open permission cards, and the retirement is
	// filtered by chat id — the SECOND place the target has to be right. Counting sinks so
	// the agent-facing response can be counted; the world always reports live so the
	// retirement is never skipped for an unrelated reason.
	int32 WireResponses = 0;
	{
		FAcpPermissionSinks Sinks;
		Sinks.RespondToAcp = [&WireResponses](const FString&, const Acp::FRequestId&, const FString&)
			{ ++WireResponses; };
		Sinks.EmitUiEvent = [](const FString&, const FString&, const FString&) {};
		Sinks.NowSeconds = []() { return 1000.0; };

		FAcpPermissionLiveContext Live;
		Live.IsOriginLive = [](const FAcpPermissionRequest&) { return true; };
		Live.IsOriginProcessLive = [](const FAcpPermissionRequest&) { return true; };

		FNwiroIKCancelTargetProbe::Coordinator(*Bridge).Configure(MoveTemp(Sinks), MoveTemp(Live));
	}
	const int32 CardA = FNwiroIKCancelTargetProbe::Coordinator(*Bridge)
		.AddPending(MakeCancelTargetRequest(TEXT("chat-a"), TEXT("edit_blueprint")));
	const int32 CardB = FNwiroIKCancelTargetProbe::Coordinator(*Bridge)
		.AddPending(MakeCancelTargetRequest(TEXT("chat-b"), TEXT("spawn_actor")));

	// The user sent in A, then in B, then switched back to A. The plugin still calls B active.
	FNwiroIKCancelTargetProbe::SetActive(*Bridge, TEXT("chat-b"));

	// --- Stop, pressed while looking at chat A ------------------------------------
	Bridge->CancelChatTurn(TEXT("chat-a"));
	{
		const FChatSession* A = FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-a"));
		const FChatSession* B = FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-b"));
		TestNotNull(TEXT("chat-a still exists"), A);
		TestNotNull(TEXT("chat-b still exists"), B);
		if (!A || !B) { Bridge->RemoveFromRoot(); return false; }

		TestFalse(TEXT("the NAMED chat stops processing"), A->bProcessing);
		TestEqual(TEXT("the NAMED chat's turn generation advances"), (int64)A->TurnGeneration, (int64)8);

		// The half that was broken: a cancel aimed at A used to land here instead.
		TestTrue(TEXT("the last-active chat keeps running"), B->bProcessing);
		TestEqual(TEXT("the last-active chat's turn is untouched"), (int64)B->TurnGeneration, (int64)7);

		FAcpPermissionCoordinator& Coord = FNwiroIKCancelTargetProbe::Coordinator(*Bridge);
		TestFalse(TEXT("the NAMED chat's permission card is retired"), Coord.HasPending(CardA));
		TestTrue(TEXT("the other chat's card survives"), Coord.HasPending(CardB));
		TestEqual(TEXT("exactly one agent got answered"), WireResponses, 1);

		// THE WIRE. Everything above is local bookkeeping; this is the notification that
		// actually stops the agent, and the session id in it is the whole user complaint.
		const FString* CancelFrame = Frames.FindByPredicate([](const FString& F)
			{ return F.Contains(TEXT("session/cancel")); });
		TestNotNull(TEXT("a session/cancel notification was sent"), CancelFrame);
		if (CancelFrame)
		{
			TestTrue(TEXT("session/cancel names the NAMED chat's session"),
				CancelFrame->Contains(TEXT("\"sessionId\":\"sess-alpha\"")));
			TestFalse(TEXT("session/cancel does NOT name the last-active chat's session"),
				CancelFrame->Contains(TEXT("sess-beta")));
		}
	}

	// --- The no-arg entry point keeps its old meaning ------------------------------
	// Older renderers still call it, so it must go on targeting ActiveChatId (chat-b).
	Bridge->CancelMessage();
	{
		const FChatSession* B = FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-b"));
		TestNotNull(TEXT("chat-b still exists"), B);
		if (!B) { Bridge->RemoveFromRoot(); return false; }
		TestFalse(TEXT("CancelMessage still cancels ActiveChatId"), B->bProcessing);
		TestEqual(TEXT("CancelMessage still advances that chat's turn"), (int64)B->TurnGeneration, (int64)8);
	}

	// --- Cancelling a chat that has no session record is a no-op, not a crash ------
	Bridge->CancelChatTurn(TEXT("chat-never-existed"));
	TestNull(TEXT("no phantom session is created by cancelling an unknown chat"),
		FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-never-existed")));

	Bridge->RemoveFromRoot();
	return true;
}

// ─── Native image staging ────────────────────────────────────────────────────
// Removing ONE chip in the composer must un-stage ONE file. `TArray::Remove` deletes every
// element equal to the value, so picking the same file twice and deleting one chip silently
// un-staged both — an attachment the user never removed vanishing from the prompt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKStagedImageRemovalTest,
	"Nwiro.IntegrationKit.Chat.StagedImageRemoval",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKStagedImageRemovalTest::RunTest(const FString& Parameters)
{
	UNwiroIKBridge* Bridge = NewObject<UNwiroIKBridge>();
	Bridge->AddToRoot();

	const FString Chat = TEXT("chat-a");
	const FString Dup  = TEXT("C:/tmp/same.png");
	Bridge->AddImagePathForChat(Chat, Dup);
	Bridge->AddImagePathForChat(Chat, Dup);            // the user picked it twice
	Bridge->AddImagePathForChat(Chat, TEXT("C:/tmp/other.png"));

	Bridge->RemoveImagePathForChat(Chat, Dup);         // ...and removed ONE chip

	const TArray<FString>* Staged = FNwiroIKCancelTargetProbe::Staged(*Bridge, Chat);
	TestNotNull(TEXT("the chat still has staged images"), Staged);
	if (Staged)
	{
		TestEqual(TEXT("exactly one copy was removed, not both"), Staged->Num(), 2);
		TestEqual(TEXT("one duplicate survives"),
			Staged->FilterByPredicate([&Dup](const FString& P) { return P == Dup; }).Num(), 1);
		TestTrue(TEXT("the unrelated image is untouched"), Staged->Contains(TEXT("C:/tmp/other.png")));
	}

	// Removing the last entry drops the bucket rather than leaving an empty one behind.
	Bridge->RemoveImagePathForChat(Chat, Dup);
	Bridge->RemoveImagePathForChat(Chat, TEXT("C:/tmp/other.png"));
	TestNull(TEXT("an emptied bucket is removed"), FNwiroIKCancelTargetProbe::Staged(*Bridge, Chat));

	Bridge->RemoveFromRoot();
	return true;
}

// ─── Deferred cancellation ───────────────────────────────────────────────────
// The stall watchdog decides to cancel, then bounds its bridge call at 5s and unlocks the
// composer when that elapses. So the user can start a NEW turn while the cancel is still in
// flight, and the late cancel would kill it. The renderer cannot prevent that — its own
// generation token can suppress rendering, but a `session/cancel` already handed to the
// adapter is gone. The refusal has to be native.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKStaleCancelTest,
	"Nwiro.IntegrationKit.Chat.StaleCancelRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKStaleCancelTest::RunTest(const FString& Parameters)
{
	UNwiroIKBridge* Bridge = NewObject<UNwiroIKBridge>();
	Bridge->AddToRoot();

	TArray<FString> Frames;
	FNwiroIKCancelTargetProbe::TapOutbound(*Bridge, Frames);
	FNwiroIKCancelTargetProbe::Seed(*Bridge, TEXT("chat-a"), TEXT("sess-alpha"));
	FNwiroIKCancelTargetProbe::SetActive(*Bridge, TEXT("chat-a"));

	TestEqual(TEXT("the generation is readable by the renderer"),
		Bridge->GetChatTurnGeneration(TEXT("chat-a")), 7);
	TestEqual(TEXT("an unknown chat reports 0, not a usable generation"),
		Bridge->GetChatTurnGeneration(TEXT("chat-nope")), 0);

	// The user's new turn: the generation moves on while the watchdog's cancel is in flight.
	FNwiroIKCancelTargetProbe::AdvanceTurn(*Bridge, TEXT("chat-a"));
	TestEqual(TEXT("a new turn advanced the generation"),
		Bridge->GetChatTurnGeneration(TEXT("chat-a")), 8);

	// The watchdog's cancel finally arrives, carrying the generation it decided about.
	Frames.Empty();
	Bridge->CancelChatTurnAt(TEXT("chat-a"), 7);

	const FChatSession* A = FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-a"));
	TestNotNull(TEXT("chat-a still exists"), A);
	if (!A) { Bridge->RemoveFromRoot(); return false; }

	TestTrue(TEXT("the user's NEW turn keeps running"), A->bProcessing);
	TestEqual(TEXT("the new turn's generation is untouched"), (int64)A->TurnGeneration, (int64)8);
	TestEqual(TEXT("no session/cancel reached the wire"), Frames.Num(), 0);

	// A card raised by the live turn. NOTE: a chat can only ever have ONE pending card —
	// AddPending supersedes any older request for the same chat — so a two-card-one-chat
	// scenario is unconstructible here. The turn filter on RetireMatching is exercised
	// directly in NwiroIKPermissionCoordinatorSpec, where two turns CAN coexist.
	{
		FAcpPermissionSinks Sinks;
		Sinks.RespondToAcp = [](const FString&, const Acp::FRequestId&, const FString&) {};
		Sinks.EmitUiEvent = [](const FString&, const FString&, const FString&) {};
		Sinks.NowSeconds = []() { return 1000.0; };
		FAcpPermissionLiveContext Live;
		Live.IsOriginLive = [](const FAcpPermissionRequest&) { return true; };
		Live.IsOriginProcessLive = [](const FAcpPermissionRequest&) { return true; };
		FNwiroIKCancelTargetProbe::Coordinator(*Bridge).Configure(MoveTemp(Sinks), MoveTemp(Live));
	}
	FAcpPermissionCoordinator& Coord = FNwiroIKCancelTargetProbe::Coordinator(*Bridge);
	const int32 LiveTurnCard = Coord.AddPending(MakeCancelTargetRequest(TEXT("chat-a"), TEXT("live"), 8));

	// The same call with the CURRENT generation is honoured — the guard rejects staleness,
	// not deferred cancellation as such.
	Bridge->CancelChatTurnAt(TEXT("chat-a"), 8);
	A = FNwiroIKCancelTargetProbe::Get(*Bridge, TEXT("chat-a"));
	TestFalse(TEXT("a current-generation cancel is honoured"), A && A->bProcessing);
	TestTrue(TEXT("and it reaches the wire"),
		Frames.ContainsByPredicate([](const FString& F) { return F.Contains(TEXT("session/cancel")); }));
	TestFalse(TEXT("the cancelled turn's card is retired"), Coord.HasPending(LiveTurnCard));

	Bridge->RemoveFromRoot();
	return true;
}
