// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Misc/InteractiveProcess.h"
#include "Dom/JsonValue.h"
// adapter-reliability-w1 build-fix: Ticker.h needed in HEADER (not just cpp)
// because TimeoutTickerHandle below is typed as FTSTicker::FDelegateHandle —
// a strong-typed handle distinct from the generic FDelegateHandle in newer
// UE. The cpp's existing include only satisfied the AddTicker call site, not
// the field declaration in the class body.
#include "Containers/Ticker.h"
#include "NwiroIKAcpTypes.h"
#include "NwiroIKPermissionCoordinator.h"
#include "NwiroIKBridge.generated.h"

// Per-chat session state
struct FChatSession
{
	FString ChatId;
	FString AdapterId;
	FString SessionId;
	bool bProcessing = false;
	// F4: per-turn latch — true once THIS turn has produced genuine agent
	// activity (content / reasoning / tool call). Reset to false at every prompt
	// send (DoSendPrompt) and set in HandleSessionUpdate. Read at the stdout
	// keyword-classification emit site to REFUSE to tear down a live, streaming
	// turn on a stray classified line (codex-acp can print a recovered
	// "unauthorized" mid-stream). A dedicated correctness latch — deliberately
	// NOT reusing the LastAdapterStage display string, whose values/reset timing
	// future UI work could change and silently break this gate.
	bool bHasStreamed = false;
	FString PendingMessage;
	int32 SessionRpcId = 0;
	int32 PromptRpcId = 0;
	FString LastModel;

	// Official Codex ACP configuration gate. The successor advertises mode and
	// base-model selectors in session/new.configOptions. Every Codex prompt is
	// held until mode and model are acknowledged in sequence with the exact
	// requested currentValue. PreparedPromptParams is never sent on an error,
	// timeout, cancellation, adapter restart, or process-generation mismatch.
	TSet<FString> AvailableModelIds;
	TSet<FString> AvailableModeIds;
	int32 ModelConfigRpcId = 0;
	double ModelConfigRpcStartedAt = 0.0;
	FString PendingConfigId;
	FString PendingMode;
	FString PendingModel;
	TSharedPtr<FJsonObject> PreparedPromptParams;
	uint64 ModelConfigProcessGeneration = 0;
	uint64 BoundProcessGeneration = 0;
	TSet<FString> SeenToolCallIds;

	// Monotonic per chat. Bumped when a prompt is sent AND when a turn ends by cancel or
	// close, so "the turn that raised this permission" is expressible as a value.
	//
	// Session identity cannot carry this: cancelling a Claude turn PRESERVES the ACP
	// session, so a permission card from the cancelled turn would still validate against
	// a live session during the next turn. Starts at 0, which no live turn ever uses —
	// a request captured with TurnGeneration 0 can never match.
	uint64 TurnGeneration = 0;

	// The permission mode as it was for THIS chat at prompt-send time.
	//
	// UNwiroIKBridge::CurrentMode is bridge-global and the frontend overwrites it (via
	// setmode) before EVERY send. Chat A sending in `default`, then the user switching to
	// chat B and sending with `bypassPermissions`, left chat A's delayed permission
	// request to be judged under chat B's mode and auto-approved. Authority has to travel
	// with the request, not be read from a global at settle time.
	FString ModeSnapshot;

	// adapter-reliability-w0: lifecycle tracking. LastAdapterStage records the
	// stage the chat last reached (e.g. "creating_session", "sending_prompt",
	// "streaming"); LastAdapterCode/Error pair the most recent classified
	// failure with that stage. Read by the JS-side watchdog so a 90s timeout
	// surfaces "stage=creating_session" instead of a generic "no response".
	FString LastAdapterStage;
	FString LastAdapterCode;
	FString LastAdapterError;

	// adapter-reliability-w1: per-RPC start times for timeout enforcement.
	// SessionRpcStartedAt drives session_new_timeout (default 30s, env
	// NWIRO_SESSION_NEW_TIMEOUT_SECONDS); PromptRpcStartedAt drives
	// first_token_timeout (default 180s, env NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS).
	// Both are cleared on success — empty
	// (0.0) means no in-flight RPC for that stage. Monotonic clock via
	// FPlatformTime::Seconds() so wall-clock jumps don't false-trigger.
	double SessionRpcStartedAt = 0.0;
	double PromptRpcStartedAt = 0.0;

	// adapter-reliability-w5: snapshot of FAdapterProcess::BytesFromStdout
	// taken at the same site that sets PromptRpcStartedAt (DoSendMessage,
	// just after session/prompt RPC dispatch). Subtracting at timeout-fire
	// time gives bytes-during-this-turn — the diagnostic signal that
	// distinguishes "adapter silent" (0) from "adapter writing but stuck
	// mid-stream" (>0). Always rewritten on each prompt-send so prior-turn
	// values can't leak.
	int64 BytesAtPromptSendStart = 0;

	// adapter-reliability-w2: latched on the first session_new_timeout. For
	// session-attached adapters, the retry omits mcpServers to isolate MCP.
	// Codex always uses process-local CODEX_CONFIG, so the same latch records
	// only its one clean process-restart retry.
	bool bRetriedSessionWithoutMcp = false;

	// adapter-reliability-w6: session resume/replay state. When the adapter
	// advertises loadSession and a PriorSessionId is known, the chat attempts
	// session/load (DoLoadSession) instead of session/new. PriorSessionId is
	// the id JS asks us to resume (via SetResumeSessionId); cleared on adapter
	// switch or empty-string clear. AdapterVersionAtCreate snapshots the
	// adapter's AgentVersion at session/new success for JS-side version-skew
	// gating (D8). SessionLoadRpcId/SessionLoadStartedAt mirror the
	// SessionRpcId/SessionRpcStartedAt pair for the in-flight session/load RPC
	// (0 / 0.0 means none in flight). bInReplay is armed before session/load is
	// sent so the inbound history replay (user/agent chunks, mode updates) is
	// re-tagged or suppressed rather than treated as live; disarmed on load
	// success/failure. bResumeAttempted latches once a load has been tried;
	// bResumeFailed latches on a load failure so the chat falls back to
	// session/new and does not retry the resume.
	FString PriorSessionId;
	FString AdapterVersionAtCreate;
	int32 SessionLoadRpcId = 0;
	double SessionLoadStartedAt = 0.0;
	bool bInReplay = false;
	bool bResumeAttempted = false;
	bool bResumeFailed = false;

	// unknown-session-recovery: LastSentPrompt is the raw user message the last
	// DoSendPrompt shipped for this chat (pre-preamble — the retry rebuilds the
	// preambles itself), retained because session/prompt keeps no copy of the
	// outbound text. When the adapter answers the prompt with "unknown session"
	// (its side of the session died while we kept the id), the recovery path in
	// HandleRpcResponse queues this as PendingMessage for the recreated session.
	// UnknownSessionRetryCount caps that transparent recreate+resend at ONE
	// attempt per user send; reset only in SendMessage on a genuine new send.
	FString LastSentPrompt;
	int32 UnknownSessionRetryCount = 0;

	// unknown-session-recovery (image-aware): LastSentImageBlocks retains the
	// EXACT wire blocks (type/data/mimeType JSON) the last DoSendPrompt
	// shipped for this chat — empty when the capability gate dropped the
	// attachments, i.e. it mirrors the wire, not the attach list. The recovery
	// path stages a copy in PendingRetryImageBlocks so the one-shot
	// recreate+resend is byte-identical (re-reading paths could silently skip
	// a deleted temp file or ship different bytes) and never touches the
	// bridge-global PendingImages, which another chat may have refilled in the
	// meantime. bLastSentHadImages records whether the wire prompt carried
	// image blocks at all: if it did but the retained blocks are gone (an
	// adapter-exit sweep cleared them), recovery must FAIL CLOSED instead of
	// silently resending a reduced request. PendingRetryImageBlocks follows
	// the PendingMessage lifecycle: consumed by DoSendPrompt, dropped on
	// terminal failure / cancel / genuine new send / adapter-exit sweeps.
	// Memory is bounded by a single turn's attachments.
	TArray<TSharedPtr<FJsonValue>> LastSentImageBlocks;
	TArray<TSharedPtr<FJsonValue>> PendingRetryImageBlocks;
	bool bLastSentHadImages = false;
};

struct FTerminalResult
{
	FString Output;
	int32 ExitCode = 0;
};

// B1 authorization evidence for one MCP tool call. File scope rather than nested in
// UNwiroIKBridge so the decision function that consumes it can be a free function and
// therefore directly testable — see IsExemptNwiroMcpApproval in NwiroIKBridge.cpp.
struct FNwiroMcpToolClaim
{
	// The adapter and the specific OS process that minted this claim. An approval is
	// only honoured while BOTH still match, so evidence cannot outlive its process.
	FString AdapterId;
	uint64  ProcessGeneration = 0;
	// "mcp.nwiro.<tool>" — the only place the tool's name exists at approval time.
	FString Title;

	// The ORIGIN of the turn that produced this evidence, captured now rather than
	// reconstructed when the approval arrives.
	//
	// Reading the chat's *current* turn and mode at approval time stamps a delayed frame
	// with whatever turn is running by then:
	//     turn A emits the rich tool_call -> A is cancelled (Claude keeps the session)
	//     -> turn B starts -> A's approval finally arrives -> it is labelled turn B
	// The liveness check then validates a tuple that is internally consistent and simply
	// wrong. Evidence has to carry the identity of the turn that created it.
	FString ChatId;
	uint64  TurnGeneration = 0;
	FString ModeSnapshot;
};

// What the permission handler decided to do. Named rather than implied by control flow so
// the decision can be computed by a pure function, asserted in tests, and logged as one
// value — previously it was spread across four early returns with no single place that
// said what had been decided.
enum class ENwiroPermissionDisposition : uint8
{
	ShellReject,   // shell evidence or prior poison — refuse regardless of mode
	AutoAllow,     // bypassPermissions / acceptEdits
	AutoDeny,      // plan mode
	ShowCard,      // ask the user
};

UCLASS()
class UNwiroIKBridge : public UObject
{
	GENERATED_BODY()

public:
	static UNwiroIKBridge* Instance;

	// JS bridge API
	UFUNCTION() void SetActiveChat(const FString& ChatId);
	UFUNCTION() void SendMessage(const FString& Message);
	UFUNCTION() void RespondToPermission(int32 RequestId, const FString& OptionId);
	UFUNCTION() void CancelMessage();
	// Cancel a NAMED chat's turn. Prefer this over CancelMessage: ActiveChatId is the last
	// chat that SENT (SetActiveChat is called only from sendViaBridge, never on a UI chat
	// switch), so a no-arg cancel can end a turn in a chat the user is not even looking at.
	//
	// For a caller acting NOW — the Stop button — "whatever is running in this chat" is
	// exactly the intent, so no generation is taken.
	UFUNCTION() void CancelChatTurn(const FString& ChatId);
	// Cancel a named chat's turn ONLY IF it is still the turn the caller decided about.
	//
	// For AUTOMATIC callers whose decision and its arrival are separated in time. The stall
	// watchdog bounds its own bridge call at 5s and unlocks the composer on timeout, so the
	// user can start a new turn while the cancel is still in flight; without this check the
	// late cancel would kill that new turn. Read the expected value with
	// GetChatTurnGeneration BEFORE issuing the call.
	UFUNCTION() void CancelChatTurnAt(const FString& ChatId, int32 ExpectedTurnGeneration);
	// The chat's current turn generation, or 0 for an unknown chat / no turn. Monotonic per
	// chat: bumped on every prompt send and every cancel or close.
	UFUNCTION() int32 GetChatTurnGeneration(const FString& ChatId) const;
	UFUNCTION() void CloseChat(const FString& ChatId);
	UFUNCTION() void NewConversation();
	UFUNCTION() bool IsProcessing() const;
	UFUNCTION() FString PollResponse(); // Legacy — kept for compat but push is preferred
	UFUNCTION() bool IsTCPRunning() const;
	UFUNCTION() void Log(const FString& Text);
	UFUNCTION() FString GetPluginInfo() const;
	UFUNCTION() FString GetDisabledToolNamespaces() const;
	UFUNCTION() void SetDisabledToolNamespaces(const FString& JsonArray);
	UFUNCTION() bool GetToolbarButton() const;
	UFUNCTION() void SetToolbarButton(bool bEnabled);
	UFUNCTION() void SetMode(const FString& Mode);
	UFUNCTION() void SetModel(const FString& Model);
	UFUNCTION() FString GetMode() const;
	UFUNCTION() FString GetModel() const;
	UFUNCTION() void SetAdapter(const FString& Adapter);
	UFUNCTION() FString GetAdapter() const;

	/** Snapshot an mcp_tool_* event's routing target AND its badge policy together.
	 *
	 *  These must not be read from separate globals. Send setup calls setadapter()
	 *  well before setactivechat(), so CurrentAdapter can already describe the chat
	 *  being opened while events still route to the previous one — a legacy-Codex
	 *  chat would then inherit a policy computed from an incoming Claude chat and
	 *  lose its only badge. The policy therefore comes from the RECEIVING chat's own
	 *  AdapterId, and both values are captured in one call.
	 *
	 *  bOutNeedsBadgeFallback is true only for legacy Zed codex-acp, the one runtime
	 *  that emits no ACP tool_call of its own. The frontend cannot determine this —
	 *  the adapter is named "codex" on both the official and legacy runtimes.
	 *
	 *  GAME-THREAD ONLY (reads ChatSessions). */
	void GetMcpToolEventContext(FString& OutChatId, bool& bOutNeedsBadgeFallback) const;
	UFUNCTION() void AddImagePath(const FString& Path);
	// Stage an attachment against a NAMED chat. `AddImagePath` has to fall back to
	// ActiveChatId, which is the last chat that SENT rather than the one on screen.
	UFUNCTION() void AddImagePathForChat(const FString& ChatId, const FString& Path);
	// Un-stage one attachment. The picker stages at PICK time, so the composer removing a
	// chip must be able to undo that — otherwise the file the user deleted is still sent.
	UFUNCTION() void RemoveImagePathForChat(const FString& ChatId, const FString& Path);
	UFUNCTION() void OpenUrl(const FString& Url);
	UFUNCTION() void OpenImagePicker();
	// Open the picker on behalf of a NAMED chat. OpenFileDialog is MODAL: the user can
	// switch conversations before it closes, so the owning chat has to be decided when the
	// button is pressed, not when the dialog returns.
	UFUNCTION() void OpenImagePickerForChat(const FString& ChatId);
private:
	// Shared body. `bAddressed` selects the modern contract (emit only; the composer's
	// validated tray is authoritative) over the legacy one (stage at pick time).
	void OpenImagePickerImpl(const FString& ChatId, bool bAddressed);
public:
	UFUNCTION() FString SaveImageBase64(const FString& Base64Data, const FString& FileName);
	UFUNCTION() void ClearImages();
	UFUNCTION() void DownloadAdapter(const FString& Adapter, const FString& Url);
	// Windows-first official Codex ACP acquisition. Package name/version are
	// fixed natively; the renderer cannot supply a URL or npm package spec.
	UFUNCTION() void InstallManagedAdapter(const FString& Adapter);
	UFUNCTION() FString GetAdapterRuntimeStatus(const FString& Adapter);
	UFUNCTION() void DeleteAdapter(const FString& Adapter);
	UFUNCTION() bool IsAdapterAvailable(const FString& Adapter);
	UFUNCTION() bool IsCliAvailable(const FString& Adapter);
	UFUNCTION() void StartAdapter(const FString& Adapter);
	// adapter-reliability-w4b: user-triggered restart from a self-healing
	// button in the chat error bubble. Wraps the internal DoRestartAdapter
	// (kill + EnsureProcess respawn). Reason is fixed to "user_action" so
	// telemetry / logs distinguish from automatic restart-on-timeout (Wave 2).
	// Returns true iff a process is actually running after the call —
	// codex-review-fix: previous void return let JS show "Adapter restarted"
	// even when the entry had been removed (after adapter_exited) or
	// preflight blocked the relaunch.
	UFUNCTION() bool RestartAdapter(const FString& Adapter);
	UFUNCTION() FString SearchAssets(const FString& Query);
	UFUNCTION() FString GetStaticMeshes();
	UFUNCTION() FString GetStaticMeshesByPath(const FString& FolderPath);
	UFUNCTION() bool ExecutePython(const FString& Code);
	UFUNCTION() bool SaveFile(const FString& Path, const FString& Content);
	UFUNCTION() void StartMCPServer(int32 Port);
	UFUNCTION() void StopMCPServer();
	UFUNCTION() void SetMCPPort(int32 Port);
	/** Enabled tools only — also feeds the local-LLM tools push. */
	UFUNCTION() FString GetToolDefinitions();
	/** Full inventory incl. disabled tools (flagged) — settings UI only. */
	UFUNCTION() FString GetToolDefinitionsForSettings();
	UFUNCTION() FString GetSecret(const FString& Key);
	UFUNCTION() void SetSecret(const FString& Key, const FString& Value);
	UFUNCTION() void SetChatExtensions(const FString& ChatId, const FString& ExtensionsJson);
	UFUNCTION() void SetAdapterContext(const FString& ContextJson);
	// Secure apiKey delivery for the localllm adapter. Stored as a private
	// member; written to the spawned shim's environment via FPlatformMisc::
	// SetEnvironmentVar in EnsureProcess. NEVER appears in log output, JSON
	// serialization, or process argv. See PLAN.md (option (d)-hard).
	UFUNCTION() void SetLocalLlmApiKey(const FString& Key);
	// Pre-warm the local-LLM adapter's currently-configured model. Sends an
	// ACP `session/warmup` RPC to the shim, which forces Ollama to load
	// weights into VRAM (or runs a no-op connectivity check for backends
	// that hold models persistently like LM Studio). Async — the result
	// arrives as an `EnqueueResponse(TEXT("warmup"), <jsonResult>, "")`
	// delivery so the JS settings store can update its WarmUpState. Safe
	// to call when the adapter isn't running yet (will spawn it).
	UFUNCTION() void WarmupLocalLlm();
	UFUNCTION() void SetPromptPreambles(const FString& ExecutePreamble, const FString& PlanPreamble);
	// adapter-reliability-w6: JS tells the bridge which prior session id to
	// resume for a chat (always-call contract — empty SessionId clears it).
	// No AdapterVersion arg: version-skew gating is JS-side per D8.
	UFUNCTION() void SetResumeSessionId(const FString& ChatId, const FString& SessionId);
	// adapter-reliability-w6: C++-owned per-(chat,adapter) session persistence.
	// GetChatSession returns the stored JSON blob (empty if none); SaveChatSession
	// writes it atomically. Bodies live in NwiroIKBridge.cpp (Part 2).
	UFUNCTION() FString GetChatSession(const FString& ChatId, const FString& AdapterId);
	UFUNCTION() bool SaveChatSession(const FString& ChatId, const FString& AdapterId, const FString& Json);
	UFUNCTION() void ImportMesh(const FString& Url, const FString& FileName, const FString& DestFolder, bool bRevealInBrowser);
	/**
	 * Import an already-saved ElevenLabs audio mp3 into the project as a
	 * USoundWave. Called by the ElevenLabs panel's "Import" button after
	 * the user previews the generated clip. The factory is auto-picked by
	 * file extension — USoundFactory handles mp3/wav/ogg natively.
	 *
	 * @param LocalPath   absolute path to the mp3 already on disk (Saved/Nwiro/audio/...)
	 * @param DestFolder  package path under /Game (e.g. /Game/Nwiro/Audio/voice)
	 * @param DestName    asset name (no extension)
	 * @param bRevealInBrowser  jump to the asset in Content Browser after import
	 */
	UFUNCTION() void ImportAudio(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser);

	/** Decode a base64 audio blob, write it to a temp file, then import as
	 *  USoundWave. Used by the ElevenLabs panel which generates audio in the
	 *  browser via the API proxy and never writes to disk itself. */
	UFUNCTION() void ImportAudioBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser);

	/** Native "Save As" dialog for the ElevenLabs panel's download button —
	 *  CEF's `<a download>` is unreliable inside the editor's embedded browser.
	 *  Decodes base64 bytes and writes to whatever the user picks. */
	UFUNCTION() void SaveAudioToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext);

	/** Decode a base64 texture (PNG/JPG/WebP), write to a temp file, then
	 *  import as UTexture2D into the Content Browser. Used by the fal.ai
	 *  panel where the browser already has the bytes from the proxy or the
	 *  fal_task event. Factory left null — auto-picks UTextureFactory. */
	UFUNCTION() void ImportTextureBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser);

	/** Native "Save As" dialog for the fal.ai panel's download button. */
	UFUNCTION() void SaveTextureToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext);

	/** Download a remote texture URL (fal CDN) and import as UTexture2D.
	 *  Avoids round-tripping multi-MB bytes through the CEF bridge — the
	 *  panel just hands us the URL the plugin already produced. */
	UFUNCTION() void ImportTextureFromUrl(const FString& Url, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser);

	/** Native "Save As": downloads a remote URL and writes wherever the user
	 *  picks. Same purpose as SaveTextureToDisk but takes a URL instead of bytes. */
	UFUNCTION() void SaveTextureFromUrl(const FString& Url, const FString& SuggestedName, const FString& Ext);

	// adapter-reliability-w1: UObject teardown — removes the timeout ticker so
	// the lambda inside FTSTicker doesn't outlive the bridge. Without this,
	// editor hot-reload (which destroys + re-creates UObjects) would leak a
	// raw `this` into the global ticker queue.
	virtual void BeginDestroy() override;

#if WITH_EDITOR
	// Editor-only test entry point. Feeds a synthetic JSON-RPC line into ProcessLine
	// so the union-aware id round-trip can be verified without a live ACP adapter.
	// See tasks/codex-acp-string-id.md "Verification plan".
	UFUNCTION(BlueprintCallable, CallInEditor, Category="Nwiro|Debug")
	void DebugInjectFrame(const FString& RawJsonLine);
#endif

	// Content Pipeline — download remote asset packs (PCG demo / Megascans-style bundles)
	// All return JSON strings; the JS side polls GetContentStatus until status is completed/failed.
	UFUNCTION() FString StartContentDownload(const FString& RequestJson);
	UFUNCTION() FString GetContentStatus(const FString& Hash);
	UFUNCTION() bool CancelContentDownload(const FString& Hash);
	UFUNCTION() FString ListCachedContents();


	bool IsChatExtensionEnabled(const FString& ExtName) const;

	void EnqueueResponse(const FString& Type, const FString& Data);
	void EnqueueResponse(const FString& Type, const FString& Data, const FString& ChatId);

	// F4 gap-close: a :5353 MCP tool executing is genuine activity for the live
	// turn. The backend MCP event can be the first observable activity even when an
	// adapter also emits an ACP tool_call, so the ACP stream latch may still be false
	// when a stray mid-turn "unauthorized" arrives. Mark the processing chat streamed
	// to prevent that line from tearing down a healthy tool-first turn.
	// GAME-THREAD ONLY (ChatSessions is game-thread-owned) — callers must marshal.
	void MarkActiveTurnStreamed();
	void EnqueueAdapterCapabilities(const FString& AdapterId);
	void PushEvent(const FString& Type, const FString& Data, const FString& ChatId = TEXT(""));
	FString GetChatIdForToolUse(const FString& ToolUseId) const;

private:
	void BeginManagedAdapterInstall(const FString& Adapter);
	bool QuiesceCodexProcessForRuntimeChange(const FString& RuntimeSource);

	// Per-adapter processes
	struct FAdapterProcess {
		TSharedPtr<FInteractiveProcess> Process;
		bool bInitialized = false;
		int32 InitRpcId = 0;
		FString StdoutBuffer;
		// Monotonic identity for one launched OS process. A delayed model-config
		// response from a replaced Codex process must never release a held prompt.
		uint64 ProcessGeneration = 0;

		// adapter-reliability-w5: monotonic lifetime byte counter from this
		// adapter's stdout. Incremented in the OnOutput lambda before the
		// buffer append. Sessions snapshot this at session/prompt-send time
		// (FChatSession::BytesAtPromptSendStart) so first_token_timeout can
		// report bytes-during-this-turn, partitioning the failure space:
		//   delta == 0          → adapter went silent (crash / pipe stuck)
		//   delta > 0, no event → bytes flowing but no parsed agent_message_chunk
		//                          (upstream slow OR framing/MCP issue mid-stream)
		// Same-thread access (game-thread OnOutput + game-thread ticker), so
		// plain int64 — no atomic/lock needed.
		int64 BytesFromStdout = 0;

		// adapter-reliability-w0: pre-session lifecycle (launching_process,
		// initializing_acp). LastError/LastErrorCode are populated by the
		// preflight check so SendMessage can re-emit them as a classified
		// adapter_error with a chatId target. RestartCount is groundwork for
		// Wave 2's adapter restart-on-timeout retry.
		FString LastStage;
		FString LastError;
		FString LastErrorCode;
		int32 RestartCount = 0;

		// adapter-reliability-w1: timestamp of the in-flight initialize RPC.
		// 0.0 means no pending initialize. Cleared on initialize success;
		// non-zero + elapsed > 60s triggers initialize_timeout in the ticker.
		double InitRpcStartedAt = 0.0;

		// adapter-reliability-w1 codex-review-fix-2: per-adapter set of ACP
		// session ids we've explicitly cancelled (first_token_timeout).
		// Per-adapter (not bridge-wide) so adapter restart / removal clears
		// it for free — when AdapterProcesses.Remove(AdapterId) runs, this
		// set goes with it. Cross-adapter id collisions are theoretical but
		// SessionToChatId is also per-bridge, so the same constraint applies
		// uniformly across all session-id state.
		TSet<FString> RejectedAcpSessionIds;

		// adapter-reliability: capabilities parsed from this adapter's
		// initialize response (protocolVersion, agentCapabilities.loadSession,
		// agentInfo.name/version). Populated in HandleRpcResponse on init
		// success; surfaced to the frontend via EnqueueAdapterCapabilities.
		bool bSupportsLoadSession = false;
		// Phase-1 image input: agentCapabilities.promptCapabilities.image — whether
		// this adapter's current model accepts image content. Gates image emission
		// in DoSendPrompt (the shim degrades cleanly if we send anyway).
		bool bSupportsImageInput = false;
		FString AgentName;
		FString AgentVersion;
		// Resolver provenance for this exact process ("managed" or "global").
		// Used to quiesce only the runtime whose files are about to change.
		FString RuntimeSource;
		// Exact package.json version from the verified launch spec. Codex
		// initialize is accepted only when agentInfo.version matches this value,
		// preventing a substituted entry point from passing on name alone.
		FString ExpectedPackageVersion;
		// MCP port captured in CODEX_CONFIG when this long-lived ACP process
		// launched. A later server rebind to another port must restart Codex.
		int32 CodexMcpPort = 0;
		double NegotiatedProtocolVersion = 0.0;

		// Windows-only Codex runtime guards, stored as opaque handles so this
		// private bridge type remains platform-neutral. The shared file lease
		// protects only a managed runtime process; the Job Object owns the full
		// Node/Codex child tree for both managed and global launch sources.
		void* ManagedRuntimeUseLockHandle = nullptr;
		void* CodexProcessJobHandle = nullptr;
		// Set when Windows created a Codex process but Job assignment failed.
		// Killing/waiting for the known root does not prove that an already
		// detached descendant exited, so this latch is intentionally permanent
		// for the lifetime of the editor. While set, managed runtime leases must
		// not be released and no replacement Codex process may be launched.
		bool bCodexShutdownUnproven = false;

		// In-flight session/warmup RPC id, mapped to a per-adapter slot so a
		// click on "Load Model" from settings can be matched to its response
		// when it arrives via HandleRpcResponse. 0 means no pending warmup.
		// Per-adapter rather than bridge-wide because warmup is an adapter-
		// scoped concept (cloud adapters never receive a warmup request).
		int32 PendingWarmupRpcId = 0;
	};
	bool StopCodexProcessTreeAndWait(FAdapterProcess& AdapterProcess, const FString& Reason);
	void ReleaseCodexRuntimeGuards(FAdapterProcess& AdapterProcess);
	TMap<FString, FAdapterProcess> AdapterProcesses;
	int32 NextRpcId = 1;
	uint64 NextProcessGeneration = 1;
	bool bManagedCodexInstallInProgress = false;

	// The one cancel implementation. ExpectedTurnGeneration 0 means "cancel whatever turn
	// is current"; a non-zero value is the turn a deferred caller decided about and scopes
	// permission retirement to that turn's cards. Callers validate staleness before this.
	void CancelChatTurnInternal(const FString& ChatId, int32 ExpectedTurnGeneration);

	// Multi-session: one session per chat
	TMap<FString, FChatSession> ChatSessions;
	FString ActiveChatId;

#if WITH_DEV_AUTOMATION_TESTS
	// NwiroIKCancelTargetSpec drives CancelChatTurn across two chats to prove the cancel
	// lands on the NAMED chat rather than on ActiveChatId. Both are private and there is
	// no public read of another chat's state (IsProcessing reports the active one only),
	// so the assertion is unreachable without this. Dev builds only.
	friend struct FNwiroIKCancelTargetProbe;

	// Outbound ACP frame tap for automation specs. Unbound in every normal run. Lets a
	// spec assert on the JSON the bridge actually built — SendRaw drops everything when no
	// adapter process is live, so without this a wire assertion cannot be written at all.
	TFunction<void(const FString& /*AdapterId*/, const FString& /*Json*/)> TestOutboundTap;
#endif

	// Reverse lookup: RPC id -> chatId
	TMap<int32, FString> RpcToChatId;
	// Reverse lookup: sessionId -> chatId
	TMap<FString, FString> SessionToChatId;

	// Map toolUseId → chatId for MCP permission routing
	TMap<FString, FString> ToolUseToChatId;
	TSet<FString> BlockedShellToolCallIds;

	// ── B1 authorization evidence ────────────────────────────────────────────────────
	// One record per rich tool_call frame positively identified as our own MCP server
	// (title "mcp.nwiro.*" AND rawInput.server == "nwiro").
	//
	// Needed because codex-acp's MCP *approval* envelope is deliberately contentless —
	// {toolCallId, kind:"execute", status:"pending"}, no rawInput, no title (verified in
	// buildPermissionRequest, 1.1.7 dist). The classifier sees only kind=="execute",
	// which real shell approvals also carry, so correlation with the preceding rich frame
	// is the only way to identify the call at approval time. It is also the only place
	// the tool's NAME exists at that point.
	//
	// This is AUTHORIZATION state, and its lifecycle is deliberately stricter than the
	// display sets it sits beside:
	//   * keyed by MakeToolCallKey(acpSessionId, toolCallId) with BOTH parts required, so
	//     a record can never be consumed by a different ACP session;
	//   * bound to the adapter and the OS process generation that minted it, so it cannot
	//     survive a restart that reuses a call id;
	//   * consumed on the FIRST permission attempt for its key, whatever the outcome, so
	//     a malformed or denied attempt leaves nothing to replay;
	//   * dropped when the call is blocked, reaches a terminal status, or its adapter or
	//     session is swept.
	//
	// An earlier draft stored this per-chat and mirrored FChatSession::SeenToolCallIds,
	// on the reasoning that it would inherit that set's proven eviction. That was wrong:
	// SeenToolCallIds exists to stop a tool badge being drawn twice. Its eviction was
	// never meant to expire an authorization fact, and it is not cleared on adapter
	// restart — so evidence outlived the process that created it. A claim is not a badge.
	TMap<FString, FNwiroMcpToolClaim> NwiroMcpToolClaims;

	// Retire ALL permission-lifecycle state for an adapter and/or ACP session: B1 claims
	// and any un-clicked permission cards, together. Single entry point on purpose —
	// these two must never drift out of sync, and a caller that remembers one but not the
	// other leaves an approval that can still be answered for a context that is gone.
	// Either filter may be empty to mean "do not filter on this".
	void RetireAcpPermissionState(const FString& InAdapterId, const FString& InAcpSessionId);

	// True only while this ACP session is still mapped AND its chat still claims it.
	// Authorization asks this; display routing does not.
	bool IsAcpSessionLive(const FString& InAcpSessionId) const;

	// The complete origin-ownership predicate for a permission request, and the narrower
	// "is the issuing process still live" used to decide whether a wire response is worth
	// sending. Both live here because only the bridge holds every side of the relationship.
	bool IsPermissionOriginLive(const FAcpPermissionRequest& Request) const;
	bool IsPermissionOriginProcessLive(const FAcpPermissionRequest& Request) const;
	TMap<FString, FTerminalResult> TerminalResults;
	// Phase 2 — per-command shell approvals (one-shot, TTL). Council #2: bind approval
	// to the exact normalized command hash and verify it at the terminal/create exec
	// site. hash → expiry seconds (FPlatformTime::Seconds). Created by the per-command
	// modal (Phase 3); consumed (removed) on use at create.
	TMap<FString, double> ApprovedShellCommandExpiry;
	FCriticalSection ShellApprovalCS; // guards ApprovedShellCommandExpiry (council #2 thread-safety)
	void ApproveShellCommand(const FString& NormalizedHash);    // Phase 3: on user Allow
	bool HasValidShellApproval(const FString& NormalizedHash);  // peek (non-consuming) — Phase 2 gate
	bool ConsumeShellApproval(const FString& NormalizedHash);   // one-shot consume — Phase 4 at spawn

	// ─── codex-acp-string-id: ACP permission round-trip ─────────────────────
	// React's permission card needs a stable int32 id (UFUNCTION contract).
	// ACP request ids may be Number | String | Null on the wire, so we mint a
	// local int32 LocalPermId for each card surfaced to React and remember the
	// original ACP id here. RespondToPermission(int32) does map lookup; if
	// missing we fall through to the in-process MCP server.
	//
	// LocalPermId mints in the NEGATIVE int range (start -1, decrement) so the
	// ACP id space is permanently disjoint from MCP's NextPermissionId (>=1000).
	// A stale map entry from a long-running session can never collide with a
	// fresh MCP click at the same integer.
	int32 NextLocalPermId = -1;

	// Owns every un-clicked permission card and every terminal outcome. See
	// NwiroIKPermissionCoordinator.h — the bridge and the automation spec drive this same
	// object, so the tests assert on what production actually emits.
	FAcpPermissionCoordinator PermissionCoordinator;
	void ConfigurePermissionCoordinator();

	// Settings
	FString CurrentAdapter = TEXT("claude");
	FString CurrentMode;
	FString CurrentModel;
	// Attachments staged natively, KEYED BY CHAT.
	//
	// One shared array leaked across conversations: open the picker in chat A, switch to B
	// while the modal file dialog is up, and A's photo was staged into whatever the next
	// send happened to be. Keying by chat makes the staging follow the same rule as the
	// renderer's composer state — ownership decided where the user acted.
	// Drained one-shot by DoSendPrompt for the chat that is sending.
	TMap<FString, TArray<FString>> PendingImagesByChat;
	TMap<FString, bool> ActiveChatExtensions; // extension name → enabled for active chat
	bool bBlockCommandExecution = true; // safety default: block shell commands from adapters

	// localllm adapter's apiKey. NEVER LOG. NEVER SERIALIZE. Set via
	// SetLocalLlmApiKey UFUNCTION; injected as env var at FInteractiveProcess
	// spawn time and cleared from process env immediately after.
	FString LocalLlmApiKey;

	// localllm adapter's endpoint config. Stored from SetAdapterContext's
	// localLlm sub-object; injected into DoInitialize's `params.localLlm`
	// so the spawned shim receives them on its first ACP initialize request.
	// Empty string means the field was absent from the context JSON.
	FString LocalLlmBaseUrl;
	FString LocalLlmModel;
	// User-configurable warmup load-request timeout (seconds, as a string; "0" =
	// unbounded). Passed to the shim via NWIRO_LOCAL_LLM_WARMUP_TIMEOUT_SECS on
	// spawn so a slow cold-start model (large GGUF / serverless) isn't capped at
	// the shim's 300s default. Empty = absent (shim uses its default). Applies on
	// the next adapter spawn/restart.
	FString LocalLlmWarmupTimeoutSecs;

	// User-configurable ACP-stage timeout overrides (seconds; 0.0 = unset →
	// fall back to the NWIRO_*_TIMEOUT_SECONDS env var, then the built-in
	// default). Set from SetAdapterContext's localLlm sub-object
	// (firstToken/sessionNew/initialize TimeoutSecs). Consumed by the
	// Get*TimeoutSeconds() resolvers in CheckAdapterTimeouts. first-token is the
	// one that actually gates a slow local model (it raises the per-adapter
	// first-token ceiling); sessionNew/initialize are exposed for parity with
	// the env vars and rare edge cases (slow binary spawn / proxies) — the
	// shim's initialize + session/new handlers are synchronous so they almost
	// never bind. Unlike the warmup value these are consumed IN-PROCESS (the
	// getters are static-cached at first read, so an env var alone can't be
	// changed mid-session — the override member is the live, UI-settable path).
	double FirstTokenTimeoutOverrideSecs = 0.0;
	double SessionNewTimeoutOverrideSecs = 0.0;
	double InitializeTimeoutOverrideSecs = 0.0;

	// Tool-call capability tier from the most recent warmup result (the
	// `toolTier` field on the wire; value one of native/emulated/none). Empty
	// until the first warmup completes => treated as emulated (the conservative
	// small-model default). Persists across turns (warmup is adapter-scoped)
	// until the next warmup overwrites it. Consumed by the localllm
	// ToolSelector in DoSendPrompt to size the pushed `tools` array per tier
	// (native = full set / no-op, emulated = capped, none = no tools).
	FString LocalLlmToolTier;

	// Preambles pushed by the React app (which fetches them from the backend
	// /chat/prompts endpoint). ExecutePreamble drives the system prompt for
	// session/new (Claude) and the per-turn prepend (Codex, since codex-acp
	// drops the `instructions` field). PlanPreamble is prepended to every
	// session/prompt while CurrentMode == "plan" so the model knows it must
	// describe a plan from knowledge rather than calling tools. If both are
	// empty (e.g. backend unreachable AND no app push) we fall back to a
	// baked-in universal default in DoCreateSession.
	FString CurrentExecutePreamble;
	FString CurrentPlanPreamble;

	// Response queue
	TArray<FString> ResponseQueue;
	FCriticalSection ResponseLock;

	// ACP protocol
	void EnsureProcess();
	void KillProcess();
	int32 SendRpc(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params = nullptr);
	void SendRpcNotification(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params);
	// Outbound bridge-originated responses (to bridge-minted numeric ids).
	void SendRpcResult(const FString& AdapterId, int32 Id, const TSharedPtr<FJsonObject>& Result);
	// Outbound responses to inbound agent-originated requests — preserves the
	// agent's original wire-type for the id (Number | String | Null).
	void SendRpcResult(const FString& AdapterId, const Acp::FRequestId& ReqId, const TSharedPtr<FJsonObject>& Result);
	void SendRpcError(const FString& AdapterId, const Acp::FRequestId& ReqId, int32 Code, const FString& Message);
	void RespondToAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId, const FString& OptionId);
	void SendRaw(const FString& AdapterId, const FString& Json);
	void ProcessLine(const FString& AdapterId, const FString& Line);
	void HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error);
	void HandleMethod(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& FullMsg);
	// adapter-reliability-w1 codex-review-fix-2: AdapterId is threaded so the
	// rejected-session check can scope to the right adapter's set. Without
	// it we'd have to walk every adapter or keep a bridge-wide set that
	// never gets cleaned up.
	void HandleSessionUpdate(const FString& AdapterId, const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update);

	void DoInitialize();
	// adapter-reliability-w2: bAttachMcp gates session-attached MCP adapters.
	// Codex deliberately leaves session/new.mcpServers empty and receives the
	// endpoint from the ACP process's CODEX_CONFIG instead.
	void DoCreateSession(
		const FString& ChatId,
		bool bAttachMcp = true);
	// adapter-reliability-w6: session/load counterpart to DoCreateSession.
	// Resumes Session.PriorSessionId on adapters that advertise loadSession.
	void DoLoadSession(const FString& ChatId);
	// adapter-reliability-w6: decides between resume (DoLoadSession) and fresh
	// create (DoCreateSession) for a chat. Single entry point for every session
	// kickoff site; reads the adapter cap + the chat's resume state itself.
	void TryCreateOrLoadSession(const FString& ChatId);
	void DoSendPrompt(const FString& ChatId, const FString& Message);
	void DispatchPromptParams(const FString& ChatId, const TSharedPtr<FJsonObject>& Params);
	void ClearCodexModelBarrier(FChatSession& Session);

	// adapter-reliability-w2: kill + reset an adapter process so the next
	// EnsureProcess respawn fires a clean initialize. Called from the
	// session_new_timeout handler before the MCP-isolation retry; the OnCompleted
	// TWeakPtr guard prevents the old process's completion callback from
	// touching the new entry. Increments AP->RestartCount and clears the
	// per-adapter rejected-session set so the new process starts in a fresh
	// id space. Also wrapped by the public UFUNCTION RestartAdapter (above)
	// for the user-action self-healing button in Wave 4B.
	void DoRestartAdapter(const FString& AdapterId, const FString& Reason);

	// adapter-reliability-w2 codex-review-fix: shared cleanup for any path
	// that fails a chat (timeouts, classified stdout, process exit). Without
	// this, FailChatWithAdapterError sets bProcessing=false but leaves
	// SessionRpcId/PromptRpcId/timestamps populated — the timeout ticker
	// could later fire a second classified error, or DoCreateSession's
	// re-entry guard would silently no-op a retry. Does NOT clear
	// PendingMessage; callers decide based on whether they're retrying or
	// failing terminally.
	void ClearChatRpcState(FChatSession& Session);

	// adapter-reliability-w0: stage tracking + classified error events.
	// SetAdapterStage is idempotent on the active stage so per-chunk callers
	// (e.g. each agent_message_chunk) don't spam logs. FailChatWithAdapterError
	// records the failure on the session and emits both a rich adapter_error
	// event (for the new classified UI) and the legacy error/done pair (so
	// older renderers still leave the "thinking..." state).
	void SetAdapterStage(const FString& AdapterId, const FString& ChatId, const FString& Stage);
	void FailChatWithAdapterError(const FString& ChatId, const FString& AdapterId,
		const FString& Stage, const FString& Code, const FString& Message);

	// adapter-reliability-w1: per-RPC timeout enforcement. EnsureTimeoutTickerStarted
	// is idempotent and called from EnsureProcess so the ticker only exists
	// once a real adapter is running. CheckAdapterTimeouts walks every adapter
	// process + chat session each second and fires FailChatWithAdapterError
	// for any RPC older than its threshold (defaults: 60s init, 30s session/new,
	// 180s first token; each tunable via NWIRO_*_TIMEOUT_SECONDS env vars).
	// On timeout it also clears stuck SessionRpcId/PromptRpcId
	// so retries are not blocked by the in-flight guards.
	//
	// build-fix: must be FTSTicker::FDelegateHandle, NOT the generic
	// FDelegateHandle — modern UE separated them and there's no implicit
	// conversion from one to the other. The original (wrong) typing compiled
	// against UE 5.6 and earlier where the types were aliased.
	FTSTicker::FDelegateHandle TimeoutTickerHandle;
	void EnsureTimeoutTickerStarted();
	void CheckAdapterTimeouts();

	// Effective stage-timeout resolvers: member override (>0) → env var → default.
	// Members so they can read the *TimeoutOverrideSecs fields set from context;
	// the env/default fallback is static-cached inside each (read once per run).
	double GetInitializeTimeoutSeconds() const;
	double GetSessionNewTimeoutSeconds() const;
	double GetFirstTokenTimeoutSeconds() const;

	// mcp-health: liveness check + auto-rebind of the embedded MCP server, driven
	// by the existing 1s CheckAdapterTimeouts ticker (game thread → no locking).
	// Hysteresis + exponential backoff prevent thrash; never rebinds on one blip.
	void CheckMcpServerHealth();
	void RestartCodexForMcpPortChangeIfNeeded();
	int32  McpConsecutiveFailures = 0;
	double McpNextHealthCheckAt = 0.0;
	static constexpr int32 kMcpFailuresBeforeRebind = 3; // ~3s confirmed-down before first rebind
	static constexpr int32 kMcpMaxBackoffShift = 6;      // cap backoff at 2^6 = 64s

	FString FindAdapterBinary() const;
	FChatSession* GetActiveSession();
	FChatSession* FindSessionByAcpId(const FString& AcpSessionId);
	FString FindChatIdByRpcId(int32 RpcId);

	static FString JsonToString(const TSharedPtr<FJsonObject>& Obj);
};
