// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Host-side hub that aggregates every EXTERNAL tool source into the MCP tool
 * layer. Mirrored in Nwiro Pro (FNwiroToolRegistry).
 *
 * Sources, each landing in its own assigned namespace:
 *   - plugin.<id>.<tool>   INwiroToolProvider modular features        (Phase 1)
 *   - plugin.<id>.<tool>   UFUNCTION(meta=(NwiroTool=...)) reflection  (Phase 2)
 *   - ue.<toolset>.<tool>  Epic 5.8 native UToolsetDefinition import   (Phase 3)
 *
 * Two integration points used by the MCP server:
 *   - AppendEnabledProviderDefs / AppendAllProviderDefsWithState: append the
 *     external, namespaced defs to the tools JSON array (reserved-namespace
 *     guards, dedup). The Enabled variant applies the user-disabled-namespace
 *     filter and is the ONLY one that may feed a model/agent; the WithState
 *     variant serializes everything (each def carrying `"disabled":true|false`)
 *     and exists solely for the settings UI, so disabling a source never hides
 *     its own re-enable toggle.
 *   - TryDispatch: route a namespaced call to the right source, on the game
 *     thread, returning a JSON result.
 *
 * The advertised set is FROZEN at session start (see EnsureFrozen) so the cached
 * tool-name prefix stays byte-stable across turns.
 */
/** Outcome of starting a ue.* (Epic ToolsetRegistry) execution. */
enum class ENwiroIKEpicStart : uint8
{
	/** Not a live ue.* dispatch (wrong namespace, disabled, unknown, or called
	 *  off the game thread). The caller must fall through to its own path. */
	NotEpic,
	/** Finished inline — OutResult is valid NOW and OnResult was NOT invoked.
	 *  Never both; this is what keeps synchronous callers re-entrancy-free. */
	Completed,
	/** OnResult will be invoked EXACTLY ONCE, later, on the game thread —
	 *  including the timeout and shutdown cases. OutResult is not written. */
	Deferred,
};

class FNwiroIKToolRegistry
{
public:
	/** Start a ue.* ToolsetRegistry call without ever blocking the game thread.
	 *
	 *  Must be called ON the game thread (the engine requires it for
	 *  ExecuteTool); off-thread callers get NotEpic and keep their old path.
	 *
	 *  Pass a null OnResult when the call site cannot await a late result — it
	 *  then never returns Deferred, answering instead with a structured
	 *  "requires async dispatch" error. This is what keeps the legacy
	 *  synchronous TryDispatch total and non-blocking.
	 *
	 *  Deferred completion, timeout, and shutdown all settle through one
	 *  exactly-once gate, so OnResult fires at most once on every path. */
	static ENwiroIKEpicStart BeginEpicTool(const FString& ToolName, const FString& ArgsJson,
	                                       TFunction<void(FString)> OnResult, FString& OutResult);

	/** TRANSPORT RESTART. Settle every still-open RESPONSE with `Reason` so no
	 *  client hangs, but KEEP the operation records, their dedup keys, and the
	 *  polling ticker: the engine work is still running and cannot be
	 *  cancelled, so forgetting it here would let a retry after the restart
	 *  start a duplicate operation. Records are reaped normally as their
	 *  futures complete. Game thread only; call while the transport can still
	 *  deliver. */
	static void DetachPendingEpicResponses(const FString& Reason);

	/** MODULE TEARDOWN. Settle open responses, then drop every record and
	 *  unregister the ticker — after this nothing of ours is reachable from
	 *  engine state, which is what makes DLL unload safe. Idempotent. */
	static void ShutdownPendingEpicCalls(const FString& Reason);

	/** In-flight deferred ue.* executions — for shutdown assertions and tests. */
	static int32 NumPendingEpicCalls();

	/** Append the external, namespaced tool defs the user has ENABLED to `J`
	 *  (the in-progress tools JSON array body). Adds a separating comma only
	 *  when needed and never leaves a trailing comma. No-op when nothing
	 *  external is registered. Every model/agent-facing consumer uses this. */
	static void AppendEnabledProviderDefs(FString& J);

	/** Append ALL external tool defs to `J`, including user-disabled ones, each
	 *  def carrying a `"disabled":true|false` field. For the settings UI only —
	 *  never feed this list to a model or agent. */
	static void AppendAllProviderDefsWithState(FString& J);

	/** If `ToolName` is namespaced (plugin./ue./mcp.), run it and write the JSON
	 *  result into OutResult, returning true. Returns false otherwise so the
	 *  caller continues to its own fallback. Rejects user-disabled namespaces. */
	static bool TryDispatch(const FString& ToolName, const FString& ArgsJson, FString& OutResult);

	// ─── End-user tool-source controls (settings) ───────────────────────────
	/** Namespaces (e.g. "plugin.acme", "ue") the user has switched off. Disabled
	 *  namespaces never appear in tools/list and are rejected in dispatch. */
	static void SetDisabledNamespaces(const TArray<FString>& Namespaces);
	static TArray<FString> GetDisabledNamespaces();

	/** Re-resolve all sources (call when providers (un)register or the user
	 *  toggles a source). The next session start re-freezes. */
	static void Invalidate();
};
