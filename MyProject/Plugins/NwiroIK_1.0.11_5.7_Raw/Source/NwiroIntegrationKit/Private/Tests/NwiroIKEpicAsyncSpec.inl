// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroIKEpicAsyncSpec.inl — settlement tests for the deferred ue.* (Epic
// ToolsetRegistry) execution path.
//
// Included from the BOTTOM of NwiroIKToolRegistry.cpp so it sits in the same
// translation unit and can drive the real internals directly. It calls the
// PRODUCTION ParkEpicFuture()/SettleResponse()/TickEpicCalls() — it does not
// reimplement the parking tail, so the tested path is the shipping path.
//
// Why a hand-driven TPromise rather than a real engine tool: the deadline,
// late-completion, and shutdown paths all need a future that does NOT resolve
// on its own. No real ue.* tool can supply one on demand —
// ue.automationtesttoolset.discovertests, the tool that exposed the original
// deadlock, now completes in ~0.3s and can never reach the deadline.

#include "Misc/AutomationTest.h"

#if NWIRO_HAS_TOOLSET_REGISTRY

namespace NwiroIKEpicAsyncTest
{
	using FToolResult = TValueOrError<FString, FString>;

	/** Heap-owned observation state. Captured BY VALUE (shared) into the sink so
	 *  a sink that fires after a pump gave up writes into live memory instead of
	 *  a dangling stack reference. */
	struct FSink
	{
		int32   Calls = 0;
		FString Last;
	};
	using FSinkPtr = TSharedPtr<FSink, ESPMode::ThreadSafe>;

	FSinkPtr MakeSink() { return MakeShared<FSink, ESPMode::ThreadSafe>(); }

	TFunction<void(FString)> SinkFn(FSinkPtr S)
	{
		return [S](FString R) { ++S->Calls; S->Last = MoveTemp(R); };
	}

	/** Pump the game thread until Predicate holds or the timeout elapses. Runs
	 *  the real core ticker (which drives TickEpicCalls) and the game-thread
	 *  task queue. */
	bool PumpUntil(TFunction<bool()> Predicate, double TimeoutSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - Start < TimeoutSeconds)
		{
			if (Predicate()) { return true; }
			FTSTicker::GetCoreTicker().Tick(0.05f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.02f);
		}
		return Predicate();
	}

	void Pump(double Seconds) { PumpUntil([] { return false; }, Seconds); }

	/** Is an operation with this key still tracked? This is exactly the
	 *  predicate BeginEpicTool uses to reject a duplicate, so asserting on it
	 *  asserts "a retry would be rejected right now". */
	bool KeyInFlight(const FString& Key)
	{
		for (const TPair<int64, FPendingEpicCall>& KV : GPendingEpic)
		{
			if (KV.Value.Key == Key) { return true; }
		}
		return false;
	}

	/** Always leaves the registry empty, even if a case fails early. */
	struct FScopedDrain
	{
		~FScopedDrain() { FNwiroIKToolRegistry::ShutdownPendingEpicCalls(TEXT("spec teardown")); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKEpicAsyncSpec,
	"NwiroIK.Epic.AsyncSettlement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKEpicAsyncSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroIKEpicAsyncTest;
	FScopedDrain Drain;

	TestTrue(TEXT("precondition: runs on the game thread"), IsInGameThread());
	TestEqual(TEXT("precondition: nothing leaked in"),
		FNwiroIKToolRegistry::NumPendingEpicCalls(), 0);

	// ── 1. Deferred completion settles exactly once, then frees the key ──────
	{
		TPromise<FToolResult> Promise;
		FSinkPtr S = MakeSink();
		const FString Key = TEXT("ue.spec.complete|{}");
		ParkEpicFuture(TEXT("ue.spec.complete"), Key, Promise.GetFuture(), SinkFn(S), 30.0, 0, 0.0);

		TestEqual(TEXT("completion: parked, not yet settled"), S->Calls, 0);
		TestTrue(TEXT("completion: key suppresses duplicates while running"), KeyInFlight(Key));

		Promise.SetValue(MakeValue(FString(TEXT("{\"ok\":1}"))));
		PumpUntil([S] { return S->Calls > 0; }, 5.0);

		TestEqual(TEXT("completion: sink fired exactly once"), S->Calls, 1);
		TestTrue(TEXT("completion: carries the tool result"),
			S->Last.Contains(TEXT("\"success\":true")) && S->Last.Contains(TEXT("\"ok\":1")));
		TestFalse(TEXT("completion: key released once the OPERATION finished"), KeyInFlight(Key));
		TestEqual(TEXT("completion: registry drained"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), 0);
	}

	// ── 2. Timeout settles the RESPONSE but the OPERATION keeps its slot ─────
	//    (a transport timeout is not cancellation), a retry is still rejected,
	//    and the eventual late completion must not answer twice.
	{
		TPromise<FToolResult> Promise;          // deliberately left unfulfilled
		FSinkPtr S = MakeSink();
		const FString Key = TEXT("ue.spec.hang|{}");
		ParkEpicFuture(TEXT("ue.spec.hang"), Key, Promise.GetFuture(), SinkFn(S), 1.0, 0, 0.0);

		TestTrue(TEXT("timeout: deadline fired"), PumpUntil([S] { return S->Calls > 0; }, 15.0));
		TestEqual(TEXT("timeout: exactly one response"), S->Calls, 1);
		TestTrue(TEXT("timeout: stable error code"),
			S->Last.Contains(TEXT("\"code\":\"nwiro.epic.timeout\"")));
		TestTrue(TEXT("timeout: marked non-retryable"), S->Last.Contains(TEXT("\"retryable\":false")));

		// THE distinction the earlier design got wrong: the response is settled,
		// but the engine operation is still running and must keep its slot.
		TestTrue(TEXT("timeout: OPERATION still tracked"), KeyInFlight(Key));
		TestEqual(TEXT("timeout: record retained"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), 1);

		// Retry BEFORE the late completion: must still be suppressed, otherwise
		// a retrying agent starts a second concurrent engine operation.
		Pump(0.5);
		TestTrue(TEXT("retry-after-timeout: still rejected as duplicate"), KeyInFlight(Key));
		TestEqual(TEXT("retry-after-timeout: no second response"), S->Calls, 1);

		// Late completion: response already settled, so it must not fire again;
		// the operation is now done, so the slot must be released.
		Promise.SetValue(MakeValue(FString(TEXT("{\"late\":1}"))));
		PumpUntil([&Key] { return !KeyInFlight(Key); }, 5.0);

		TestEqual(TEXT("late completion: still exactly one response"), S->Calls, 1);
		TestTrue(TEXT("late completion: original timeout payload untouched"),
			S->Last.Contains(TEXT("nwiro.epic.timeout")));
		TestFalse(TEXT("late completion: OPERATION slot released"), KeyInFlight(Key));
		TestEqual(TEXT("late completion: registry drained"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), 0);
	}

	// ── 3. A null sink still tracks the operation ────────────────────────────
	//    (the leak the previous version had: it started work and forgot it).
	{
		TPromise<FToolResult> Promise;
		const FString Key = TEXT("ue.spec.nosink|{}");
		ParkEpicFuture(TEXT("ue.spec.nosink"), Key, Promise.GetFuture(), nullptr, 30.0, 0, 0.0);

		TestTrue(TEXT("null sink: operation tracked"), KeyInFlight(Key));
		Promise.SetValue(MakeValue(FString(TEXT("{\"ok\":1}"))));
		PumpUntil([&Key] { return !KeyInFlight(Key); }, 5.0);
		TestFalse(TEXT("null sink: reaped on completion, no crash"), KeyInFlight(Key));
	}

	// ── 4. Shutdown drain settles open responses; late completion is inert ───
	{
		TPromise<FToolResult> Promise;
		FSinkPtr S = MakeSink();
		ParkEpicFuture(TEXT("ue.spec.drain"), TEXT("ue.spec.drain|{}"),
			Promise.GetFuture(), SinkFn(S), 300.0, 0, 0.0);

		TestEqual(TEXT("drain: one in flight"), FNwiroIKToolRegistry::NumPendingEpicCalls(), 1);

		FNwiroIKToolRegistry::ShutdownPendingEpicCalls(TEXT("MCP server restarting; please retry"));

		TestEqual(TEXT("drain: settled exactly once"), S->Calls, 1);
		TestTrue(TEXT("drain: stable shutdown code"),
			S->Last.Contains(TEXT("\"code\":\"nwiro.epic.shutdown\"")));
		TestEqual(TEXT("drain: registry emptied"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), 0);

		// The future outlives the drain; resolving it must touch nothing. This
		// is the module-unload safety property: because we only ever POLLED the
		// future (never TFuture::Next), dropping it unregisters no plugin code.
		Promise.SetValue(MakeValue(FString(TEXT("{\"late\":2}"))));
		Pump(1.0);
		TestEqual(TEXT("drain: late completion did not double-respond"), S->Calls, 1);
	}

	// ── 5. Many concurrent operations each settle exactly once ───────────────
	{
		constexpr int32 N = 8;
		TArray<TPromise<FToolResult>> Promises; Promises.SetNum(N);
		TArray<FSinkPtr> Sinks;
		for (int32 i = 0; i < N; ++i)
		{
			Sinks.Add(MakeSink());
			ParkEpicFuture(FString::Printf(TEXT("ue.spec.multi%d"), i),
				FString::Printf(TEXT("ue.spec.multi%d|{}"), i),
				Promises[i].GetFuture(), SinkFn(Sinks[i]), 60.0, 0, 0.0);
		}
		TestEqual(TEXT("concurrency: all parked"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), N);

		// Resolve out of order — settlement must not depend on park order.
		for (int32 i = N - 1; i >= 0; --i)
		{
			Promises[i].SetValue(MakeValue(FString::Printf(TEXT("{\"i\":%d}"), i)));
		}
		PumpUntil([&Sinks] {
			for (const FSinkPtr& S : Sinks) { if (S->Calls == 0) { return false; } }
			return true;
		}, 10.0);

		bool bAllOnce = true;
		for (const FSinkPtr& S : Sinks) { if (S->Calls != 1) { bAllOnce = false; } }
		TestTrue(TEXT("concurrency: every operation settled exactly once"), bAllOnce);
		TestEqual(TEXT("concurrency: registry drained"),
			FNwiroIKToolRegistry::NumPendingEpicCalls(), 0);
	}

	return true;
}

#endif // NWIRO_HAS_TOOLSET_REGISTRY
