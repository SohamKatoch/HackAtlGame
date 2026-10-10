// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IHttpRouter.h"

/**
 * Embedded HTTP MCP Server for Nwiro.
 * Implements MCP protocol (JSON-RPC 2.0 over HTTP) so Claude Code can
 * discover and call UE5 tools directly — no external process needed.
 *
 * Endpoints:
 *   POST /mcp     — Streamable HTTP (MCP 2025-03-26)
 *   OPTIONS /mcp  — CORS preflight
 */
class FNwiroIKMCPServer
{
public:
	static void Start(int32 Port = 5353);
	static void Stop();
	static bool IsRunning();
	static int32 GetPort();

	/** Cheap liveness predicate (game-thread only): running flag AND router valid. */
	static bool IsHealthy();
	/**
	 * Merge this editor's MCP endpoint into the official ACP's documented
	 * process-local CODEX_CONFIG JSON. Existing top-level settings and unrelated
	 * MCP servers are preserved; only mcp_servers.nwiro belongs to this process.
	 */
	static bool BuildCodexProcessConfig(
		const FString& ExistingConfig,
		FString& OutConfig,
		FString& OutFailure);
	/** Drain-safe Stop()+Start() targeting the last requested port. Re-binds routes
	 *  and rewrites .mcp.json. Game-thread only. Returns true if a valid router
	 *  is bound afterward. */
	static bool Restart();

	/** Merge this server into the project's .mcp.json for Claude Code. */
	static void WriteClaudeConfig();

	/** Process a JSON-RPC MCP message — used by both HTTP handler and ACP bridge.
	 *
	 *  TOTAL and synchronous: a deferred ue.* tool cannot be awaited through
	 *  this entry point, so it answers with a structured "requires async
	 *  dispatch" error rather than blocking. Transports that CAN deliver a late
	 *  response (HTTP, ACP) should call ProcessJsonRpcAsync instead. */
	static TSharedPtr<FJsonObject> ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request);

	/** Asynchronous form, for transports able to answer later.
	 *
	 *  Returns true  => OutResponse is final; respond NOW. OnAsyncResponse is
	 *                   NOT invoked (never both — this keeps synchronous
	 *                   callers free of inline-callback re-entrancy).
	 *  Returns false => the request is parked; OnAsyncResponse fires EXACTLY
	 *                   ONCE, later, on the game thread — including the timeout
	 *                   and shutdown paths. OutResponse is not written. */
	static bool ProcessJsonRpcAsync(const TSharedPtr<FJsonObject>& Request,
	                                TFunction<void(TSharedPtr<FJsonObject>)> OnAsyncResponse,
	                                TSharedPtr<FJsonObject>& OutResponse);

private:
	// HTTP handlers
	static bool HandleMCPPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// MCP methods
	static TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonObject>& Params);
	/** Same true/false contract as ProcessJsonRpcAsync. A null OnAsyncResult
	 *  means the caller cannot defer, and the call then always returns true. */
	static bool HandleToolsCallAsync(const TSharedPtr<FJsonObject>& Params,
	                                 TFunction<void(TSharedPtr<FJsonObject>)> OnAsyncResult,
	                                 TSharedPtr<FJsonObject>& OutResult);

	// Tool dispatch — calls existing tool implementations directly
	static FString DispatchTool(const FString& ToolName, const FString& ArgsJson);

public:
	// Permission gating for tool calls
	struct FPendingToolCall
	{
		int32 PermissionId;
		/** The ORIGINAL JSON-RPC message, kept whole. Approval replays it
		 *  through ProcessJsonRpcAsync, which is what gives the approved call
		 *  the same argument validation, mcp_tool_start/mcp_tool_end pair,
		 *  isError classification and deferred-ue.* handling as every other
		 *  entry point. Keeping the whole request also preserves the `id`
		 *  EXACTLY — the previous FString form narrowed numeric ids to int32
		 *  and did not escape string ids. */
		TSharedPtr<FJsonObject> Request;
		/** Session header the client held when it made the request; an
		 *  `initialize` arriving while the prompt is open restamps the live one. */
		FString CapturedSessionId;
		FString ToolName;   // for the permission UI / logging only
		FString ArgsJson;   // for the permission UI / logging only
		FHttpResultCallback Callback;
	};
	static void RespondToToolPermission(int32 PermissionId, bool bAllowed);

	// Response helpers
	static TUniquePtr<FHttpServerResponse> MakeJsonResponse(int32 Code, const FString& Body);
	static FString MakeJsonRpcResponse(const FString& Id, const TSharedPtr<FJsonObject>& Result);
	static FString MakeJsonRpcError(const FString& Id, int32 Code, const FString& Message);
	/** Enabled tools only — what agents receive via MCP `tools/list` and the
	 *  local-LLM tools push. The only list a model may ever see. */
	static FString GetEnabledToolDefinitionsJson();
	/** Full inventory incl. disabled namespaced tools (each flagged
	 *  `"disabled":true|false`) — settings UI only, never for an agent. */
	static FString GetToolDefinitionsForSettingsJson();

	static TSharedPtr<IHttpRouter> HttpRouter;
	static int32 BoundPort;
	static int32 RequestedPort; // last port passed to Start(); target for auto-rebind
	static bool bRunning;
	static FString SessionId;

	static TArray<FPendingToolCall> PendingToolCalls;
	static FCriticalSection PendingToolCallsLock;
	static int32 NextPermissionId;
	static bool bSessionAllowed; // When true, skip permission for all tool calls
};
