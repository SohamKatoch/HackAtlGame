// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKBridge.h"
#include "NwiroIKPermissionCoordinator.h"
#include "NwiroIKMCPServer.h"
#include "NwiroIKToolRegistry.h"
#include "NwiroIKShellPolicy.h"
#include "NwiroIKPanel.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeLock.h"

// Engine — Core
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Json.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

// Engine — Editor / Slate
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "SWebBrowser.h"

// Engine — HTTP
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// Engine — Assets
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"

// Engine — Editor state probe (localllm ToolSelector context gate). These back
// ProbeContextCategory(): we read the focused asset editor's edited-asset class
// and the level selection to pre-filter the pushed tool array by context. All
// are cheap pointer/UClass checks — no asset loads on the prompt-send path.
#include "Editor.h"
#include "Engine/Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"

// Engine — Scripting
#include "IPythonScriptPlugin.h"

// Nwiro — Zip extraction (miniz-based, no PowerShell dependency)
#include "ContentPipeline/NwiroIKZipExtractor.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
	#include <jobapi2.h>
	#include <processthreadsapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

/** Cross-platform "is this path a real file we could exec?". UE's
 *  FPaths::FileExists relies on stat() through a normaliser that has been
 *  observed to miss symlinks like `~/.local/bin/claude` on macOS (the
 *  claude installer always lays the CLI down as a symlink). access(F_OK)
 *  follows symlinks and matches what posix_spawn will actually try to do. */
static bool NwiroPathExists(const FString& Path)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	const FTCHARToUTF8 Conv(*Path);
	return access(Conv.Get(), F_OK) == 0;
#else
	return FPaths::FileExists(Path);
#endif
}

// Minimal JSON string escape for embedding diagnostic text into the
// PushEvent JSON payloads. Filesystem paths on Windows carry backslashes,
// and miniz / FFileHelper error messages can contain quotes and newlines —
// without escaping, the frontend's JSON.parse would silently swallow the
// event (matching the very bug we're fixing). Not a general-purpose JSON
// encoder; covers the characters that actually appear in our error text.
static FString NwiroJsonEscape(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Out;
}

// ─── Adapter timeout overrides (env + live UI override) ───────────────────
// Three stage timeouts (INITIALIZE / SESSION_NEW / FIRST_TOKEN) used by
// CheckAdapterTimeouts(). Resolution order is: per-instance override member
// (set live from the Local LLM settings via SetAdapterContext) → OS env var →
// built-in default. The env var path requires NO plugin rebuild:
//
//   setx NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS 240   (Windows, persistent)
//   $env:NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS = "240"   (Windows, current shell)
//   export NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS=240    (POSIX)
//
// The env/default value is cached on first read via static-local — thread-safe
// per C++17 §6.7/4 and free of synchronisation cost on the hot path. Trade-off
// of the env path: it must be set before UE launches; changing it mid-session
// has no effect until restart. That is exactly why the UI override exists: the
// *TimeoutOverrideSecs members ARE consulted live every tick, so a user can
// raise a timeout for a slow local model without an env var or a restart. The
// cached env value is logged once per session so deployments are auditable.
namespace
{
	static double ReadTimeoutEnvSeconds(const TCHAR* EnvName, const TCHAR* DisplayName, double DefaultSeconds)
	{
		const FString S = FPlatformMisc::GetEnvironmentVariable(EnvName);
		double V = S.IsEmpty() ? DefaultSeconds : FCString::Atod(*S);
		if (V <= 0.0) V = DefaultSeconds;
		UE_LOG(LogTemp, Log, TEXT("Nwiro IK: %s = %.1fs (env %s=%s)"),
			DisplayName, V, EnvName,
			S.IsEmpty() ? TEXT("<unset → default>") : *S);
		return V;
	}

	// Read an optional timeout field (seconds) from a JSON object, accepting a
	// number OR a numeric string. Returns 0.0 when absent/blank/invalid, which
	// the resolvers below treat as "unset → use env/default". Negatives clamp to
	// 0 (unset) rather than disabling the watchdog — a typo must never strand a
	// chat forever.
	static double ReadOptionalTimeoutSecsField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		if (!Obj.IsValid()) return 0.0;
		double Num = 0.0;
		if (Obj->TryGetNumberField(Field, Num)) return FMath::Max(0.0, Num);
		FString S;
		if (Obj->TryGetStringField(Field, S) && !S.IsEmpty()) return FMath::Max(0.0, FCString::Atod(*S));
		return 0.0;
	}
}

// Effective stage timeouts: live override member (>0) wins, else the static-
// cached env/default. Members (not free functions) so they can read the
// *TimeoutOverrideSecs fields populated from the localLlm context.
double UNwiroIKBridge::GetInitializeTimeoutSeconds() const
{
	if (InitializeTimeoutOverrideSecs > 0.0) return InitializeTimeoutOverrideSecs;
	static const double V = ReadTimeoutEnvSeconds(
		TEXT("NWIRO_INITIALIZE_TIMEOUT_SECONDS"), TEXT("INITIALIZE_TIMEOUT"), 60.0);
	return V;
}
double UNwiroIKBridge::GetSessionNewTimeoutSeconds() const
{
	if (SessionNewTimeoutOverrideSecs > 0.0) return SessionNewTimeoutOverrideSecs;
	static const double V = ReadTimeoutEnvSeconds(
		TEXT("NWIRO_SESSION_NEW_TIMEOUT_SECONDS"), TEXT("SESSION_NEW_TIMEOUT"), 30.0);
	return V;
}
double UNwiroIKBridge::GetFirstTokenTimeoutSeconds() const
{
	if (FirstTokenTimeoutOverrideSecs > 0.0) return FirstTokenTimeoutOverrideSecs;
	// Default 300s matches the historical hardcoded localllm first-token ceiling
	// (NOT the old, dead 180s env default), so removing the shadowing redeclaration
	// at the call site is a behavioural no-op when the user hasn't set anything.
	static const double V = ReadTimeoutEnvSeconds(
		TEXT("NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS"), TEXT("FIRST_TOKEN_TIMEOUT"), 300.0);
	return V;
}

// ════════════════════════════════════════════════════════════════════════════
// localllm ToolSelector — BM25 + static context gate + pinlist (council design)
// ════════════════════════════════════════════════════════════════════════════
//
// Purpose: the localllm push path in DoSendPrompt() pushes the full ~220-tool
// OpenAI `tools` array to the shim, which populates its tool_names allow-list
// from exactly that array. Small Emulated-tier models (GLM-4-9B / Qwen3-14B
// class) suffer "schema bleed" when handed 220 schemas and stop calling tools
// reliably; the shim also warns past ~50 tools. This selector caps the pushed
// array per the warmup-reported tool tier, while keeping the Native tier a
// strict byte-for-byte no-op (full set passes through unchanged).
//
// Pipeline (all synchronous, game-thread, on the prompt-send hot path BUT
// before SendRpc — i.e. outside the first-token latency window):
//   (1) TIER GATE     — ResolveToolBudget maps the warmup toolTier value to N.
//                       native => -1 sentinel (caller skips selection entirely).
//   (2) PIN           — a fixed <=8 pinlist is force-included first (a floor),
//                       so universally-needed verbs are present every turn
//                       regardless of query/context (the shim makes a dropped
//                       tool HARD un-callable, so pinning carries recall).
//   (3) CONTEXT GATE  — ProbeContextCategory() reads the focused asset editor /
//                       level selection and maps to a developer-authored tool
//                       group; this is the PRIMARY recall filter for the
//                       semantic gap BM25 can't cover ("make it shiny" with a
//                       material open => material tools regardless of words).
//                       UNKNOWN context => rank over the full unpinned set.
//   (4) RANK          — BM25 over the gated candidate subset fills remaining
//                       slots. Honest caveat: over ~220 short docs IDF is
//                       near-degenerate so BM25 ≈ weighted keyword overlap; it
//                       is a tiebreaker within a coherent subset, NOT the
//                       load-bearing recall mechanism (the gate + pinlist are).
// No edge-placement: each schema is emitted EXACTLY once (the shim's parser
// dedups/forbids duplicate function names, and a duplicate would corrupt the
// allow-list). A CVar-gated debug dump traces (pinned/gated/ranked/dropped) per
// turn so "why was tool X excluded" is answerable from one log line.
//
// THREAD-SAFETY: the module-scope BM25 cache is mutated without a lock; this is
// safe ONLY because DoSendPrompt (the sole caller) runs on the UE game thread.
// GetOrBuildBm25Index() asserts IsInGameThread() to make that invariant
// load-bearing and self-documenting.
namespace NwiroToolSelector
{
	// ── (1) Tier → budget ───────────────────────────────────────────────────
	// Returns -1 for the no-op (native) path, else the per-turn tool cap.
	// Tier is checked FIRST so the native path never pays the env read/syscall
	// (matches the "strict no-op" requirement). The env value is cached once on
	// first read — consistent with ReadTimeoutEnvSeconds above: env overrides
	// only take effect until the next editor restart, so re-reading per turn is
	// both wasteful and inconsistent with the established convention.
	static int32 GetToolBudgetEnvOverride()
	{
		// Cached via static-local + lambda IIFE (thread-safe per C++17 §6.7/4).
		// 0 means "unset or non-positive" => no override.
		static const int32 V = []() -> int32
		{
			const FString S = FPlatformMisc::GetEnvironmentVariable(TEXT("NWIRO_TOOL_BUDGET"));
			if (S.IsEmpty()) { return 0; }
			const int32 Parsed = FCString::Atoi(*S);
			if (Parsed > 0)
			{
				UE_LOG(LogTemp, Log,
					TEXT("Nwiro IK: NWIRO_TOOL_BUDGET = %d (overrides emulated-tier default)"),
					Parsed);
				return Parsed;
			}
			return 0;
		}();
		return V;
	}

	// Pure budget resolver — NO process-global env read. `Override` is the
	// already-resolved NWIRO_TOOL_BUDGET value (0 == unset/non-positive). The
	// `none` -> 0 check stays BEFORE the override check so a none-tier strip can
	// never be resurrected back into tools. Extracted for unit testing (P0-G B11):
	// the spec injects `Override` directly, never touching the magic-static cache.
	static int32 ResolveToolBudgetImpl(const FString& ToolTier, int32 Override)
	{
		// Tier value is snake_case on the wire ('native'/'emulated'/'none').
		const FString Tier = ToolTier.ToLower();
		if (Tier == TEXT("none")) { return 0; } // shim strips tools anyway

		// Operator cap applies to ALL tool-using tiers (native + emulated). It can
		// no longer "resurrect" tools on none's 0 (handled above).
		if (Override > 0) { return Override; }

		// v0.2.x: native is NO LONGER a no-op (was `return -1`, which skipped
		// SelectTools entirely and shipped the full ~220-tool surface every turn).
		// Over-exposing even a STRONG native model degrades it: gpt-oss-120b
		// empirically calls a buried tool fine among ~200 tool NAMES, but on the
		// real ~25K-token rich-schema set it confabulates across a multi-turn chat
		// ("I don't have create_blueprint" though it is present). Run the SAME
		// pin + context-gate + BM25 selection as emulated, with a GENEROUS cap that
		// respects native's higher capacity — a big cut from ~220, still
		// relevance-complete via the context gate + pinlist. Tunable via
		// NWIRO_TOOL_BUDGET. Rationale: local-llm-acp docs/TOOL-SURFACE-OVERLOAD.md.
		if (Tier == TEXT("native")) { return 64; }

		// 'emulated' OR empty (no warmup yet) => conservative small-model cap.
		return 24; // NWIRO_TOOL_BUDGET default; untunable without labelled data.
	}

	// Thin wrapper: reads the once-cached env override, then delegates to the pure
	// impl. The production call site (DoSendPrompt) is UNCHANGED. (P0-G testability seam.)
	static int32 ResolveToolBudget(const FString& ToolTier)
	{
		return ResolveToolBudgetImpl(ToolTier, GetToolBudgetEnvOverride());
	}

	// ── (2) Tokenizer — snake_case + camelCase + digit boundaries ────────────
	// Splits on three boundaries and lowercases: (a) any non-alphanumeric char
	// is a delimiter (so 'spawn_actor' -> [spawn, actor], plus '.', '/', space);
	// (b) a lower->Upper transition inside a run (camelCase/PascalCase); (c) a
	// letter->digit transition. No stemming / stopwords — corpus is too small to
	// benefit and this keeps it deterministic and dependency-free.
	static void TokenizeToolText(const FString& In, TArray<FString>& OutTokens)
	{
		FString Cur;
		auto Flush = [&OutTokens, &Cur]()
		{
			if (!Cur.IsEmpty()) { OutTokens.Add(Cur.ToLower()); Cur.Reset(); }
		};
		TCHAR Prev = 0;
		for (const TCHAR* P = *In; *P; ++P)
		{
			const TCHAR C = *P;
			if (!FChar::IsAlnum(C)) { Flush(); Prev = C; continue; } // delimiter
			const bool bBoundary =
				(FChar::IsUpper(C) && Prev != 0 && FChar::IsLower(Prev)) || // camelCase
				(FChar::IsDigit(C) && Prev != 0 && FChar::IsAlpha(Prev));   // word->digit
			if (bBoundary) { Flush(); }
			Cur.AppendChar(C);
			Prev = C;
		}
		Flush();
	}

	// ── (3) BM25 index — module-init IDF + per-doc term frequencies ──────────
	// Precomputed ONCE per unique tool set, cached at module scope, keyed by a
	// content hash of the tool names so it rebuilds only when the registry
	// changes (the IK compile-gate can flip the count 220<->208). k1/b are
	// hardcoded and untunable here without a labelled query->tool dataset.
	struct FBm25Index
	{
		TArray<TMap<FString, int32>> DocTf; // per tool: token -> raw count
		TArray<int32>                DocLen; // per tool: total token count
		TMap<FString, int32>         DocFreq; // token -> #docs containing it
		float  AvgDocLen = 0.f;
		int32  NumDocs   = 0;
		uint32 SourceHash = 0;              // rebuild trigger (220 vs 208)
		static constexpr float K1 = 1.2f;   // UNTUNABLE here (no labelled data)
		static constexpr float B  = 0.75f;
	};

	static FBm25Index GBm25Index; // module-scope cache (game-thread only)

	static const FBm25Index& GetOrBuildBm25Index(const TArray<TSharedPtr<FJsonValue>>& McpTools)
	{
		// The cache is mutated without a lock; assert the single-thread
		// invariant the design relies on (DoSendPrompt is game-thread).
		check(IsInGameThread());

		uint32 H = 0;
		for (const TSharedPtr<FJsonValue>& V : McpTools)
		{
			if (V.IsValid() && V->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				FString N;
				if (O.IsValid() && O->TryGetStringField(TEXT("name"), N))
				{
					H = HashCombine(H, GetTypeHash(N));
				}
			}
		}
		if (GBm25Index.SourceHash == H && GBm25Index.NumDocs > 0)
		{
			return GBm25Index; // cache hit — no rebuild on the per-turn path
		}

		FBm25Index Ix;
		Ix.SourceHash = H;
		Ix.NumDocs = McpTools.Num();
		Ix.DocTf.SetNum(Ix.NumDocs);
		Ix.DocLen.SetNumZeroed(Ix.NumDocs);
		int64 TotalLen = 0;
		for (int32 i = 0; i < Ix.NumDocs; ++i)
		{
			const TSharedPtr<FJsonObject> O =
				McpTools[i].IsValid() ? McpTools[i]->AsObject() : nullptr;
			if (!O.IsValid()) { continue; }
			FString Name, Desc;
			O->TryGetStringField(TEXT("name"), Name);
			O->TryGetStringField(TEXT("description"), Desc);
			TArray<FString> Toks;
			TokenizeToolText(Name + TEXT(" ") + Desc, Toks);
			Ix.DocLen[i] = Toks.Num();
			TotalLen += Toks.Num();
			TSet<FString> Seen;
			for (const FString& T : Toks)
			{
				Ix.DocTf[i].FindOrAdd(T)++;
				bool bAlready = false;
				Seen.Add(T, &bAlready);
				if (!bAlready) { Ix.DocFreq.FindOrAdd(T)++; }
			}
		}
		Ix.AvgDocLen = Ix.NumDocs > 0 ? (float)TotalLen / (float)Ix.NumDocs : 0.f;
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: ToolSelector built BM25 index — %d docs, avgLen=%.1f, %d unique terms"),
			Ix.NumDocs, Ix.AvgDocLen, Ix.DocFreq.Num());
		GBm25Index = MoveTemp(Ix);
		return GBm25Index;
	}

	// Okapi BM25 with the +1 (plus-one) idf variant so idf is never negative —
	// important here because common verbs like 'create'/'set' appear in many of
	// the ~220 docs and the classic idf would go negative. Reads only the
	// precomputed maps; no recomputation per turn.
	static float ScoreBm25(const FBm25Index& Ix, const TArray<FString>& QueryTokens, int32 Doc)
	{
		if (!Ix.DocTf.IsValidIndex(Doc) || Ix.AvgDocLen <= 0.f) { return 0.f; }
		const TMap<FString, int32>& Tf = Ix.DocTf[Doc];
		const float DocLenF = (float)Ix.DocLen[Doc];
		float Score = 0.f;
		for (const FString& T : QueryTokens)
		{
			const int32* FPtr = Tf.Find(T);
			if (!FPtr) { continue; } // term absent from this doc
			const float F = (float)(*FPtr);
			const int32 Df = Ix.DocFreq.FindRef(T); // 0 if unseen in corpus
			const float Idf = FMath::Loge(
				((float)Ix.NumDocs - (float)Df + 0.5f) / ((float)Df + 0.5f) + 1.0f);
			const float Denom = F + FBm25Index::K1 *
				(1.0f - FBm25Index::B + FBm25Index::B * DocLenF / Ix.AvgDocLen);
			const float TfComp = (Denom > 0.f) ? (F * (FBm25Index::K1 + 1.0f)) / Denom : 0.f;
			Score += Idf * TfComp;
		}
		return Score;
	}

	// ── (4) Static context-category gate (PRIMARY filter) ────────────────────
	enum class EToolContextCategory : uint8
	{
		Unknown, Material, Blueprint, Level, Sequencer, Niagara, Animation, Pcg
	};

	// True if Cls or any superclass is named one of NamePrefixes (case-sensitive
	// UClass name match, e.g. "NiagaraSystem"). Used for asset types whose
	// concrete headers we deliberately do NOT include to keep the probe header-
	// light; walking the UClass chain is a cheap pointer hop.
	static bool ClassChainHasName(const UClass* Cls, TConstArrayView<const TCHAR*> Names)
	{
		for (const UClass* C = Cls; C; C = C->GetSuperClass())
		{
			const FString CName = C->GetName();
			for (const TCHAR* N : Names)
			{
				if (CName == N) { return true; }
			}
		}
		return false;
	}

	static EToolContextCategory ProbeContextCategory()
	{
		if (!GEditor) { return EToolContextCategory::Unknown; }

		// 1) Focused asset editor wins (most specific). Cheap: no asset load —
		//    GetAllEditedAssets returns already-open UObject*s.
		if (UAssetEditorSubsystem* AE = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			const TArray<UObject*> Edited = AE->GetAllEditedAssets();
			for (const UObject* O : Edited)
			{
				if (!O) { continue; }
				const UClass* Cls = O->GetClass();
				// Material uses concrete IsA (light, common headers).
				if (O->IsA(UMaterialInterface::StaticClass())
					|| O->IsA(UMaterialFunctionInterface::StaticClass()))
				{
					return EToolContextCategory::Material;
				}
				// Specific class-chain matches MUST precede the generic UBlueprint
				// check below: UAnimBlueprint derives from UBlueprint, so an open
				// AnimBP would otherwise be misrouted to the generic Blueprint group
				// and miss its anim tools.
				if (ClassChainHasName(Cls, { TEXT("MovieSceneSequence"), TEXT("LevelSequence") }))
				{
					return EToolContextCategory::Sequencer;
				}
				if (ClassChainHasName(Cls, { TEXT("NiagaraSystem"), TEXT("NiagaraEmitter") }))
				{
					return EToolContextCategory::Niagara;
				}
				if (ClassChainHasName(Cls, { TEXT("AnimBlueprint"), TEXT("AnimMontage"),
					TEXT("BehaviorTree"), TEXT("BlackboardData"), TEXT("StateTree"),
					TEXT("IKRigDefinition"), TEXT("IKRetargeter"), TEXT("PoseSearchDatabase") }))
				{
					return EToolContextCategory::Animation;
				}
				if (ClassChainHasName(Cls, { TEXT("PCGGraph") }))
				{
					return EToolContextCategory::Pcg;
				}
				// Generic Blueprint LAST (base class of several specifics above).
				if (O->IsA(UBlueprint::StaticClass()))
				{
					return EToolContextCategory::Blueprint;
				}
			}
		}

		// 2) Else fall back to level selection (any selected actor => Level).
		if (USelection* Sel = GEditor->GetSelectedActors())
		{
			if (Sel->Num() > 0) { return EToolContextCategory::Level; }
		}
		return EToolContextCategory::Unknown; // cold-start => full unpinned set
	}

	static const TCHAR* CategoryName(EToolContextCategory Cat)
	{
		switch (Cat)
		{
		case EToolContextCategory::Material:   return TEXT("Material");
		case EToolContextCategory::Blueprint:  return TEXT("Blueprint");
		case EToolContextCategory::Level:      return TEXT("Level");
		case EToolContextCategory::Sequencer:  return TEXT("Sequencer");
		case EToolContextCategory::Niagara:    return TEXT("Niagara");
		case EToolContextCategory::Animation:  return TEXT("Animation");
		case EToolContextCategory::Pcg:        return TEXT("Pcg");
		default:                               return TEXT("Unknown");
		}
	}

	// Developer-authored mapping (NOT query-derived) — deterministic &
	// debuggable. Every name below was validated against the live registry in
	// FNwiroIKMCPServer::GetEnabledToolDefinitionsJson(). ValidateToolNamesOnce()
	// re-checks them at first use and logs any that drift out of the registry.
	static void ResolveCategoryGroup(EToolContextCategory Cat, TSet<FName>& Out)
	{
		switch (Cat)
		{
		case EToolContextCategory::Material:
			Out.Append({
				TEXT("find_materials"), TEXT("inspect_material"), TEXT("create_material"),
				TEXT("edit_material"), TEXT("inspect_material_graph"), TEXT("set_material_property"),
				TEXT("create_material_instance"), TEXT("edit_material_instance"), TEXT("apply_material"),
				TEXT("delete_material"), TEXT("find_textures"), TEXT("find_material_functions"),
				TEXT("inspect_material_function"), TEXT("create_material_function"),
				TEXT("edit_material_function"), TEXT("delete_material_function"),
				TEXT("generate_material_fal"), TEXT("generate_texture_meshy"),
				TEXT("set_landscape_material") });
			break;
		case EToolContextCategory::Blueprint:
			Out.Append({
				TEXT("find_blueprints"), TEXT("read_blueprint"), TEXT("create_blueprint"),
				TEXT("edit_blueprint"), TEXT("find_blueprint_nodes"), TEXT("delete_node"),
				TEXT("create_function_graph"), TEXT("clear_graph"), TEXT("add_interface"),
				TEXT("remove_interface"), TEXT("create_event_dispatcher"), TEXT("reparent_blueprint"),
				TEXT("remove_component"), TEXT("edit_component"), TEXT("set_cdo_property"),
				TEXT("delete_blueprint"), TEXT("duplicate_blueprint"), TEXT("rename_blueprint"),
				TEXT("bp_get_compile_errors"), TEXT("bp_find_unconnected_pins"),
				TEXT("bp_fix_broken_references"), TEXT("bp_fix_deprecated_nodes"),
				TEXT("bp_refresh_all_nodes"), TEXT("bp_set_breakpoint"), TEXT("bp_remove_breakpoint"),
				TEXT("bp_list_breakpoints"), TEXT("bp_add_watch"), TEXT("bp_get_watch_values") });
			break;
		case EToolContextCategory::Level:
			Out.Append({
				TEXT("spawn_actor"), TEXT("delete_actor"), TEXT("transform_actor"),
				TEXT("get_actor_property"), TEXT("set_actor_property"), TEXT("duplicate_actor"),
				TEXT("rename_actor"), TEXT("attach_actor"), TEXT("detach_actor"),
				TEXT("select_actor"), TEXT("get_level_actors"), TEXT("get_level_info"),
				TEXT("apply_material"), TEXT("set_light_properties"), TEXT("set_fog"),
				TEXT("set_post_process"), TEXT("set_sky_atmosphere"), TEXT("create_spline_actor"),
				TEXT("add_spline_point"), TEXT("set_spline_point"), TEXT("paint_foliage"),
				TEXT("add_foliage_type"), TEXT("erase_foliage"), TEXT("spawn_sound"),
				TEXT("save_level"), TEXT("get_world_settings"), TEXT("set_world_settings") });
			break;
		case EToolContextCategory::Sequencer:
			Out.Append({
				TEXT("create_sequence"), TEXT("read_sequence"), TEXT("add_sequence_binding"),
				TEXT("add_sequence_track"), TEXT("add_sequence_keyframe"), TEXT("set_sequence_range"),
				TEXT("create_montage"), TEXT("read_montage"), TEXT("add_montage_section"),
				TEXT("link_montage_sections"), TEXT("add_montage_notify"), TEXT("spawn_actor"),
				TEXT("transform_actor"), TEXT("get_level_actors") });
			break;
		case EToolContextCategory::Niagara:
			Out.Append({
				TEXT("create_niagara_system"), TEXT("read_niagara_system"),
				TEXT("add_niagara_emitter"), TEXT("set_niagara_parameter"),
				TEXT("spawn_actor"), TEXT("transform_actor"), TEXT("execute_python") });
			break;
		case EToolContextCategory::Animation:
			Out.Append({
				TEXT("create_anim_blueprint"), TEXT("read_anim_blueprint"),
				TEXT("add_anim_bp_state_machine"), TEXT("create_montage"), TEXT("read_montage"),
				TEXT("add_montage_section"), TEXT("add_montage_notify"), TEXT("link_montage_sections"),
				TEXT("create_behavior_tree"), TEXT("read_behavior_tree"), TEXT("add_behavior_tree_nodes"),
				TEXT("create_blackboard"), TEXT("edit_blackboard"), TEXT("create_state_tree"),
				TEXT("read_state_tree"), TEXT("add_state_tree_state"), TEXT("create_ik_rig"),
				TEXT("read_ik_rig"), TEXT("add_ik_goal"), TEXT("add_ik_solver"),
				TEXT("create_ik_retargeter"), TEXT("read_ik_retargeter"), TEXT("add_retarget_chain"),
				TEXT("set_chain_mapping"), TEXT("create_pose_search_database"),
				TEXT("create_pose_search_schema"), TEXT("add_pose_search_animation") });
			break;
		case EToolContextCategory::Pcg:
			Out.Append({
				TEXT("create_pcg_graph"), TEXT("find_pcg_graphs"), TEXT("pcg_generate"),
				TEXT("spawn_pcg_volume"), TEXT("spawn_actor"), TEXT("execute_python") });
			break;
		default:
			break; // Unknown => caller uses the full unpinned set
		}
	}

	// ── Pinlist — force-included first, a floor against the budget ───────────
	// Universally-needed verbs that must be present EVERY turn regardless of
	// query or context, because the shim makes a dropped tool hard un-callable.
	// All validated against the live registry. write_file/delete_file/rename_file
	// are deliberately excluded (File-Editor-extension-gated, usually absent);
	// read_file WAS excluded for the same reason but is pinned since reads were
	// ungated (2026-07-10) — "open X" / "what does this class do" must never lose
	// read_file to the BM25 budget.
	// v0.2.x: the CREATION-verb class (create_blueprint/create_material) was added.
	// The pinlist pinned edit_* and spawn_* but OMITTED create_* — so a request
	// like "generate MySimpleBP" (no "blueprint" token) scored ~0 on BM25 and was
	// dropped. Pinning the whole creation class (not one tool) is the general fix.
	static const TArray<FName>& GetPinnedToolNames()
	{
		// Function-local static: C++11 magic-static, thread-safe init.
		static const TArray<FName> Pinned = {
			TEXT("execute_python"),    // spec-named; the universal escape hatch
			TEXT("spawn_actor"),       // gate's named example (actor creation)
			TEXT("transform_actor"),   // "move the thing" — paraphrastic recall
			TEXT("set_actor_property"),// generic property edits
			TEXT("create_blueprint"),  // creation-verb class — BM25-invisible to "MySimpleBP"
			TEXT("edit_blueprint"),    // high-frequency BP domain
			TEXT("create_material"),   // creation-verb class (mirrors create_blueprint)
			TEXT("edit_material"),     // "make it shiny" — paraphrastic recall
			TEXT("get_level_actors"),  // level introspection
			TEXT("take_screenshot"),   // "show me" — paraphrastic recall
			TEXT("read_file")          // ungated reads — code Q&A staple
		};
		return Pinned;
	}

	// ── Debug dump (CVar-gated) — answers "why was tool X dropped?" ──────────
	static TAutoConsoleVariable<int32> CVarToolSelectorDebug(
		TEXT("nwiro.ToolSelector.Debug"), 0,
		TEXT("When non-zero, logs the localllm ToolSelector decision per prompt ")
		TEXT("(category, pinned, kept count, and the dropped tool names)."),
		ECVF_Default);

	static void DumpToolSelection(
		const TArray<TSharedPtr<FJsonValue>>& McpTools,
		EToolContextCategory Cat,
		const TArray<FName>& Pinned,
		const TArray<int32>& KeepIndices)
	{
		if (CVarToolSelectorDebug.GetValueOnGameThread() == 0) { return; }

		TSet<int32> Kept(KeepIndices);
		// Build a pin-name string.
		FString PinStr;
		for (const FName& P : Pinned)
		{
			if (!PinStr.IsEmpty()) { PinStr += TEXT(", "); }
			PinStr += P.ToString();
		}
		// Collect dropped tool names (cap the list so the log line stays sane).
		FString DroppedStr;
		int32 DroppedCount = 0;
		for (int32 i = 0; i < McpTools.Num(); ++i)
		{
			if (Kept.Contains(i)) { continue; }
			++DroppedCount;
			if (DroppedCount <= 40)
			{
				FString N;
				if (McpTools[i].IsValid() && McpTools[i]->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> O = McpTools[i]->AsObject();
					if (O.IsValid()) { O->TryGetStringField(TEXT("name"), N); }
				}
				if (!DroppedStr.IsEmpty()) { DroppedStr += TEXT(", "); }
				DroppedStr += N;
			}
		}
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: ToolSelector ctx=%s total=%d kept=%d dropped=%d | pinned=[%s] | dropped=[%s%s]"),
			CategoryName(Cat), McpTools.Num(), KeepIndices.Num(), DroppedCount,
			*PinStr, *DroppedStr, (DroppedCount > 40) ? TEXT(", ...") : TEXT(""));
	}

	// ── Startup validation — detect drift in pinlist / category-group names ──
	// The context gate is the PRIMARY recall mechanism but its tool names are
	// unchecked string literals; a typo or a renamed tool is a SILENT recall
	// hole. This logs every pinlist/category name that is absent from the live
	// registry so the gaps are visible. Runs once per editor session (the first
	// time SelectTools sees a non-empty registry).
	static void ValidateToolNamesOnce(const TSet<FName>& RegistryNames)
	{
		static bool bValidated = false;
		if (bValidated) { return; }
		bValidated = true;

		auto CheckGroup = [&RegistryNames](const TCHAR* Label, const TSet<FName>& Group)
		{
			FString Missing;
			for (const FName& N : Group)
			{
				if (!RegistryNames.Contains(N))
				{
					if (!Missing.IsEmpty()) { Missing += TEXT(", "); }
					Missing += N.ToString();
				}
			}
			if (!Missing.IsEmpty())
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro IK: ToolSelector %s references tools NOT in the live registry ")
					TEXT("(silent recall hole): %s"), Label, *Missing);
			}
		};

		// Pinlist.
		{
			TSet<FName> PinSet(GetPinnedToolNames());
			CheckGroup(TEXT("pinlist"), PinSet);
		}
		// Each category group.
		const EToolContextCategory Cats[] = {
			EToolContextCategory::Material, EToolContextCategory::Blueprint,
			EToolContextCategory::Level, EToolContextCategory::Sequencer,
			EToolContextCategory::Niagara, EToolContextCategory::Animation,
			EToolContextCategory::Pcg };
		for (EToolContextCategory C : Cats)
		{
			TSet<FName> G;
			ResolveCategoryGroup(C, G);
			CheckGroup(CategoryName(C), G);
		}
	}

	// ── (5) Orchestrator — tier → pin → gate → BM25 → indices ────────────────
	// Returns surviving McpTools indices in final send order (pinned first in
	// declaration order, then context-gated + BM25-ranked in score-descending
	// order). The native (-1) path is handled by the caller before this runs.
	//
	// `Category` is INJECTED (not probed here) so unit tests (P0-G B11/B12/B13) can
	// drive each context branch deterministically without a live world/CVar probe.
	// The thin SelectTools() wrapper below supplies it via ProbeContextCategory().
	static void SelectToolsImpl(
		const FString& UserText,
		const TArray<TSharedPtr<FJsonValue>>& McpTools,
		int32 Budget,
		EToolContextCategory Category,
		TArray<int32>& OutKeepIndices)
	{
		OutKeepIndices.Reset();
		if (Budget <= 0 || McpTools.Num() == 0) { return; }

		// Pinned tools are a FLOOR: if an env override sets Budget below the
		// pinlist size, raise it so we never silently exceed Budget while still
		// guaranteeing the pinned verbs survive (the alternative — truncating
		// the pinlist — would drop a universally-needed, hard-to-recall verb).
		const TArray<FName>& Pinned = GetPinnedToolNames();
		Budget = FMath::Max(Budget, Pinned.Num());

		// name -> index lookup for pin + gate membership (O(1) per probe). Also
		// the registry-name set used for one-time drift validation.
		TMap<FString, int32> NameToIdx;
		NameToIdx.Reserve(McpTools.Num());
		TSet<FName> RegistryNames;
		RegistryNames.Reserve(McpTools.Num());
		for (int32 i = 0; i < McpTools.Num(); ++i)
		{
			if (!McpTools[i].IsValid() || McpTools[i]->Type != EJson::Object) { continue; }
			const TSharedPtr<FJsonObject> O = McpTools[i]->AsObject();
			if (!O.IsValid()) { continue; }
			FString N;
			if (O->TryGetStringField(TEXT("name"), N) && !N.IsEmpty())
			{
				NameToIdx.Add(N, i);
				RegistryNames.Add(FName(*N));
			}
		}

		ValidateToolNamesOnce(RegistryNames);

		// If the whole registry already fits the budget, keep everything in its
		// natural order (no scoring needed). Done AFTER the Budget floor so an
		// env override can't make this fire spuriously.
		if (McpTools.Num() <= Budget)
		{
			for (int32 i = 0; i < McpTools.Num(); ++i) { OutKeepIndices.Add(i); }
			DumpToolSelection(McpTools, EToolContextCategory::Unknown, Pinned, OutKeepIndices);
			return;
		}

		TSet<int32> Chosen;
		Chosen.Reserve(Budget);

		// (A) PINNED first — authoritative, never excluded. Each consumes a slot.
		for (const FName& P : Pinned)
		{
			if (const int32* Ix = NameToIdx.Find(P.ToString()))
			{
				if (!Chosen.Contains(*Ix))
				{
					Chosen.Add(*Ix);
					OutKeepIndices.Add(*Ix);
				}
			}
		}

		// (B) PRIMARY static context gate -> candidate subset. UNKNOWN context
		//     falls back to the full set (cold-start path). `Category` is the
		//     injected probe result (see the SelectToolsImpl seam note above).
		TSet<FName> GroupNames;
		ResolveCategoryGroup(Category, GroupNames);
		TArray<int32> Candidates;
		if (GroupNames.Num() > 0)
		{
			for (const FName& G : GroupNames)
			{
				if (const int32* Ix = NameToIdx.Find(G.ToString()))
				{
					Candidates.AddUnique(*Ix);
				}
			}
		}
		else
		{
			Candidates.Reserve(McpTools.Num());
			for (int32 i = 0; i < McpTools.Num(); ++i) { Candidates.AddUnique(i); }
		}

		// (C) BM25-rank the candidate subset (tiebreaker within a coherent set).
		const FBm25Index& Ix = GetOrBuildBm25Index(McpTools);
		TArray<FString> Q;
		TokenizeToolText(UserText, Q);
		TArray<TPair<int32, float>> Scored;
		Scored.Reserve(Candidates.Num());
		for (int32 Doc : Candidates)
		{
			if (Chosen.Contains(Doc)) { continue; } // already pinned
			Scored.Add(TPair<int32, float>(Doc, ScoreBm25(Ix, Q, Doc)));
		}
		// Descending by score; ties broken by lower index for determinism.
		Scored.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B)
		{
			if (A.Value != B.Value) { return A.Value > B.Value; }
			return A.Key < B.Key;
		});
		for (const TPair<int32, float>& S : Scored)
		{
			if (OutKeepIndices.Num() >= Budget) { break; }
			if (!Chosen.Contains(S.Key))
			{
				Chosen.Add(S.Key);
				OutKeepIndices.Add(S.Key);
			}
		}

		// (D) deterministic debug dump (CVar-gated).
		DumpToolSelection(McpTools, Category, Pinned, OutKeepIndices);
	}

	// Thin wrapper: probes the live context category, then delegates to the pure
	// impl. The production call site (DoSendPrompt) is UNCHANGED. (P0-G testability seam.)
	static void SelectTools(
		const FString& UserText,
		const TArray<TSharedPtr<FJsonValue>>& McpTools,
		int32 Budget,
		TArray<int32>& OutKeepIndices)
	{
		const EToolContextCategory Category = ProbeContextCategory();
		SelectToolsImpl(UserText, McpTools, Budget, Category, OutKeepIndices);
	}
} // namespace NwiroToolSelector

// P0-G Phase 1 — ToolSelector unit tests. Included into THIS translation unit
// (same TU as namespace NwiroToolSelector) so the spec can call the static
// ResolveToolBudgetImpl / SelectToolsImpl seams without any export or header.
// Compiled only in dev/automation builds.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroIKToolSelectorSpec.inl"
#endif

UNwiroIKBridge* UNwiroIKBridge::Instance = nullptr;

// ============================================================
// Adapter registry — add new adapters here
// ============================================================

struct FAdapterInfo
{
	FString Id;
	FString BinaryName;
	FString DownloadUrl;
	FString EnvKey;       // Environment variable to set (e.g. CLAUDE_CODE_EXECUTABLE)
	TArray<FString> ExeCandidates; // Paths to search for the underlying CLI
};

/** Parse "2.1.128" into [2, 1, 128]. Returns empty TOptional if the string isn't
 *  exactly three dot-separated non-negative integers. Pre-release suffixes
 *  ("2.1.0-rc1") and v-prefixed strings ("v2.1.0") are intentionally rejected
 *  — the Anthropic installer uses pure semver paths, anything else is unknown. */
static TOptional<TArray<int32>> ParseSemverParts(const FString& VersionStr)
{
	TArray<FString> Parts;
	VersionStr.ParseIntoArray(Parts, TEXT("."), true);
	if (Parts.Num() != 3) return {};
	TArray<int32> Result;
	Result.Reserve(3);
	for (const FString& P : Parts)
	{
		if (P.IsEmpty()) return {};
		for (TCHAR C : P) if (!FChar::IsDigit(C)) return {};
		Result.Add(FCString::Atoi(*P));
	}
	return Result;
}

namespace NwiroCodex
{
	static constexpr const TCHAR* PackageName = TEXT("@agentclientprotocol/codex-acp");
	static constexpr const TCHAR* PackageVersion = TEXT("1.1.7");
	static constexpr const TCHAR* AgentName = TEXT("@agentclientprotocol/codex-acp");
	static constexpr int32 ProtocolVersion = 1;
	static constexpr double ModelConfigTimeoutSeconds = 30.0;
	static constexpr double RuntimeProbeCacheSeconds = 30.0;

	struct FNodeToolchain
	{
		FString NodePath;
		FString NpmCliPath;
		FString NodeVersion;
		FString NpmVersion;
		bool bNodeSupported = false;
	};

	struct FLaunchSpec
	{
		FString Executable;
		FString Arguments;
		FString EntryPath;
		FString Source;
		FString Version;

		bool IsValid() const
		{
			return !Executable.IsEmpty() && !EntryPath.IsEmpty()
				&& NwiroPathExists(Executable) && NwiroPathExists(EntryPath);
		}
	};

	struct FModelCatalog
	{
		bool bHasModelOption = false;
		FString CurrentModel;
		TSet<FString> ModelIds;
	};

	struct FSelectConfigCatalog
	{
		bool bHasOption = false;
		FString CurrentValue;
		TSet<FString> Values;
	};

	struct FGameThreadBarrier
	{
		FEvent* Event = FPlatformProcess::GetSynchEventFromPool(true);
		FCriticalSection StateMutex;
		bool bStarted = false;
		bool bCancelled = false;
		bool bCompleted = false;

		bool TryBegin()
		{
			FScopeLock Lock(&StateMutex);
			if (bCancelled) return false;
			bStarted = true;
			return true;
		}

		// Returns true when the queued game-thread operation was cancelled
		// before it began. If it already began, the worker must wait for Event
		// rather than racing promotion against the in-flight quiesce.
		bool CancelIfPending()
		{
			FScopeLock Lock(&StateMutex);
			if (bStarted) return false;
			bCancelled = true;
			return true;
		}

		~FGameThreadBarrier()
		{
			if (Event) FPlatformProcess::ReturnSynchEventToPool(Event);
		}
	};

	static FString QuoteArg(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString FirstNonEmptyLine(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, true);
		for (FString Line : Lines)
		{
			Line = Line.TrimStartAndEnd().TrimQuotes();
			if (!Line.IsEmpty()) return Line;
		}
		return TEXT("");
	}

	static bool IsVersionAtLeast(const FString& Version, int32 Major, int32 Minor, int32 Patch)
	{
		FString Clean = Version.TrimStartAndEnd();
		if (Clean.StartsWith(TEXT("v"))) Clean.RightChopInline(1);
		const TOptional<TArray<int32>> Parsed = ParseSemverParts(Clean);
		if (!Parsed.IsSet()) return false;
		const TArray<int32>& V = Parsed.GetValue();
		if (V[0] != Major) return V[0] > Major;
		if (V[1] != Minor) return V[1] > Minor;
		return V[2] >= Patch;
	}

	static bool IsVersionAtLeast(const FString& Version, const FString& MinimumVersion)
	{
		const TOptional<TArray<int32>> ParsedVersion = ParseSemverParts(Version.TrimStartAndEnd());
		const TOptional<TArray<int32>> ParsedMinimum = ParseSemverParts(MinimumVersion.TrimStartAndEnd());
		if (!ParsedVersion.IsSet() || !ParsedMinimum.IsSet()) return false;
		const TArray<int32>& V = ParsedVersion.GetValue();
		const TArray<int32>& M = ParsedMinimum.GetValue();
		if (V[0] != M[0]) return V[0] > M[0];
		if (V[1] != M[1]) return V[1] > M[1];
		return V[2] >= M[2];
	}

	enum class EManagedInstallDecision
	{
		InstallManaged,
		UseManaged,
		UseGlobal
	};

	// Native is the sole authority for whether the pinned managed runtime may
	// be installed. A healthy managed runtime is immutable, and a verified
	// equal/newer global package remains user-owned and unshadowed.
	static EManagedInstallDecision DecideManagedInstall(
		bool bManagedRuntimeReady,
		bool bGlobalRuntimeReady,
		const FString& GlobalRuntimeVersion)
	{
		if (bManagedRuntimeReady)
		{
			return EManagedInstallDecision::UseManaged;
		}
		if (bGlobalRuntimeReady
			&& IsVersionAtLeast(GlobalRuntimeVersion, FString(PackageVersion)))
		{
			return EManagedInstallDecision::UseGlobal;
		}
		return EManagedInstallDecision::InstallManaged;
	}

	// Official successor versions are supported within major 1 only. A future
	// major may change ACP behavior or package layout, so accepting any version
	// >= 1.1.5 would silently opt users into an unverified compatibility range.
	static bool IsSupportedPackageVersion(const FString& Version)
	{
		const TOptional<TArray<int32>> Parsed = ParseSemverParts(Version.TrimStartAndEnd());
		if (!Parsed.IsSet()) return false;
		const TArray<int32>& V = Parsed.GetValue();
		if (V[0] != 1) return false;
		if (V[1] != 1) return V[1] > 1;
		return V[2] >= 5;
	}

	static FString ResolveWithWhere(const FString& Name)
	{
#if PLATFORM_WINDOWS
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(TEXT("where.exe"), *Name, &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode == 0)
		{
			TArray<FString> Lines;
			StdOut.ParseIntoArrayLines(Lines, true);
			for (FString Candidate : Lines)
			{
				Candidate = Candidate.TrimStartAndEnd().TrimQuotes();
				if (NwiroPathExists(Candidate)) return FPaths::ConvertRelativePathToFull(Candidate);
			}
		}
#endif
		return TEXT("");
	}

	static FString ResolveNodePath()
	{
#if PLATFORM_WINDOWS
		FString NodePath = ResolveWithWhere(TEXT("node.exe"));
		if (!NodePath.IsEmpty()) return NodePath;

		const FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
		if (!ProgramFiles.IsEmpty())
		{
			NodePath = FPaths::Combine(ProgramFiles, TEXT("nodejs"), TEXT("node.exe"));
			if (NwiroPathExists(NodePath)) return NodePath;
		}
#endif
		return TEXT("");
	}

	static FString ResolveNpmCliPath(const FString& NodePath)
	{
		if (NodePath.IsEmpty()) return TEXT("");
		FString NpmCli = FPaths::Combine(FPaths::GetPath(NodePath), TEXT("node_modules"), TEXT("npm"), TEXT("bin"), TEXT("npm-cli.js"));
		if (NwiroPathExists(NpmCli)) return NpmCli;

#if PLATFORM_WINDOWS
		const FString NpmCmd = ResolveWithWhere(TEXT("npm.cmd"));
		if (!NpmCmd.IsEmpty())
		{
			NpmCli = FPaths::Combine(FPaths::GetPath(NpmCmd), TEXT("node_modules"), TEXT("npm"), TEXT("bin"), TEXT("npm-cli.js"));
			if (NwiroPathExists(NpmCli)) return NpmCli;
		}
#endif
		return TEXT("");
	}

	static FNodeToolchain ResolveNodeToolchain()
	{
		static FCriticalSection CacheMutex;
		static FNodeToolchain Cached;
		static double CachedAt = -1.0;
		FScopeLock Lock(&CacheMutex);
		const double Now = FPlatformTime::Seconds();
		if (CachedAt >= 0.0 && Now - CachedAt < RuntimeProbeCacheSeconds)
			return Cached;

		FNodeToolchain Result;
		Result.NodePath = ResolveNodePath();
		if (Result.NodePath.IsEmpty())
		{
			Cached = Result;
			CachedAt = Now;
			return Result;
		}

		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(*Result.NodePath, TEXT("--version"), &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode == 0)
		{
			Result.NodeVersion = FirstNonEmptyLine(StdOut);
			Result.bNodeSupported = IsVersionAtLeast(Result.NodeVersion, 20, 0, 0);
		}

		Result.NpmCliPath = ResolveNpmCliPath(Result.NodePath);
		if (!Result.NpmCliPath.IsEmpty())
		{
			ReturnCode = -1;
			StdOut.Empty();
			StdErr.Empty();
			const FString Args = QuoteArg(Result.NpmCliPath) + TEXT(" --version");
			FPlatformProcess::ExecProcess(*Result.NodePath, *Args, &ReturnCode, &StdOut, &StdErr);
			if (ReturnCode == 0) Result.NpmVersion = FirstNonEmptyLine(StdOut);
		}
		Cached = Result;
		CachedAt = Now;
		return Result;
	}

	static FString RuntimeBaseDir()
	{
#if PLATFORM_WINDOWS
		FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		if (Base.IsEmpty()) Base = FPlatformProcess::UserSettingsDir();
		return FPaths::Combine(Base, TEXT("Nwiro"), TEXT("IntegrationKit"), TEXT("runtimes"), TEXT("codex-acp"));
#else
		return TEXT("");
#endif
	}

	static FString ManagedRuntimeDir()
	{
		return FPaths::Combine(RuntimeBaseDir(), PackageVersion);
	}

	static FString RuntimeCoordinationLockPath()
	{
		// Keep the coordination file outside the versioned runtime directory:
		// DeleteAdapter may remove that directory while the exclusive handle is
		// open, but every editor must continue resolving the same lock object.
		return FPaths::Combine(RuntimeBaseDir(), TEXT(".runtime-coordination.lock"));
	}

#if PLATFORM_WINDOWS
	static void ReleaseRuntimeFileLock(void*& InOutHandle)
	{
		if (!InOutHandle) return;
		::CloseHandle(static_cast<Windows::HANDLE>(InOutHandle));
		InOutHandle = nullptr;
	}

	// Shared handles protect a running managed runtime; an exclusive handle
	// protects install/reinstall/delete. Windows share-mode arbitration is
	// atomic across processes and handles are released automatically on crash.
	static bool AcquireRuntimeFileLockAtPath(
		const FString& LockPath,
		bool bExclusive,
		void*& OutHandle)
	{
		OutHandle = nullptr;
		const FString ParentDir = FPaths::GetPath(LockPath);
		if (ParentDir.IsEmpty()
			|| !FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*ParentDir))
		{
			return false;
		}

		FString PlatformPath = FPaths::ConvertRelativePathToFull(LockPath);
		FPaths::MakePlatformFilename(PlatformPath);
		const uint32 ShareMode = bExclusive ? 0u : (FILE_SHARE_READ | FILE_SHARE_WRITE);
		Windows::HANDLE Handle = ::CreateFileW(
			*PlatformPath,
			GENERIC_READ | GENERIC_WRITE,
			ShareMode,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (Handle == INVALID_HANDLE_VALUE) return false;
		OutHandle = Handle;
		return true;
	}

	static bool AcquireRuntimeFileLock(bool bExclusive, void*& OutHandle)
	{
		return AcquireRuntimeFileLockAtPath(
			RuntimeCoordinationLockPath(), bExclusive, OutHandle);
	}

	static bool AttachCodexProcessKillJob(
		const FProcHandle& ProcessHandle,
		void*& OutJobHandle)
	{
		OutJobHandle = nullptr;
		if (!ProcessHandle.IsValid()) return false;

		Windows::HANDLE Job = ::CreateJobObjectW(nullptr, nullptr);
		if (!Job) return false;

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
		Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!::SetInformationJobObject(
			Job,
			JobObjectExtendedLimitInformation,
			&Limits,
			sizeof(Limits))
			|| !::AssignProcessToJobObject(Job, ProcessHandle.Get()))
		{
			::CloseHandle(Job);
			return false;
		}

		OutJobHandle = Job;
		return true;
	}

	static bool TerminateCodexProcessJobAndWait(
		void*& InOutJobHandle,
		double TimeoutSeconds = 1.0)
	{
		if (!InOutJobHandle) return false;
		Windows::HANDLE Job = static_cast<Windows::HANDLE>(InOutJobHandle);

		// TerminateJobObject covers the Node ACP root plus every @openai/codex
		// descendant assigned by Windows. Query ActiveProcesses until zero;
		// TerminateJobObject returning only means termination was requested.
		// Callers currently coordinate game-thread-owned bridge state, so bound
		// the exceptional wait to one second and fail closed if Windows does not
		// drain promptly instead of freezing the editor for the old 10s ceiling.
		::TerminateJobObject(Job, 1);
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		bool bDrained = false;
		do
		{
			JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
			if (::QueryInformationJobObject(
				Job,
				JobObjectBasicAccountingInformation,
				&Accounting,
				sizeof(Accounting),
				nullptr)
				&& Accounting.ActiveProcesses == 0)
			{
				bDrained = true;
				break;
			}
			FPlatformProcess::SleepNoStats(0.01f);
		}
		while (FPlatformTime::Seconds() < Deadline);

		if (bDrained)
		{
			::CloseHandle(Job);
			InOutJobHandle = nullptr;
		}
		return bDrained;
	}
#endif

	static FString PackageDir(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT("node_modules"), TEXT("@agentclientprotocol"), TEXT("codex-acp"));
	}

	static bool ReadPackageIdentity(const FString& Root, FString& OutName, FString& OutVersion)
	{
		OutName.Empty();
		OutVersion.Empty();
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *FPaths::Combine(PackageDir(Root), TEXT("package.json")))) return false;
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return false;

		const TSharedPtr<FJsonObject>* Bin = nullptr;
		FString BinEntry;
		if (!Json->TryGetObjectField(TEXT("bin"), Bin) || !Bin || !Bin->IsValid()
			|| !(*Bin)->TryGetStringField(TEXT("codex-acp"), BinEntry))
		{
			return false;
		}
		BinEntry.ReplaceInline(TEXT("\\"), TEXT("/"));
		return BinEntry == TEXT("dist/index.js")
			&& Json->TryGetStringField(TEXT("name"), OutName)
			&& Json->TryGetStringField(TEXT("version"), OutVersion);
	}

	static bool HasExactManagedMarker(const FString& Root)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(
			Content, *FPaths::Combine(Root, TEXT(".install-complete.json"))))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		FString MarkerPackage;
		FString MarkerVersion;
		return FJsonSerializer::Deserialize(Reader, Json)
			&& Json.IsValid()
			&& Json->TryGetStringField(TEXT("package"), MarkerPackage)
			&& Json->TryGetStringField(TEXT("version"), MarkerVersion)
			&& MarkerPackage == PackageName
			&& MarkerVersion == PackageVersion;
	}

	enum class ERuntimeValidation
	{
		ManagedStaging,
		ManagedComplete,
		Global
	};

	static bool IsRuntimeRootValid(
		const FString& Root,
		ERuntimeValidation Validation,
		FString* OutVersion = nullptr)
	{
		if (Root.IsEmpty()) return false;
		const FString Entry = FPaths::Combine(PackageDir(Root), TEXT("dist"), TEXT("index.js"));
		if (!NwiroPathExists(Entry)) return false;
		if (Validation == ERuntimeValidation::ManagedComplete && !HasExactManagedMarker(Root))
			return false;

		FString Name;
		FString Version;
		if (!ReadPackageIdentity(Root, Name, Version) || Name != PackageName) return false;
		if (Validation == ERuntimeValidation::Global)
		{
			if (!IsSupportedPackageVersion(Version)) return false;
		}
		else if (Version != PackageVersion)
		{
			return false;
		}
		if (OutVersion) *OutVersion = Version;
		return true;
	}

	static FString ResolveGlobalRuntimeRoot(const FNodeToolchain& Toolchain)
	{
		if (Toolchain.NodePath.IsEmpty() || Toolchain.NpmCliPath.IsEmpty()) return TEXT("");
		static FCriticalSection CacheMutex;
		static FString CachedRoot;
		static double CachedAt = -1.0;
		FScopeLock Lock(&CacheMutex);
		const double Now = FPlatformTime::Seconds();
		if (CachedAt >= 0.0 && Now - CachedAt < RuntimeProbeCacheSeconds)
			return CachedRoot;

		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		const FString Args = QuoteArg(Toolchain.NpmCliPath) + TEXT(" root --global");
		FPlatformProcess::ExecProcess(*Toolchain.NodePath, *Args, &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0)
		{
			CachedRoot.Empty();
			CachedAt = Now;
			return TEXT("");
		}
		const FString GlobalNodeModules = FirstNonEmptyLine(StdOut);
		if (GlobalNodeModules.IsEmpty())
		{
			CachedRoot.Empty();
			CachedAt = Now;
			return TEXT("");
		}

		// PackageDir expects a prefix containing node_modules, while npm root
		// returns node_modules itself. Its parent is therefore the launch root.
		const FString Root = FPaths::GetPath(GlobalNodeModules);
		CachedRoot = IsRuntimeRootValid(Root, ERuntimeValidation::Global) ? Root : TEXT("");
		CachedAt = Now;
		return CachedRoot;
	}

	static FLaunchSpec ResolveLaunchSpec()
	{
		FLaunchSpec Spec;
		const FNodeToolchain Toolchain = ResolveNodeToolchain();
		if (!Toolchain.bNodeSupported) return Spec;

		FString Root = ManagedRuntimeDir();
		FString Version;
		if (IsRuntimeRootValid(Root, ERuntimeValidation::ManagedComplete, &Version))
		{
			Spec.Source = TEXT("managed");
		}
		else
		{
			Root = ResolveGlobalRuntimeRoot(Toolchain);
			if (Root.IsEmpty()) return Spec;
			if (!IsRuntimeRootValid(Root, ERuntimeValidation::Global, &Version)) return Spec;
			Spec.Source = TEXT("global");
		}

		Spec.Executable = Toolchain.NodePath;
		Spec.EntryPath = FPaths::Combine(PackageDir(Root), TEXT("dist"), TEXT("index.js"));
		Spec.Arguments = QuoteArg(Spec.EntryPath);
		Spec.Version = Version;
		return Spec;
	}

	// Keep the platform choice in one pure, production-used seam. Tests pass an
	// explicit platform value so both branches are covered without pretending
	// that Unreal's compile-time PLATFORM_WINDOWS macro can be mocked.
	static bool UsesOfficialRuntime(
		const FString& AdapterId,
		bool bIsWindowsPlatform)
	{
		return bIsWindowsPlatform && AdapterId == TEXT("codex");
	}

	static bool UsesOfficialRuntime(const FString& AdapterId)
	{
		return UsesOfficialRuntime(AdapterId, PLATFORM_WINDOWS != 0);
	}

	// True only for LEGACY Zed codex-acp, the one runtime that emits no ACP tool_call
	// for MCP-origin calls and therefore needs the frontend to synthesise a badge.
	// Deliberately NOT `!UsesOfficialRuntime(...)`: that means "Windows AND codex", so
	// its complement also selects claude, localllm, nwiro, empty and unknown — all of
	// which already badge via ACP tool_call and would then render every call twice.
	// Same explicit-platform seam as above so both branches are unit-testable.
	static bool NeedsMcpBadgeFallback(
		const FString& AdapterId,
		bool bIsWindowsPlatform)
	{
		return AdapterId == TEXT("codex")
			&& !UsesOfficialRuntime(AdapterId, bIsWindowsPlatform);
	}

	// Badge policy for an mcp_tool_* event, resolved from the chat that will RECEIVE
	// it. ChatAdapterId wins whenever the chat has one recorded; the global adapter is
	// only a fallback for a chat that has not been assigned one yet.
	//
	// The distinction is the whole point: send setup calls setadapter() several awaits
	// before setactivechat(), so the global can already name an incoming Claude chat
	// while events still route to a legacy-Codex chat. Resolving from the global there
	// would strip that chat's only badge.
	static bool NeedsMcpBadgeFallbackForChat(
		const FString& ChatAdapterId,
		const FString& GlobalAdapterId,
		bool bIsWindowsPlatform)
	{
		const FString& Effective = ChatAdapterId.IsEmpty() ? GlobalAdapterId : ChatAdapterId;
		return NeedsMcpBadgeFallback(Effective, bIsWindowsPlatform);
	}

	static bool IsModelAllowedForRuntime(
		const FString& AdapterId,
		const FString& ModelId,
		bool bIsWindowsPlatform)
	{
		// GPT-5.6 is an official-successor capability. Reject it on the legacy
		// Zed path independently of whatever model catalog that adapter happens
		// to advertise, so a stale or unexpected catalog cannot release a prompt.
		const bool bIsGpt56Model =
			ModelId.Equals(TEXT("gpt-5.6"), ESearchCase::IgnoreCase)
			|| ModelId.StartsWith(TEXT("gpt-5.6-"), ESearchCase::IgnoreCase);
		return AdapterId != TEXT("codex")
			|| !bIsGpt56Model
			|| UsesOfficialRuntime(AdapterId, bIsWindowsPlatform);
	}

	static bool IsModelAllowedForRuntime(
		const FString& AdapterId,
		const FString& ModelId)
	{
		return IsModelAllowedForRuntime(
			AdapterId, ModelId, PLATFORM_WINDOWS != 0);
	}

	static bool IsOfficialIdentity(
		const FString& InAgentName,
		const FString& InAgentVersion,
		const FString& ExpectedPackageVersion,
		double InProtocolVersion)
	{
		return InAgentName == AgentName
			&& InProtocolVersion == ProtocolVersion
			&& IsSupportedPackageVersion(ExpectedPackageVersion)
			&& InAgentVersion == ExpectedPackageVersion;
	}

	static bool ShouldRetireCancelledSession(
		const FString& AdapterId,
		bool bIsWindowsPlatform)
	{
		// Official Codex ACP session/update notifications do not identify the
		// cancelled prompt. Retiring that session is the fail-closed boundary
		// for late chunks. Legacy Zed, Claude, and local-LLM cancellation remains
		// turn-scoped so their ACP conversation stays available for the next turn.
		return UsesOfficialRuntime(AdapterId, bIsWindowsPlatform);
	}

	static bool ShouldRetireCancelledSession(const FString& AdapterId)
	{
		return ShouldRetireCancelledSession(
			AdapterId, PLATFORM_WINDOWS != 0);
	}

	static FSelectConfigCatalog ParseSelectConfigCatalog(
		const TSharedPtr<FJsonObject>& Result,
		const FString& RequestedConfigId)
	{
		FSelectConfigCatalog Catalog;
		const TArray<TSharedPtr<FJsonValue>>* ConfigOptions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("configOptions"), ConfigOptions) || !ConfigOptions)
			return Catalog;

		for (const TSharedPtr<FJsonValue>& Value : *ConfigOptions)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Option = Value->AsObject();
			FString Id;
			if (!Option.IsValid() || !Option->TryGetStringField(TEXT("id"), Id) || Id != RequestedConfigId) continue;

			Catalog.bHasOption = true;
			Option->TryGetStringField(TEXT("currentValue"), Catalog.CurrentValue);
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Option->TryGetArrayField(TEXT("options"), Values) || !Values) break;
			for (const TSharedPtr<FJsonValue>& CandidateValue : *Values)
			{
				if (!CandidateValue.IsValid() || CandidateValue->Type != EJson::Object) continue;
				FString ConfigValue;
				if (CandidateValue->AsObject()->TryGetStringField(TEXT("value"), ConfigValue) && !ConfigValue.IsEmpty())
					Catalog.Values.Add(ConfigValue);
			}
			break;
		}
		return Catalog;
	}

	static FModelCatalog ParseModelCatalog(const TSharedPtr<FJsonObject>& Result)
	{
		const FSelectConfigCatalog Select = ParseSelectConfigCatalog(Result, TEXT("model"));
		FModelCatalog Catalog;
		Catalog.bHasModelOption = Select.bHasOption;
		Catalog.CurrentModel = Select.CurrentValue;
		Catalog.ModelIds = Select.Values;
		return Catalog;
	}

	static bool TryParseRequiredSessionContract(
		const TSharedPtr<FJsonObject>& Result,
		uint64 ProcessGeneration,
		FModelCatalog& OutModelCatalog,
		FSelectConfigCatalog& OutModeCatalog)
	{
		OutModelCatalog = ParseModelCatalog(Result);
		OutModeCatalog = ParseSelectConfigCatalog(Result, TEXT("mode"));
		return OutModelCatalog.bHasModelOption
			&& OutModelCatalog.ModelIds.Num() > 0
			&& OutModeCatalog.bHasOption
			&& OutModeCatalog.Values.Contains(TEXT("read-only"))
			&& OutModeCatalog.Values.Contains(TEXT("agent"))
			&& ProcessGeneration != 0;
	}

	static bool TryParseRequiredModelContract(
		const TSharedPtr<FJsonObject>& Result,
		uint64 ProcessGeneration,
		FModelCatalog& OutModelCatalog)
	{
		OutModelCatalog = ParseModelCatalog(Result);
		return OutModelCatalog.bHasModelOption
			&& OutModelCatalog.ModelIds.Num() > 0
			&& ProcessGeneration != 0;
	}

	static bool IsExactSelectConfigAck(
		const TSharedPtr<FJsonObject>& Result,
		const FString& ConfigId,
		const FString& RequestedValue,
		FSelectConfigCatalog& OutCatalog)
	{
		OutCatalog = ParseSelectConfigCatalog(Result, ConfigId);
		return OutCatalog.bHasOption
			&& OutCatalog.CurrentValue == RequestedValue
			&& OutCatalog.Values.Contains(RequestedValue);
	}

	static bool IsExactModelAck(const TSharedPtr<FJsonObject>& Result, const FString& RequestedModel, FModelCatalog& OutCatalog)
	{
		OutCatalog = ParseModelCatalog(Result);
		return OutCatalog.bHasModelOption
			&& OutCatalog.CurrentModel == RequestedModel
			&& OutCatalog.ModelIds.Contains(RequestedModel);
	}

	static bool HasMode(const TSharedPtr<FJsonObject>& Result, const FString& ModeId)
	{
		if (!Result.IsValid()) return false;
		const TSharedPtr<FJsonObject>* Modes = nullptr;
		if (!Result->TryGetObjectField(TEXT("modes"), Modes) || !Modes || !Modes->IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Available = nullptr;
		if (!(*Modes)->TryGetArrayField(TEXT("availableModes"), Available) || !Available) return false;
		for (const TSharedPtr<FJsonValue>& Value : *Available)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;
			FString Id;
			if (Value->AsObject()->TryGetStringField(TEXT("id"), Id) && Id == ModeId) return true;
		}
		return false;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroIKCodexSuccessorSpec.inl"
#endif

#if PLATFORM_WINDOWS
/** Enumerate Anthropic-installer Claude Code installs under
 *  %APPDATA%\Claude\claude-code\<semver>\claude.exe and return absolute paths
 *  sorted by semver descending (newest first). Empty array if the install root
 *  is missing or contains no valid versioned subdirs.
 *
 *  Rationale: stale `claude.exe` shims in %USERPROFILE%\.local\bin (typically
 *  left by older npm-style installers) silently shadow the Anthropic-installed
 *  binary. Older claude.exe + newer claude-agent-acp produces a silent
 *  TypeError ("H.effortLevel" / reasoning_effort) on session/prompt — the
 *  adapter never emits agent_message_chunk, surfacing as first_token_timeout
 *  after 180s. Searching the installer path FIRST avoids that trap. */
static TArray<FString> FindClaudeInstallerPaths()
{
	TArray<FString> Out;
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (AppData.IsEmpty()) return Out;

	const FString InstallRoot = FPaths::Combine(AppData, TEXT("Claude"), TEXT("claude-code"));
	if (!FPaths::DirectoryExists(InstallRoot)) return Out;

	TArray<FString> VersionDirs;
	IFileManager::Get().FindFiles(VersionDirs,
		*FPaths::Combine(InstallRoot, TEXT("*")), /*Files*/ false, /*Dirs*/ true);

	struct FCandidate { TArray<int32> Sem; FString DirName; };
	TArray<FCandidate> Parsed;
	for (const FString& Dir : VersionDirs)
	{
		TOptional<TArray<int32>> Sem = ParseSemverParts(Dir);
		if (Sem.IsSet()) Parsed.Add({MoveTemp(Sem.GetValue()), Dir});
	}
	// Descending: 2.1.138 wins over 2.1.5 (NOT alphabetical) and over 2.0.58.
	Parsed.Sort([](const FCandidate& A, const FCandidate& B) {
		if (A.Sem[0] != B.Sem[0]) return A.Sem[0] > B.Sem[0];
		if (A.Sem[1] != B.Sem[1]) return A.Sem[1] > B.Sem[1];
		return A.Sem[2] > B.Sem[2];
	});
	for (const FCandidate& C : Parsed)
	{
		Out.Add(FPaths::Combine(InstallRoot, C.DirName, TEXT("claude.exe")));
	}
	return Out;
}

/** Reverse of FindClaudeInstallerPaths: given an absolute .exe path, extract
 *  the semver from the parent directory IFF the path matches the installer
 *  layout (`...\Claude\claude-code\<semver>\claude.exe`). Empty for
 *  `.local\bin\claude.exe` and other non-installer locations. Used by the
 *  min-version warning emitted at adapter launch. */
static TOptional<TArray<int32>> ParseSemverFromInstallerPath(const FString& ExePath)
{
	const FString VersionDir   = FPaths::GetPath(ExePath);            // ...\<semver>
	const FString ClaudeCodeDir = FPaths::GetPath(VersionDir);        // ...\claude-code
	if (VersionDir.IsEmpty() || ClaudeCodeDir.IsEmpty()) return {};
	if (FPaths::GetCleanFilename(ClaudeCodeDir) != TEXT("claude-code")) return {};
	return ParseSemverParts(FPaths::GetCleanFilename(VersionDir));
}
#endif // PLATFORM_WINDOWS

/** adapter-reliability-w7: log a one-shot pre-flight summary of MCP servers
 *  configured in the user's Claude Code settings.
 *
 *  Why: when a user has e.g. "blender" or "unity" MCP servers configured but
 *  those servers aren't running, claude.exe may block ~180s on TCP timeout
 *  trying to enumerate them, producing the same `bytesThisTurn ≈ 1.9KB` +
 *  first_token_timeout signature as the version-mismatch bug. Logging the
 *  configured servers up front gives support a starting point for triage —
 *  "user has 3 MCP servers, did they all need to be running?"
 *
 *  We intentionally do NOT TCP-probe each server. Most MCP servers configured
 *  in Claude Code are stdio type (command/args), which can't be probed without
 *  spawning the command — too invasive for a diagnostic. HTTP/SSE servers
 *  could be probed but adding network ops to plugin startup is fragile (false
 *  alarms during transient blips). Configured-server enumeration is enough
 *  to triage; if probing becomes necessary, ship it as a separate iteration.
 *
 *  Tries known config paths in priority order. Silently skips if no config is
 *  found or parse fails — this is best-effort observability, not a gate. */
static void LogClaudeMcpServersConfig(const FString& AdapterId)
{
	if (AdapterId != TEXT("claude")) return;

	// Static-once: log on first claude adapter spawn per UE editor session.
	// Subsequent restarts (Restart Adapter button) don't re-log unless the
	// editor is fully restarted, preventing log spam on iterative testing.
	static bool bHasLogged = false;
	if (bHasLogged) return;

	TArray<FString> CandidatePaths;
#if PLATFORM_WINDOWS
	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!UserProfile.IsEmpty())
	{
		// Anthropic's claude-code stores MCP servers here on Windows.
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".claude.json")));
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".claude"), TEXT("settings.json")));
	}
	if (!AppData.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(AppData, TEXT("Claude"), TEXT("config.json")));
	}
#else
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(Home, TEXT(".claude.json")));
		CandidatePaths.Add(FPaths::Combine(Home, TEXT(".claude"), TEXT("settings.json")));
	}
#endif

	FString ConfigContent;
	FString FoundPath;
	for (const FString& Path : CandidatePaths)
	{
		if (FFileHelper::LoadFileToString(ConfigContent, *Path))
		{
			FoundPath = Path;
			break;
		}
	}
	bHasLogged = true;  // mark done even if we skip — don't retry every spawn

	if (FoundPath.IsEmpty())
	{
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: Claude Code config not found in any known location — MCP pre-flight skipped (this is fine if user has no MCP servers configured)"));
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ConfigContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: Claude Code config at %s exists but isn't valid JSON — MCP pre-flight skipped"),
			*FoundPath);
		return;
	}

	const TSharedPtr<FJsonObject>* McpServersObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("mcpServers"), McpServersObj) || !McpServersObj || !McpServersObj->IsValid())
	{
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: Claude Code config at %s has no mcpServers field — no MCP servers configured"),
			*FoundPath);
		return;
	}

	TArray<FString> Summaries;
	for (const auto& Pair : (*McpServersObj)->Values)
	{
		if (!Pair.Value.IsValid()) continue;
		TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		if (!Entry.IsValid()) continue;

		FString Type;
		if (!Entry->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
		{
			// Anthropic config schema doesn't always include "type". Infer:
			//   has "url"     → http/sse server
			//   has "command" → stdio server (subprocess invocation)
			if (Entry->HasField(TEXT("url"))) Type = TEXT("http");
			else if (Entry->HasField(TEXT("command"))) Type = TEXT("stdio");
			else Type = TEXT("unknown");
		}
		Summaries.Add(FString::Printf(TEXT("%s [%s]"), *Pair.Key, *Type));
	}

	if (Summaries.Num() == 0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: Claude Code mcpServers field is empty — no MCP servers configured"));
		return;
	}

	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: Claude Code has %d MCP server(s) configured: %s. ")
		TEXT("If any are unreachable when claude.exe starts a session, you may see first_token_timeout. ")
		TEXT("Source: %s"),
		Summaries.Num(),
		*FString::Join(Summaries, TEXT(", ")),
		*FoundPath);
}

/** Build the platform-appropriate CLI search paths for a given CLI base name.
 *  Windows for "claude": %APPDATA%\Claude\claude-code\<latest-version>\claude.exe (installer)
 *           then %USERPROFILE%\.local\bin\{name}.{exe,cmd,(none)} + %APPDATA%\npm\{name}.cmd
 *  Windows other:        only .local\bin + npm fallbacks
 *  Unix:    $HOME/.local/bin, /usr/local/bin, /opt/homebrew/bin (Apple Silicon),
 *           and common npm-global prefixes. Order matters — first hit wins. */
static TArray<FString> BuildCliCandidates(const FString& CliBaseName)
{
	TArray<FString> Out;
#if PLATFORM_WINDOWS
	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));

	// Claude only: prefer the Anthropic installer's versioned path over any stale
	// .local\bin shim. See FindClaudeInstallerPaths() for the failure mode this
	// fixes (TypeError on effortLevel → silent 180s first_token_timeout).
	if (CliBaseName == TEXT("claude"))
	{
		for (const FString& InstallerPath : FindClaudeInstallerPaths())
		{
			Out.Add(InstallerPath);
		}
	}

	Out.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), CliBaseName + TEXT(".exe")));
	Out.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), CliBaseName + TEXT(".cmd")));
	Out.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), CliBaseName));
	Out.Add(FPaths::Combine(AppData, TEXT("npm"), CliBaseName + TEXT(".cmd")));
#else
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	Out.Add(FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), CliBaseName));
	Out.Add(FPaths::Combine(Home, TEXT(".npm-global"), TEXT("bin"), CliBaseName));
	Out.Add(FPaths::Combine(Home, TEXT(".volta"), TEXT("bin"), CliBaseName));
	Out.Add(FPaths::Combine(Home, TEXT(".nvm"), TEXT("versions"), TEXT("node")) /* nvm: caller can stick the latest under here */);
	Out.Add(TEXT("/opt/homebrew/bin/") + CliBaseName);   // Apple Silicon Homebrew
	Out.Add(TEXT("/usr/local/bin/") + CliBaseName);      // Intel Homebrew + plain npm -g
	Out.Add(TEXT("/usr/bin/") + CliBaseName);
#endif
	return Out;
}

static TArray<FAdapterInfo> GetAdapterRegistry()
{
	TArray<FAdapterInfo> Registry;

	// Claude
	{
		FAdapterInfo A;
		A.Id = TEXT("claude");
		A.BinaryName = TEXT("claude-agent-acp");
		// DownloadUrl is intentionally empty: the JS layer (app/src/utils/queries.ts)
		// fetches the correct GitHub release asset for the current platform/arch and
		// passes it to bridge.downloadadapter(adapter, url). Keeping it out of C++
		// means new releases / repo moves don't require a plugin recompile.
		A.DownloadUrl = TEXT("");
		A.EnvKey = TEXT("CLAUDE_CODE_EXECUTABLE");
		A.ExeCandidates = BuildCliCandidates(TEXT("claude"));
		Registry.Add(MoveTemp(A));
	}

	// Codex
	{
		FAdapterInfo A;
		A.Id = TEXT("codex");
		if (NwiroCodex::UsesOfficialRuntime(A.Id))
		{
			// Windows bypasses this filename and launches the verified official
			// package through Node. Keep the managed label so generic Windows
			// discovery can never accidentally select an archived Zed binary.
			A.BinaryName = TEXT("codex-acp-managed");
		}
		else
		{
			// macOS/Linux retain the pre-migration Zed ACP download and CLI
			// discovery contract until the official npm runtime is supported.
			A.BinaryName = TEXT("codex-acp");
			A.DownloadUrl = TEXT("");
			A.ExeCandidates = BuildCliCandidates(TEXT("codex"));
		}
		Registry.Add(MoveTemp(A));
	}

	// local-llm-acp -- first-party shim binary for OpenAI-compatible local LLMs
	// (Ollama, LM Studio, llama.cpp, vLLM, etc.). Self-contained, NOT a CLI shim
	// -- leave EnvKey and ExeCandidates empty so CheckAdapterBinaries only verifies
	// the downloaded binary itself.
	{
		FAdapterInfo A;
		A.Id = TEXT("localllm");
		A.BinaryName = TEXT("local-llm-acp");
		// DownloadUrl supplied by JS layer (see Claude/Codex comments above).
		A.DownloadUrl = TEXT("");
		Registry.Add(MoveTemp(A));
	}

	return Registry;
}

static const FAdapterInfo* FindAdapter(const FString& Id)
{
	static TArray<FAdapterInfo> Registry = GetAdapterRegistry();
	for (const FAdapterInfo& A : Registry)
	{
		if (A.Id == Id) return &A;
	}
	return nullptr;
}

// adapter-reliability-w0: deterministic preflight before EnsureProcess spawns
// the adapter binary. File-existence checks only — intentionally no `--version`
// probe, no auth detection, no MCP check; those land in Wave 3. Returns true
// when healthy. On false, OutCode/OutMessage describe the failure for an
// adapter_error event. Two failure modes:
//   - adapter_exe_missing: the resolved adapter binary (e.g. claude-agent-acp)
//     isn't on disk where FindAdapterBinary said it would be.
//   - cli_missing: the adapter is a shim (EnvKey set) and none of its
//     ExeCandidates exist. Without this check, the shim spawns and answers
//     `initialize` itself, then hangs `session/new` waiting for the absent
//     CLI to delegate to (Hypothesis #1 in tasks/todo.md).
static bool CheckAdapterBinaries(const FString& AdapterId, const FString& AdapterBinary,
	FString& OutCode, FString& OutMessage)
{
	// Empty AdapterBinary means FindAdapterBinary couldn't resolve a path —
	// treat as missing so the user gets a specific error instead of the
	// generic adapter_launch_failed that would fire after the spawn attempt.
	if (AdapterBinary.IsEmpty() || !NwiroPathExists(AdapterBinary))
	{
		OutCode = TEXT("adapter_exe_missing");
		if (NwiroCodex::UsesOfficialRuntime(AdapterId))
		{
			OutCode = TEXT("managed_runtime_missing");
			OutMessage = TEXT("The official Codex ACP runtime is not ready. Install it from Settings > Integrations.");
			return false;
		}
		OutMessage = AdapterBinary.IsEmpty()
			? FString::Printf(TEXT("Adapter binary for '%s' could not be located. Open Settings and click Reinstall Adapter."), *AdapterId)
			: FString::Printf(TEXT("Adapter binary not found at %s. Reinstall the adapter or pick a different one in Settings."), *AdapterBinary);
		return false;
	}

	const FAdapterInfo* AdapterInfo = FindAdapter(AdapterId);
	if (AdapterInfo && !AdapterInfo->EnvKey.IsEmpty())
	{
		for (const FString& Candidate : AdapterInfo->ExeCandidates)
		{
			if (NwiroPathExists(Candidate)) return true;
		}
		OutCode = TEXT("cli_missing");
#if PLATFORM_WINDOWS
		const TCHAR* SearchHint = TEXT("%APPDATA%\\Claude\\claude-code\\<version> (preferred — Anthropic installer), %USERPROFILE%\\.local\\bin, or %APPDATA%\\npm");
#else
		const TCHAR* SearchHint = TEXT("~/.local/bin, /opt/homebrew/bin or /usr/local/bin");
#endif
		OutMessage = FString::Printf(
			TEXT("%s adapter requires the underlying CLI to be installed (none of the expected paths under %s exist). Install the CLI from the upstream docs and retry."),
			*AdapterId, SearchHint);
		return false;
	}

	return true;
}

static FString GetOptionalStringField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	FString Value;
	return Obj.IsValid() && Obj->TryGetStringField(FieldName, Value) ? Value : TEXT("");
}

static TSharedPtr<FJsonObject> GetOptionalObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
{
	if (!Obj.IsValid()) return nullptr;
	// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
	// while remaining compatible with UE 5.7. Returns TSharedPtr (not pointer-to-TSharedPtr).
	TSharedPtr<FJsonValue> Field = Obj->TryGetField(FieldName);
	return Field.IsValid() ? Field->AsObject() : nullptr;
}

static bool HasTerminalContent(const TSharedPtr<FJsonObject>& Obj)
{
	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!Obj.IsValid() || !Obj->TryGetArrayField(TEXT("content"), Content)) return false;

	for (const TSharedPtr<FJsonValue>& Item : *Content)
	{
		TSharedPtr<FJsonObject> ContentObj = Item.IsValid() ? Item->AsObject() : nullptr;
		if (!ContentObj.IsValid()) continue;
		if (GetOptionalStringField(ContentObj, TEXT("type")).Equals(TEXT("terminal"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

// Shell-evidence field names, checked at every nesting level. Deliberately a SUPERSET
// of the shapes ExtractShellCommand knows how to read (including its top-level fallback):
// a command we could execute but could not detect is the one asymmetry that must never
// exist. Replaces an earlier 3-key helper that was narrower than the extractor.
//
// These are HEURISTICS ON PARAMETER NAMES, not capability detection. A field named
// `script` may be an OS command, a sandboxed orchestration script (UE 5.8's
// ProgrammaticToolset `execute_tool_script`), or an innocent provider parameter — this
// cannot tell them apart, and it classifies all three as shell. Capability policy belongs
// at the dispatch/execution sink, not here.
static bool HasShellInputFieldsAtLevel(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid()) return false;

	static const TCHAR* const ShellKeys[] = {
		TEXT("command"), TEXT("cmd"), TEXT("commandLine"),
		TEXT("cwd"), TEXT("terminalId"),
		TEXT("argv"), TEXT("args"), TEXT("script")
	};
	for (const TCHAR* const Key : ShellKeys)
	{
		if (Obj->HasField(Key)) return true;
	}
	return false;
}

// MCP frames carry their real payload under `rawInput.arguments`, which a top-level-only
// check never reaches. That gap is half of a constructible bypass: an MCP server registered
// under the name "nwiro" invoking `run_command` with `arguments.command` satisfies the nwiro
// identity check while hiding the command from every shell predicate.
//
// Descends only into known container field names rather than enumerating keys: FJsonObject's
// key type differs between UE 5.7 (FString) and 5.8 (FSharedString), and iterating it
// portably is its own hazard (see the note on GetOptionalObjectField). Depth is bounded so a
// hostile deeply-nested payload cannot burn the stack.
static bool HasShellInputFieldsDeep(const TSharedPtr<FJsonObject>& Obj, int32 Depth = 0)
{
	static constexpr int32 MaxDepth = 3;
	if (!Obj.IsValid() || Depth > MaxDepth) return false;

	if (HasShellInputFieldsAtLevel(Obj)) return true;

	// Descend only into fields that are genuinely objects. GetOptionalObjectField calls
	// AsObject() speculatively, which emits a LogJson warning for scalar/array values —
	// noisy on real frames (the permission envelope's `options` is an array).
	auto ObjectFieldOrNull = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key) -> TSharedPtr<FJsonObject>
	{
		if (!Parent.IsValid()) return nullptr;
		TSharedPtr<FJsonValue> Field = Parent->TryGetField(Key);
		if (!Field.IsValid() || Field->Type != EJson::Object) return nullptr;
		return Field->AsObject();
	};

	static const TCHAR* const Containers[] = {
		TEXT("arguments"), TEXT("input"), TEXT("params"), TEXT("options")
	};
	for (const TCHAR* const Container : Containers)
	{
		if (HasShellInputFieldsDeep(ObjectFieldOrNull(Obj, Container), Depth + 1))
		{
			return true;
		}
	}
	return false;
}

static bool LooksLikeShellTitle(const FString& Title)
{
	return Title.Contains(TEXT("terminal/"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("PowerShell"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("cmd.exe"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("bash"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("shell"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Get-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Set-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("New-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Remove-"), ESearchCase::IgnoreCase)
		|| Title.StartsWith(TEXT("Start-"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("pwd"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("dir"), ESearchCase::IgnoreCase)
		|| Title.Equals(TEXT("ls"), ESearchCase::IgnoreCase);
}

static FString MakeToolCallKey(const FString& SessionId, const FString& ToolCallId)
{
	if (ToolCallId.IsEmpty()) return TEXT("");
	return SessionId.IsEmpty()
		? ToolCallId
		: FString::Printf(TEXT("%s::%s"), *SessionId, *ToolCallId);
}

// adapter-reliability-w2 §8: classified-string table for non-JSON adapter
// stdout/stderr lines. Many adapter failures arrive as bare-text lines (auth
// errors, trust prompts, MCP connection failures) before any RPC times out.
// Mapping these to specific codes lets the user see "auth_required: ..."
// instead of waiting 30s for session_new_timeout. Patterns are case-insensitive
// substrings; first match wins. Keep narrow + adapter-agnostic.
struct FStdoutKeywordPattern
{
	const TCHAR* Needle;     // lowercase substring to match
	const TCHAR* Code;       // classified code (must be in shared/errors allowlist)
	const TCHAR* Stage;      // stage at which the failure conceptually occurred
	const TCHAR* Message;    // user-facing prose paired with the code
};

// Order matters: more specific phrases (longer, less ambiguous) come first
// so e.g. "permission denied" matches before bare "permission" would.
static const FStdoutKeywordPattern STDOUT_KEYWORD_TABLE[] = {
	{ TEXT("not logged in"),         TEXT("auth_required"),        TEXT("creating_session"),
	  TEXT("Adapter reported you are not logged in. Open Claude Code and sign in, then retry.") },
	{ TEXT("unauthorized"),          TEXT("auth_required"),        TEXT("creating_session"),
	  TEXT("Adapter reported unauthorized — Claude Code auth is missing or expired. Sign in via Claude Code and retry.") },
	{ TEXT("permission denied"),     TEXT("project_not_trusted"),  TEXT("creating_session"),
	  TEXT("Adapter reported permission denied — likely a project-trust issue. Open this project in Claude Code once to grant trust.") },
	{ TEXT("project not trusted"),   TEXT("project_not_trusted"),  TEXT("creating_session"),
	  TEXT("Adapter reported the project is not trusted. Open it in Claude Code once and confirm trust.") },
	{ TEXT("handshaking with mcp"),  TEXT("mcp_unavailable"),      TEXT("creating_session"),
	  TEXT("Adapter could not complete the MCP handshake — Nwiro MCP may not be running or the port is wrong.") },
	{ TEXT("mcp startup"),           TEXT("mcp_unavailable"),      TEXT("creating_session"),
	  TEXT("Adapter reported MCP startup failure — try Restart MCP from Settings.") },
	{ TEXT("econnrefused"),          TEXT("mcp_connection_failed"), TEXT("creating_session"),
	  TEXT("Adapter could not connect to MCP server (ECONNREFUSED) — check that Nwiro MCP is running on the expected port.") },
	{ TEXT("connection refused"),    TEXT("mcp_connection_failed"), TEXT("creating_session"),
	  TEXT("Adapter could not connect to MCP server. Check that Nwiro MCP is running.") },
	{ TEXT("failed to start"),       TEXT("adapter_launch_failed"), TEXT("launching_process"),
	  TEXT("Adapter reported a launch failure on stdout. Check the Output Log for details.") },
	// adapter-reliability-w7: catch the farias-documented version-mismatch crash.
	// Older Claude Code (< 2.1.0) throws "TypeError: null is not an object (evaluating 'H.effortLevel')"
	// when newer claude-agent-acp passes the reasoning_effort field it doesn't know about.
	// Manifests as silent first_token_timeout 180s after session/prompt unless caught here.
	// 'effortlevel' (lowercase, minified) is highly specific — extremely low false-positive risk.
	{ TEXT("effortlevel"),           TEXT("adapter_launch_failed"), TEXT("sending_prompt"),
	  TEXT("Claude Code threw an internal error on the reasoning_effort field. ")
	  TEXT("This usually means Claude Code is older than 2.1.0 — incompatible with the current claude-agent-acp. ")
	  TEXT("Update Claude Code via the Anthropic installer (path: %APPDATA%\\Claude\\claude-code\\).") },
};

static const FStdoutKeywordPattern* MatchStdoutKeyword(const FString& Lower)
{
	for (const FStdoutKeywordPattern& Entry : STDOUT_KEYWORD_TABLE)
	{
		if (Lower.Contains(Entry.Needle)) return &Entry;
	}
	return nullptr;
}

// F3: human-readable label for the ACTIVE adapter, so a classified adapter
// error names the adapter that produced it (Codex / the local model) instead
// of the table's hardcoded "Claude Code" placeholder. Auth/trust errors in
// practice fire only on claude/codex; the fallback covers everything else.
static FString AdapterDisplayLabel(const FString& AdapterId)
{
	if (AdapterId == TEXT("claude")) return TEXT("Claude Code");
	if (AdapterId == TEXT("codex"))  return TEXT("Codex");
	if (AdapterId == TEXT("local-llm") || AdapterId == TEXT("localllm") || AdapterId == TEXT("local_llm"))
		return TEXT("the local model");
	return AdapterId.IsEmpty() ? TEXT("the agent") : AdapterId;
}

// Matches tool calls that CLAIM to come from our MCP server. codex-acp titles MCP calls
// `mcp.<server>.<tool>` and copies `rawInput.server` from the Codex client config, so this
// pair is narrower than "arrived over MCP" — a third-party MCP wrapper under a different
// name is not exempted.
//
// THIS IS NOT AUTHENTICATION. The server name is a free-text key in the user's Codex
// config, which codex-acp inherits wholesale. Anything registered under the name "nwiro"
// inherits this exemption, and nothing here binds it to the plugin-spawned process. The
// deep field scan above narrows what such a server could smuggle through to payloads that
// avoid every recognised command field name — it does not close the impersonation itself.
// Closing it requires binding identity to a plugin-owned value (a per-launch nonce echoed
// by our own server, or a process/transport check). Tracked separately.
static bool IsNwiroMcpToolCall(const TSharedPtr<FJsonObject>& ToolCall)
{
	if (!ToolCall.IsValid()) return false;

	TSharedPtr<FJsonObject> RawInput = GetOptionalObjectField(ToolCall, TEXT("rawInput"));
	if (!GetOptionalStringField(RawInput, TEXT("server")).Equals(TEXT("nwiro"), ESearchCase::IgnoreCase))
		return false;

	return GetOptionalStringField(ToolCall, TEXT("title")).StartsWith(TEXT("mcp.nwiro."));
}

// Returns the predicate that classified this call as shell, or nullptr if it is not
// shell. Callers that only need the boolean use IsShellToolCall; the reason string is
// logged at the block sites so a user report identifies which rule fired.
static const TCHAR* ShellClassificationReason(const TSharedPtr<FJsonObject>& ToolCall)
{
	if (!ToolCall.IsValid()) return nullptr;

	// Positive FIELD/TERMINAL evidence first: it must outrank the exemption below, so a
	// misbehaving nwiro server cannot smuggle a terminal payload through the hole.
	// NOTE the title heuristic (LooksLikeShellTitle) is deliberately NOT part of this —
	// it runs after the exemption, because it is a weak name-shaped guess that would
	// false-positive legitimate nwiro tools. Only hard evidence outranks identity.
	if (HasTerminalContent(ToolCall)) return TEXT("terminal_content");
	// AtLevel, not the 3-key HasShellInputFields: ExtractShellCommand falls back to
	// reading command/cmd/commandLine/args/argv off the TOP-LEVEL tool call, so a
	// narrower detector here would leave a command we can execute but cannot see.
	if (HasShellInputFieldsAtLevel(ToolCall)) return TEXT("shell_input_fields");

	// SHALLOW at rawInput level, and deliberately ABOVE the exemption. A genuine codex shell
	// frame puts its command at `rawInput.command` (buildCommandPermissionRequest), so this
	// must outrank identity: a server calling itself "nwiro" cannot rescue a real shell frame.
	// No MCP envelope carries a ShellKey at this level — `rawInput` is {server,tool,arguments}
	// — so this costs zero false positives across the tool registry.
	TSharedPtr<FJsonObject> RawInput = GetOptionalObjectField(ToolCall, TEXT("rawInput"));
	if (HasShellInputFieldsAtLevel(RawInput)) return TEXT("rawinput_shell_fields");

	// Tools claiming our MCP identity are not treated as a shell surface. codex-acp emits
	// every MCP tool call with kind "execute" (its generic invocation bucket), so without
	// this exemption the fallback below misclassifies all of them.
	//
	// Reaching here means no recognised shell evidence was found — NOT that the call is
	// safe. See the trust caveat on IsNwiroMcpToolCall.
	if (IsNwiroMcpToolCall(ToolCall)) return nullptr;

	// DEEP scan, deliberately BELOW the exemption — it applies to third-party MCP servers only.
	//
	// It was briefly ordered above, on the theory that shell evidence must always outrank
	// identity. That was wrong on cost/benefit. Our OWN `pie_console_command` legitimately
	// carries `rawInput.arguments.command`, so scanning nwiro-identity payloads at depth
	// blocks a real tool AND cancels the whole turn. The security it bought was near-zero:
	// ExtractShellCommand never descends into `arguments`, so nwiro cannot execute what this
	// finds; the only target was a squatting server whose command already ran on its own
	// process, and squatting is a known-open hole that renaming a field already evades.
	if (HasShellInputFieldsDeep(RawInput)) return TEXT("rawinput_shell_fields_deep");

	// Fail-closed fallback for everything not claiming our MCP identity. Deliberately kept
	// below the exemption rather than removed: it is the last line of defence for MCP
	// servers whose payload shape hides a command from every check above. Note this also
	// blocks benign third-party MCP calls (e.g. mcp.github.list_issues) — a known
	// false-positive class, tracked separately from this classifier.
	if (GetOptionalStringField(ToolCall, TEXT("kind")).Equals(TEXT("execute"), ESearchCase::IgnoreCase))
		return TEXT("kind_execute");

	if (LooksLikeShellTitle(GetOptionalStringField(ToolCall, TEXT("title"))))
		return TEXT("shell_title");

	return nullptr;
}

static bool IsShellToolCall(const TSharedPtr<FJsonObject>& ToolCall)
{
	return ShellClassificationReason(ToolCall) != nullptr;
}

// True only for evidence STRONGER than the generic kind:"execute" bucket.
//
// For a session/update tool_call, kind_execute is a useful fail-closed fallback: the
// frame is rich (title, rawInput), so anything still landing on the fallback is genuinely
// unidentifiable. At the PERMISSION site the same rule is worthless, because codex-acp
// (buildPermissionRequest, verified in 1.1.7 dist) sends the correlated MCP approval as
// a bare frame:
//     { toolCallId, kind: "execute", status: "pending" }     — no title, no rawInput
// and stamps kind:"execute" on real shell approvals too. Every MCP approval therefore
// matches kind_execute, which is precisely how the misclassification arose. A permission
// check that counts kind_execute as evidence cannot distinguish the two shapes at all.
//
// So the permission site asks this narrower question and gets identity from its own
// correlation record instead. Everything except kind_execute still counts.
// Everything the B1 exemption decision depends on, gathered so the decision itself is a
// pure function. The surrounding handler does RPC I/O and UI dispatch that a unit test
// cannot drive, and an authorization rule that can only be exercised by running the whole
// editor is an authorization rule that does not get tested — which is how the first draft
// of this decision shipped unreachable.
struct FNwiroMcpApprovalInputs
{
	bool    bMcpApprovalMarker       = false;  // _meta.is_mcp_tool_approval, boolean true
	bool    bRealShellMarker         = false;  // _meta.codex present
	bool    bOfficialCodexRuntime    = false;  // the only runtime that emits the marker
	bool    bSessionLive             = false;  // the named ACP session is still mapped
	bool    bClaimFound              = false;  // exact (session, callId) claim consumed
	FString ClaimAdapterId;
	uint64  ClaimProcessGeneration   = 0;
	FString RequestAdapterId;
	uint64  CurrentProcessGeneration = 0;
	bool    bEnvelopeShellEvidence   = false;  // HasShellEvidenceBeyondKind
	bool    bPreviouslyBlocked       = false;  // id already poisoned
};

// Fail-closed by construction: every clause is a reason to REFUSE, and the function only
// reaches `true` when all of them are cleared.
static bool IsExemptNwiroMcpApproval(const FNwiroMcpApprovalInputs& In)
{
	// The marker is emitted only by official codex-acp. Honouring it on any other
	// adapter would extend an exemption to a runtime whose envelope we have not read.
	if (!In.bOfficialCodexRuntime)                                return false;
	// A retired session cannot authorize anything. Process generation does NOT cover
	// this: CloseChat, first-token timeout and unknown-session recovery all drop a
	// session while the adapter process keeps running, so a delayed approval would
	// still match on adapter+generation and reach auto-allow.
	if (!In.bSessionLive)                                         return false;
	if (!In.bMcpApprovalMarker || In.bRealShellMarker)            return false;
	// No claim = we cannot name what we are approving, so we do not approve it.
	if (!In.bClaimFound)                                          return false;
	if (In.ClaimAdapterId != In.RequestAdapterId)                 return false;
	// Generation 0 means "no live process" on either side and must never match itself.
	if (In.ClaimProcessGeneration == 0)                           return false;
	if (In.ClaimProcessGeneration != In.CurrentProcessGeneration) return false;
	// Hard evidence and prior poison both outrank identity.
	if (In.bEnvelopeShellEvidence)                                return false;
	if (In.bPreviouslyBlocked)                                    return false;
	return true;
}

// How long an un-clicked permission card stays answerable.
//
// Deliberately UNDER the upstream client's tool timeout. The repo pins Codex
// `tool_timeout_sec = 120`, so a card outliving that settles a request the agent has
// already abandoned — we would answer into the void and still believe the turn was
// resolved. 110s leaves a small margin for the round trip.
//
// Caveat worth stating rather than hiding: it is not proven that the ACP approval wait
// uses that exact 120s timer. If it turns out to have its own, longer budget, this value
// is unnecessarily tight and should be raised — the env override exists so that can be
// tested without a rebuild.
static double NwiroPermissionTtlSeconds()
{
	static const double Ttl = []() -> double
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("NWIRO_PERMISSION_TTL_SECONDS"));
		const double Parsed = Override.IsEmpty() ? 0.0 : FCString::Atod(*Override);
		return Parsed > 0.0 ? Parsed : 110.0;
	}();
	return Ttl;
}

// The single place that says what happens to a permission request. Shell rejection
// outranks every mode; an unrecognised mode falls through to ShowCard rather than to a
// permissive comparison, so a malformed or future mode string asks the user instead of
// silently behaving like bypass.
static ENwiroPermissionDisposition DecidePermissionDisposition(bool bShellPermission, const FString& Mode)
{
	if (bShellPermission)                  return ENwiroPermissionDisposition::ShellReject;
	if (Mode == TEXT("bypassPermissions")) return ENwiroPermissionDisposition::AutoAllow;
	if (Mode == TEXT("acceptEdits"))       return ENwiroPermissionDisposition::AutoAllow;
	if (Mode == TEXT("plan"))              return ENwiroPermissionDisposition::AutoDeny;
	if (Mode == TEXT("dontAsk"))           return ENwiroPermissionDisposition::AutoDeny;
	return ENwiroPermissionDisposition::ShowCard;
}

static bool HasShellEvidenceBeyondKind(const TSharedPtr<FJsonObject>& ToolCall)
{
	if (!ToolCall.IsValid()) return false;

	// Tested independently, NOT read off ShellClassificationReason's verdict: that
	// function returns kind_execute before it ever reaches the title heuristic, so
	// {kind:"execute", title:"PowerShell"} reports "kind_execute" and would otherwise
	// be waved through here as carrying no stronger evidence. A title is absent from the
	// bare approval frame, so unlike kind it is a real discriminator when present.
	if (LooksLikeShellTitle(GetOptionalStringField(ToolCall, TEXT("title")))) return true;

	const TCHAR* Reason = ShellClassificationReason(ToolCall);
	return Reason != nullptr && FCString::Strcmp(Reason, TEXT("kind_execute")) != 0;
}

// Phase 3b.3: extract the shell command string from a tool-call across the payload shapes adapters
// use. codex puts it in rawInput.command (string); other shapes seen: rawInput.cmd / commandLine, or
// an args/argv array (joined with spaces). Returns empty if none found — callers MUST treat empty as
// fail-closed (feed Assess(""), which the policy returns as Block), never as "no command => allow".
static FString ExtractShellCommand(const TSharedPtr<FJsonObject>& ToolCall)
{
	if (!ToolCall.IsValid()) return FString();

	auto FromObject = [](const TSharedPtr<FJsonObject>& Obj) -> FString
	{
		if (!Obj.IsValid()) return FString();
		for (const TCHAR* Key : { TEXT("command"), TEXT("cmd"), TEXT("commandLine") })
		{
			FString S;
			if (Obj->TryGetStringField(Key, S) && !S.TrimStartAndEnd().IsEmpty()) return S;
		}
		for (const TCHAR* Key : { TEXT("command"), TEXT("args"), TEXT("argv") })
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Obj->TryGetArrayField(Key, Arr) && Arr)
			{
				TArray<FString> Parts;
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString Item;
					if (V.IsValid() && V->TryGetString(Item)) Parts.Add(Item);
				}
				if (Parts.Num() > 0) return FString::Join(Parts, TEXT(" "));
			}
		}
		return FString();
	};

	FString Cmd = FromObject(GetOptionalObjectField(ToolCall, TEXT("rawInput")));
	if (Cmd.IsEmpty()) Cmd = FromObject(ToolCall);
	return Cmd;
}

// Append the `text` of every {type,text} content item to Out, newline-separated.
static void AppendAcpContentText(const TArray<TSharedPtr<FJsonValue>>* Items, FString& Out)
{
	if (!Items) return;
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		if (!Item.IsValid() || Item->Type != EJson::Object) continue;
		FString Text;
		if (Item->AsObject()->TryGetStringField(TEXT("text"), Text))
		{
			if (!Out.IsEmpty()) Out += TEXT("\n");
			Out += Text;
		}
	}
}

// Append the text carried by one rawOutput sub-value: a plain string, an array of
// {type,text} items, or an object holding either under `content` / `output` / `text`.
// Deliberately NOT recursive — the shapes below nest exactly one level and a self-call
// would let a malformed frame from an adapter drive unbounded recursion.
static void AppendAcpValueText(const TSharedPtr<FJsonValue>& Value, FString& Out)
{
	if (!Value.IsValid()) return;

	auto AppendLine = [&Out](const FString& Text)
	{
		if (Text.IsEmpty()) return;
		if (!Out.IsEmpty()) Out += TEXT("\n");
		Out += Text;
	};

	if (Value->Type == EJson::String) { AppendLine(Value->AsString()); return; }
	if (Value->Type == EJson::Array) { AppendAcpContentText(&Value->AsArray(), Out); return; }
	if (Value->Type != EJson::Object) return;

	const TSharedPtr<FJsonObject> Obj = Value->AsObject();
	if (!Obj.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Nested = nullptr;
	if (Obj->TryGetArrayField(TEXT("content"), Nested)) { AppendAcpContentText(Nested, Out); return; }

	FString Text;
	if (Obj->TryGetStringField(TEXT("output"), Text) || Obj->TryGetStringField(TEXT("text"), Text))
	{
		AppendLine(Text);
	}
}

// Ceiling on what one tool result may put on the wire to CEF.
//
// The renderer copies this string into a bridge event and a tool block, neither of which
// caps it. A single multi-megabyte payload is enough to freeze or reload the panel, so the
// bound lives HERE, at the only place that decides what a Result contains.
static constexpr int32 NwiroMaxToolOutputChars = 64 * 1024;
// Reserved out of the ceiling so a huge RESULT can never squeeze out the ERROR appended
// after it. A truncation that deletes the failure message turns a failed tool into a
// silent one, which is the bug the `error` field was surfaced to fix in the first place.
static constexpr int32 NwiroToolErrorBudgetChars = 4 * 1024;

// Truncate from the MIDDLE, not the end.
//
// Shell and build output reports its failure in the LAST lines — head-only truncation keeps
// exactly the half that says the command started and drops the half that says how it went.
static FString ClampToolOutputText(const FString& Text, int32 MaxChars)
{
	if (MaxChars <= 0) return FString();
	if (Text.Len() <= MaxChars) return Text;

	const int32 Dropped = Text.Len() - MaxChars;
	const FString Marker = FString::Printf(TEXT("\n… [%d characters not shown] …\n"), Dropped);
	// The marker itself can exceed a small ceiling. Returning it alone would break the one
	// thing this function promises — a hard bound on what reaches CEF — so a budget too
	// small to say anything useful just truncates plainly.
	if (Marker.Len() >= MaxChars) return Text.Left(MaxChars);

	// Marker included in the budget, and the tail favoured — that is where failures live.
	const int32 Budget = MaxChars - Marker.Len();
	int32 Head = Budget / 3;
	int32 Tail = Budget - Head;

	// FString is UTF-16, and Left/Right cut CODE UNITS. Cutting between the halves of a
	// surrogate pair emits a lone surrogate — reachable on any oversized output containing
	// emoji or non-BMP text, which shell output routinely has. Step off the boundary rather
	// than emit one; losing one character beats corrupting the encoding.
	auto IsHighSurrogate = [](TCHAR C) { return C >= 0xD800 && C <= 0xDBFF; };
	auto IsLowSurrogate  = [](TCHAR C) { return C >= 0xDC00 && C <= 0xDFFF; };
	if (Head > 0 && IsHighSurrogate(Text[Head - 1])) --Head;          // don't end on a lead
	if (Tail > 0 && IsLowSurrogate(Text[Text.Len() - Tail])) --Tail;  // don't start on a trail

	return Text.Left(Head) + Marker + Text.Right(Tail);
}

// codex's image-generation frame, which reuses the SAME `result` key the MCP envelope uses
// — for the complete base64 PNG. See imageGenerationRawOutput() in codex-acp:
//
//     { status, revisedPrompt, result: <base64 png>, savedPath? }
//
// `revisedPrompt` and `savedPath` appear in no other rawOutput shape, so either one
// identifies the frame. The MCP envelope is identified positively instead (below), which
// means an unrecognised future shape carrying a bare `result` string is NOT rendered — the
// fail-safe direction, since the cost of guessing wrong here is dumping binary into the UI.
static bool IsImageGenerationRawOutput(const TSharedPtr<FJsonObject>& Obj)
{
	return Obj.IsValid() && (Obj->HasField(TEXT("revisedPrompt")) || Obj->HasField(TEXT("savedPath")));
}

// Human-readable text out of an ACP tool_call_update `rawOutput`.
//
// Every shape below has been observed on the wire and must keep working:
//
//   codex-acp, MCP tools       : { "result": { "content": [{type,text}] }, "error": null }
//   codex-acp, image generation: { "status", "revisedPrompt", "result": <base64 png>, "savedPath"? }
//   codex-acp, function calls  : { "output": <string | [{text}] | {output,metadata}> }
//   codex-acp, terminal path   : { "formatted_output": "...", "exit_code": 0 }
//   codex 0.13.0 / localllm    : { "content": [{type,text}] }
//   older adapters             : [{type,text}]
//   some adapters              : "plain string"
//
// The `result` wrapper comes from codex-acp's own createMcpRawOutput(result, error). The
// previous extractor read only `.content`, one level too shallow, so EVERY nwiro MCP tool
// produced an empty string — the badge modal showed Arguments with a blank Result while
// the tool had in fact succeeded. This is the second bug of exactly this shape in this
// field (see the changelog's "ACP rawOutput parsing fixed"), which is why the shapes are
// enumerated here and pinned by NwiroIKToolOutputSpec rather than left inline.
//
// `result` IS NOT A UNIVERSAL KEY, and reading it as text wherever it appeared was the
// third bug in this field: image generation puts a whole base64 PNG there. So the MCP
// envelope is now matched by its SIGNATURE — createMcpRawOutput always emits `result` and
// `error` TOGETHER, or returns undefined — rather than by the presence of `result` alone.
// A field name means whatever its producer decided it means; there is no global meaning.
//
// The `output` / `formatted_output` shapes are the SAME empty-Result bug for codex's own
// built-in tools. Those never reach createMcpRawOutput at all: codex-acp routes them
// through createFunctionCallOutputUpdate, which emits `{output}` normally and
// `{formatted_output, exit_code}` on the terminal path. Neither carries `content`, so
// `wait`, `shell` and `apply_patch` rendered a blank Result even while working.
//
// `error` is surfaced too: a failed MCP call carries its message there, and dropping it
// meant a failure rendered as an empty result — indistinguishable from success.
static FString ExtractAcpToolOutputText(const TSharedPtr<FJsonValue>& Raw)
{
	if (!Raw.IsValid()) return FString();

	if (Raw->Type == EJson::String) return ClampToolOutputText(Raw->AsString(), NwiroMaxToolOutputChars);

	FString Out;

	if (Raw->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Arr = Raw->AsArray();
		AppendAcpContentText(&Arr, Out);
		return ClampToolOutputText(Out, NwiroMaxToolOutputChars);
	}

	if (Raw->Type != EJson::Object) return Out;
	const TSharedPtr<FJsonObject> Obj = Raw->AsObject();
	if (!Obj.IsValid()) return Out;

	// Image generation FIRST, before anything can look at `result`. Only the bounded
	// metadata is shown; `result` holds the raw base64 PNG and must never be forwarded as
	// text. The image itself already reaches the UI through the frame's own `content`
	// block, which carries it as a typed image — this path is the badge's Result pane.
	if (IsImageGenerationRawOutput(Obj))
	{
		FString Field;
		if (Obj->TryGetStringField(TEXT("revisedPrompt"), Field) && !Field.IsEmpty())
		{
			Out += TEXT("Revised prompt: ") + Field;
		}
		if (Obj->TryGetStringField(TEXT("savedPath"), Field) && !Field.IsEmpty())
		{
			if (!Out.IsEmpty()) Out += TEXT("\n");
			Out += TEXT("Saved to: ") + Field;
		}
		return ClampToolOutputText(Out, NwiroMaxToolOutputChars);
	}

	// Shape 2: content directly on rawOutput.
	const TArray<TSharedPtr<FJsonValue>>* Direct = nullptr;
	if (Obj->TryGetArrayField(TEXT("content"), Direct)) AppendAcpContentText(Direct, Out);

	// Shape 1: the MCP envelope, matched by its SIGNATURE rather than by `result` alone.
	// createMcpRawOutput(result, error) emits both keys or returns undefined, so `error`
	// being present is what makes this an MCP frame. `result` is then an object with
	// content, a bare array, or a plain string, depending on what the MCP server returned.
	const bool bIsMcpEnvelope = Obj->HasField(TEXT("error"));
	if (Out.IsEmpty() && bIsMcpEnvelope) AppendAcpValueText(Obj->TryGetField(TEXT("result")), Out);

	// Shapes 3 and 4: codex's own tools. Checked after `result` because these keys never
	// co-occur with it — an MCP frame has no `output`, a function-call frame has no
	// `result` — so the order only decides which miss is free.
	if (Out.IsEmpty()) AppendAcpValueText(Obj->TryGetField(TEXT("output")), Out);
	if (Out.IsEmpty()) AppendAcpValueText(Obj->TryGetField(TEXT("formatted_output")), Out);

	// Clamp the RESULT before the error is appended, against a ceiling that leaves the error
	// its own budget. Clamping the concatenation instead lets a large result delete the
	// failure message entirely.
	Out = ClampToolOutputText(Out, NwiroMaxToolOutputChars - NwiroToolErrorBudgetChars);

	// An error is a RESULT too — the most important one to show.
	if (const TSharedPtr<FJsonValue> Error = Obj->TryGetField(TEXT("error")))
	{
		FString ErrText;
		if (Error->Type == EJson::String)
		{
			ErrText = Error->AsString();
		}
		else if (Error->Type == EJson::Object)
		{
			if (!Error->AsObject()->TryGetStringField(TEXT("message"), ErrText))
			{
				Error->AsObject()->TryGetStringField(TEXT("text"), ErrText);
			}
		}
		if (!ErrText.IsEmpty())
		{
			if (!Out.IsEmpty()) Out += TEXT("\n");
			Out += ClampToolOutputText(ErrText, NwiroToolErrorBudgetChars);
		}
	}

	return Out;
}

void UNwiroIKBridge::Log(const FString& Text)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Text]()
	{
		UE_LOG(LogTemp, Log, TEXT("Nwiro [JS]: %s"), *Text);
	});
}

bool UNwiroIKBridge::IsTCPRunning() const { return FNwiroIKMCPServer::IsRunning(); }
bool UNwiroIKBridge::IsProcessing() const
{
	const FChatSession* S = ChatSessions.Find(ActiveChatId);
	return S ? S->bProcessing : false;
}
void UNwiroIKBridge::SetActiveChat(const FString& ChatId)
{
	ActiveChatId = ChatId;
	if (!ChatId.IsEmpty())
	{
		FChatSession& S = ChatSessions.FindOrAdd(ChatId);
		S.ChatId = ChatId;
		if (S.AdapterId.IsEmpty())
		{
			S.AdapterId = CurrentAdapter;
		}
		else if (S.AdapterId != CurrentAdapter)
		{
			// Moving this chat to a different adapter: B1 claims minted under the old
			// one are dead evidence. Purged here, while SessionId is still readable.
			if (!S.SessionId.IsEmpty()) RetireAcpPermissionState(FString(), S.SessionId);
			if (!S.SessionId.IsEmpty()) SessionToChatId.Remove(S.SessionId);
			ClearCodexModelBarrier(S);
			S.AdapterId = CurrentAdapter;
			S.SessionId.Empty();
			S.PendingMessage.Empty();
			S.PendingRetryImageBlocks.Empty();
			S.SessionRpcId = 0;
			S.PromptRpcId = 0;
			S.LastModel.Empty();
			S.AvailableModelIds.Empty();
			S.AvailableModeIds.Empty();
			S.BoundProcessGeneration = 0;
			S.SeenToolCallIds.Empty();
			// adapter-reliability-w6: the resume/replay state is per-adapter —
			// a prior session id from the old adapter is meaningless to the new
			// one, so wipe it alongside the rest of the session state.
			if (!S.PriorSessionId.IsEmpty()) SessionToChatId.Remove(S.PriorSessionId);
			S.PriorSessionId.Empty();
			S.AdapterVersionAtCreate.Empty();
			S.SessionLoadRpcId = 0;
			S.SessionLoadStartedAt = 0.0;
			S.bInReplay = false;
			S.bResumeAttempted = false;
			S.bResumeFailed = false;
		}
		// If adapter is initialized and session not created yet, create it now
		FAdapterProcess* AP = AdapterProcesses.Find(S.AdapterId);
		if (AP && AP->bInitialized && S.SessionId.IsEmpty())
		{
			TryCreateOrLoadSession(ChatId);
		}
	}
}
void UNwiroIKBridge::SetMode(const FString& Mode) { CurrentMode = Mode; }
void UNwiroIKBridge::SetModel(const FString& Model) { CurrentModel = Model; }
FString UNwiroIKBridge::GetMode() const { return CurrentMode; }
FString UNwiroIKBridge::GetModel() const { return CurrentModel; }
void UNwiroIKBridge::SetAdapter(const FString& Adapter) { CurrentAdapter = Adapter; }
FString UNwiroIKBridge::GetAdapter() const { return CurrentAdapter; }

// ONLY legacy Zed codex-acp needs a synthesised badge. The official codex-acp
// runtime emits its own ACP tool_call for MCP-origin calls (createMcpToolCallUpdate),
// so a synthetic badge would be a SECOND badge for the same call; claude and localllm
// have always badged via ACP tool_call too.
//
// Note the predicate is deliberately NOT `!UsesOfficialRuntime(...)`. That function
// means "Windows AND codex", so its complement selects claude, localllm, nwiro, empty
// and unknown adapters as well — every one of which would then get a duplicate badge.
// Both conjuncts are required.
//
// Computed here because UsesOfficialRuntime is TU-local, and because the frontend only
// ever sees the adapter NAME, which is "codex" on both the official and legacy runtimes.
void UNwiroIKBridge::GetMcpToolEventContext(
	FString& OutChatId,
	bool& bOutNeedsBadgeFallback) const
{
	OutChatId = ActiveChatId;

	// Policy from the chat that will RECEIVE this event, not from the global
	// CurrentAdapter. The two diverge by design during send setup — setadapter()
	// runs several awaits before setactivechat() — so a tool call routed to a
	// legacy-Codex chat could otherwise inherit a policy computed from an
	// incoming Claude/localllm chat and lose its only badge.
	// CurrentAdapter is the fallback only when the chat has no adapter recorded yet.
	const FChatSession* Session = ChatSessions.Find(OutChatId);
	bOutNeedsBadgeFallback = NwiroCodex::NeedsMcpBadgeFallbackForChat(
		Session ? Session->AdapterId : FString(),
		CurrentAdapter,
		PLATFORM_WINDOWS != 0);
}
void UNwiroIKBridge::AddImagePath(const FString& Path) { AddImagePathForChat(ActiveChatId, Path); }
void UNwiroIKBridge::AddImagePathForChat(const FString& ChatId, const FString& Path)
{
	PendingImagesByChat.FindOrAdd(ChatId).Add(Path);
}
void UNwiroIKBridge::RemoveImagePathForChat(const FString& ChatId, const FString& Path)
{
	if (TArray<FString>* Staged = PendingImagesByChat.Find(ChatId))
	{
		// RemoveSingle, not Remove: the composer removes ONE chip, and `Remove` deletes every
		// element equal to Path. Pick the same file twice, delete one chip, and both copies
		// silently un-staged. Duplicates are identical strings, so dropping the first match
		// rather than the nth is observationally the same — only the count matters, which is
		// why this needs no index plumbing and no change to the bridge signature.
		Staged->RemoveSingle(Path);
		if (Staged->Num() == 0) PendingImagesByChat.Remove(ChatId);
	}
}
void UNwiroIKBridge::ClearImages() { PendingImagesByChat.Remove(ActiveChatId); }

FChatSession* UNwiroIKBridge::GetActiveSession()
{
	return ChatSessions.Find(ActiveChatId);
}

FChatSession* UNwiroIKBridge::FindSessionByAcpId(const FString& AcpSessionId)
{
	FString* ChatId = SessionToChatId.Find(AcpSessionId);
	if (ChatId) return ChatSessions.Find(*ChatId);
	return nullptr;
}

FString UNwiroIKBridge::FindChatIdByRpcId(int32 RpcId)
{
	FString* ChatId = RpcToChatId.Find(RpcId);
	return ChatId ? *ChatId : ActiveChatId;
}

// ============================================================
// JSON helpers
// ============================================================

FString UNwiroIKBridge::JsonToString(const TSharedPtr<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	return Out;
}

// ============================================================
// Response queue (polled by frontend JS)
// ============================================================

void UNwiroIKBridge::EnqueueResponse(const FString& Type, const FString& Data)
{
	PushEvent(Type, Data, ActiveChatId);
}

void UNwiroIKBridge::EnqueueResponse(const FString& Type, const FString& Data, const FString& ChatId)
{
	PushEvent(Type, Data, ChatId);
}

// adapter-reliability: adapter-scoped capability event. Emitted on initialize
// success once the adapter's capabilities are parsed. Empty chatId marks this
// as adapter-scoped (not tied to a single chat) so the frontend can key the
// {adapter, loadSession} payload by adapter id.
void UNwiroIKBridge::EnqueueAdapterCapabilities(const FString& AdapterId)
{
	bool bSupportsLoadSession = false;
	FString AgentVersion;
	if (const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
	{
		bSupportsLoadSession = AP->bSupportsLoadSession;
		AgentVersion = AP->AgentVersion;
	}

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
	Payload->SetStringField(TEXT("adapter"), AdapterId);
	Payload->SetBoolField(TEXT("loadSession"), bSupportsLoadSession);
	Payload->SetStringField(TEXT("version"), AgentVersion);
	EnqueueResponse(TEXT("adapter_capabilities"), JsonToString(Payload), TEXT(""));
}

// adapter-reliability-w0: stage tracking. Records the stage on session +
// adapter, but only logs/notifies when the stage actually changes — multiple
// agent_message_chunk events all assert "streaming", and we don't want N log
// lines per turn for that. The Log-level emission feeds the UE Output Log so
// support can read the lifecycle without verbose-level filters.
void UNwiroIKBridge::MarkActiveTurnStreamed()
{
	// A :5353 MCP tool executing proves the live turn is producing activity —
	// mirror the ACP-side bHasStreamed latch (set on agent_message_chunk/tool_call
	// in HandleSessionUpdate) so a stray mid-turn classified stdout line (codex's
	// recovered "unauthorized") can't tear down a codex TOOL-FIRST turn whose only
	// activity so far was a direct-MCP tool call. Normally exactly one chat is
	// processing; scope to bProcessing so a between-turns chat is never marked.
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.bProcessing) Pair.Value.bHasStreamed = true;
	}
}

void UNwiroIKBridge::SetAdapterStage(const FString& AdapterId, const FString& ChatId, const FString& Stage)
{
	bool bChanged = false;
	if (!ChatId.IsEmpty())
	{
		if (FChatSession* Session = ChatSessions.Find(ChatId))
		{
			if (Session->LastAdapterStage != Stage)
			{
				Session->LastAdapterStage = Stage;
				bChanged = true;
			}
		}
	}
	if (!AdapterId.IsEmpty())
	{
		if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
		{
			if (AP->LastStage != Stage)
			{
				AP->LastStage = Stage;
				bChanged = true;
			}
		}
	}
	if (!bChanged) return;

	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: adapter stage adapter=%s chat=%s stage=%s"),
		AdapterId.IsEmpty() ? TEXT("<unset>") : *AdapterId,
		ChatId.IsEmpty() ? TEXT("<none>") : *ChatId,
		*Stage);
}

// adapter-reliability-w0: classified failure event. Records {stage, code,
// message} on the session so the JS-side watchdog (chat.ts) can read them
// if it later fires. Emits three frames for layered compatibility:
//   - adapter_error: rich payload, used by the new classified UI block.
//   - error: legacy text, used by older renderer paths.
//   - done: forces the chat out of "thinking..." regardless of which event
//     the renderer recognises — without it the spinner persists for 90s.
void UNwiroIKBridge::FailChatWithAdapterError(const FString& ChatId, const FString& AdapterId,
	const FString& Stage, const FString& Code, const FString& Message)
{
	UE_LOG(LogTemp, Warning,
		TEXT("Nwiro IK: adapter failure adapter=%s chat=%s stage=%s code=%s message=%s"),
		AdapterId.IsEmpty() ? TEXT("<unset>") : *AdapterId,
		ChatId.IsEmpty() ? TEXT("<none>") : *ChatId,
		*Stage, *Code, *Message);

	if (!ChatId.IsEmpty())
	{
		if (FChatSession* Session = ChatSessions.Find(ChatId))
		{
			Session->LastAdapterStage = Stage;
			Session->LastAdapterCode = Code;
			Session->LastAdapterError = Message;
			// Every classified failure is terminal for the current turn — including for
			// any permission it raised. This is the common funnel for creating_session,
			// first-token, sending_prompt, configuring_model and adapter-exit failures,
			// so settling here covers all of them at once instead of one call site each.
			++Session->TurnGeneration;
			PermissionCoordinator.RetireMatching(
				FString(), FString(), ChatId, EAcpPermissionOutcome::Retired);
			// Clear all request mappings and retained payloads here so a late response,
			// adapter restart, or initialize fan-out cannot resurrect hidden work.
			ClearChatRpcState(*Session);
			Session->PendingMessage.Empty();
			Session->PendingRetryImageBlocks.Empty();
			Session->bProcessing = false;
		}
	}

	if (ChatId.IsEmpty()) return;

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject);
	Payload->SetStringField(TEXT("adapter"), AdapterId);
	Payload->SetStringField(TEXT("stage"), Stage);
	Payload->SetStringField(TEXT("code"), Code);
	Payload->SetStringField(TEXT("message"), Message);
	EnqueueResponse(TEXT("adapter_error"), JsonToString(Payload), ChatId);
	EnqueueResponse(TEXT("error"), Message, ChatId);
	EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
}

// adapter-reliability-w2 codex-review-fix: shared RPC-state cleanup for any
// path that fails a chat (timeouts, classified stdout, process exit). Failing
// without this leaves SessionRpcId/PromptRpcId non-zero, which (a) lets the
// timeout ticker fire a second classified error for a chat that already
// errored, and (b) makes DoCreateSession's re-entry guard silently no-op the
// next retry. Caller is responsible for clearing PendingMessage / setting
// bRetriedSessionWithoutMcp because those decisions differ between retry and
// terminal-failure paths.
void UNwiroIKBridge::ClearCodexModelBarrier(FChatSession& Session)
{
	if (Session.ModelConfigRpcId != 0) RpcToChatId.Remove(Session.ModelConfigRpcId);
	Session.ModelConfigRpcId = 0;
	Session.ModelConfigRpcStartedAt = 0.0;
	Session.PendingConfigId.Empty();
	Session.PendingMode.Empty();
	Session.PendingModel.Empty();
	Session.PreparedPromptParams.Reset();
	Session.ModelConfigProcessGeneration = 0;
}

// Drop permission-lifecycle state that can no longer be valid: B1 claims AND un-clicked
// permission cards. Both are additionally revalidated at their point of use, so this is
// not the only thing standing between stale state and an approval — but state for a dead
// adapter or session has no reason to be reachable, and neither map should grow forever.
void UNwiroIKBridge::RetireAcpPermissionState(const FString& InAdapterId, const FString& InAcpSessionId)
{
	const FString SessionPrefix = InAcpSessionId.IsEmpty()
		? FString() : FString::Printf(TEXT("%s::"), *InAcpSessionId);

	for (auto It = NwiroMcpToolClaims.CreateIterator(); It; ++It)
	{
		if (!InAdapterId.IsEmpty() && It.Value().AdapterId != InAdapterId) continue;
		if (!SessionPrefix.IsEmpty() && !It.Key().StartsWith(SessionPrefix)) continue;
		It.RemoveCurrent();
	}

	// Cards are SETTLED, not dropped: the coordinator answers the agent on the wire while
	// the origin process is still live, and always tells the renderer the card is dead.
	// Removing them here (the previous behaviour) left the agent waiting forever on a
	// request nobody would answer, and the card visible with no way to resolve it.
	PermissionCoordinator.RetireMatching(
		InAdapterId, InAcpSessionId, FString(), EAcpPermissionOutcome::Retired);
}

void UNwiroIKBridge::ConfigurePermissionCoordinator()
{
	FAcpPermissionSinks CoordinatorSinks;
	CoordinatorSinks.RespondToAcp =
		[this](const FString& InAdapterId, const Acp::FRequestId& InReqId, const FString& InOptionId)
		{
			RespondToAcpPermission(InAdapterId.IsEmpty() ? CurrentAdapter : InAdapterId, InReqId, InOptionId);
		};
	CoordinatorSinks.EmitUiEvent =
		[this](const FString& InType, const FString& InPayload, const FString& InChatId)
		{
			// Explicit chat, never ActiveChatId: a background chat's outcome must land in
			// its own transcript.
			EnqueueResponse(InType, InPayload, InChatId);
		};
	CoordinatorSinks.NowSeconds = []() { return FPlatformTime::Seconds(); };

	FAcpPermissionLiveContext CoordinatorLive;
	CoordinatorLive.IsOriginLive = [this](const FAcpPermissionRequest& Request)
		{
			return IsPermissionOriginLive(Request);
		};
	CoordinatorLive.IsOriginProcessLive = [this](const FAcpPermissionRequest& Request)
		{
			return IsPermissionOriginProcessLive(Request);
		};

	PermissionCoordinator.Configure(MoveTemp(CoordinatorSinks), MoveTemp(CoordinatorLive));
}

bool UNwiroIKBridge::IsAcpSessionLive(const FString& InAcpSessionId) const
{
	if (InAcpSessionId.IsEmpty()) return false;
	const FString* MappedChatId = SessionToChatId.Find(InAcpSessionId);
	if (!MappedChatId) return false;
	// The reverse map alone is not enough: retirement paths clear FChatSession::SessionId
	// and the map entry independently. Require the chat to still claim THIS session.
	const FChatSession* Chat = ChatSessions.Find(*MappedChatId);
	return Chat && Chat->SessionId == InAcpSessionId;
}

// Is the OS process that issued this request still the live one for its adapter?
// Narrow on purpose: gates only whether a wire response is worth sending.
bool UNwiroIKBridge::IsPermissionOriginProcessLive(const FAcpPermissionRequest& Request) const
{
	const FAdapterProcess* Proc = AdapterProcesses.Find(Request.AdapterId);
	if (!Proc) return false;
	if (Proc->ProcessGeneration == 0 || Request.ProcessGeneration == 0) return false;
	if (Proc->ProcessGeneration != Request.ProcessGeneration) return false;
	// Generation alone can still name a process that has exited but not yet been swept.
	return Proc->Process.IsValid() && Proc->Process->IsRunning();
}

// The complete ownership tuple, answered atomically.
//
// Three independent facts — "generation is current", "session exists", "chat is on turn
// N" — can all be true of a request whose session belongs to a DIFFERENT chat, or whose
// chat has since moved to another adapter. Ownership is a relationship, and only the
// bridge holds every side of it, so it is proven here in one place rather than assembled
// from callbacks at the call site.
bool UNwiroIKBridge::IsPermissionOriginLive(const FAcpPermissionRequest& Request) const
{
	// 1. The issuing process is still running at the captured generation.
	if (!IsPermissionOriginProcessLive(Request)) return false;

	// 2. The session still resolves, and resolves to THIS chat — not merely to some chat.
	if (Request.AcpSessionId.IsEmpty() || Request.ChatId.IsEmpty()) return false;
	const FString* MappedChatId = SessionToChatId.Find(Request.AcpSessionId);
	if (!MappedChatId || *MappedChatId != Request.ChatId) return false;

	// 3. The chat still exists and still claims that exact session.
	const FChatSession* Chat = ChatSessions.Find(Request.ChatId);
	if (!Chat || Chat->SessionId != Request.AcpSessionId) return false;

	// 4. The chat is still on the adapter that issued the request. A chat that switched
	//    adapters keeps its id, so without this a card from the old adapter survives.
	const FString ChatAdapterId = Chat->AdapterId.IsEmpty() ? CurrentAdapter : Chat->AdapterId;
	if (ChatAdapterId != Request.AdapterId) return false;

	// 5. The captured turn is still the current one.
	if (Chat->TurnGeneration == 0 || Request.TurnGeneration == 0) return false;
	if (Chat->TurnGeneration != Request.TurnGeneration) return false;

	// 6. And that turn is still actually in flight. A turn that already ended has nothing
	//    left to authorize, even though its generation has not yet been superseded.
	return Chat->bProcessing;
}

void UNwiroIKBridge::ClearChatRpcState(FChatSession& Session)
{
	if (Session.SessionRpcId != 0) RpcToChatId.Remove(Session.SessionRpcId);
	if (Session.PromptRpcId != 0)  RpcToChatId.Remove(Session.PromptRpcId);
	ClearCodexModelBarrier(Session);
	Session.SessionRpcId = 0;
	Session.PromptRpcId = 0;
	Session.SessionRpcStartedAt = 0.0;
	Session.PromptRpcStartedAt = 0.0;
}

// adapter-reliability-w2: kill + reset an adapter process and trigger a
// fresh launch via EnsureProcess. Used by the session_new_timeout retry path
// to give MCP-isolation retry a clean adapter state. The OnCompleted of the
// outgoing process is guarded by a TWeakPtr identity check (Wave 1
// codex-review-fix) so the old completion can't remove the new entry.
//
void UNwiroIKBridge::ReleaseCodexRuntimeGuards(FAdapterProcess& AdapterProcess)
{
	if (AdapterProcess.bCodexShutdownUnproven)
	{
		// The OS closes this editor's managed-runtime lease at process exit.
		// Releasing it sooner would let another editor replace files that an
		// untracked descendant may still have mapped.
		return;
	}
#if PLATFORM_WINDOWS
	if (AdapterProcess.CodexProcessJobHandle)
	{
		// Never drop the final Job handle without first observing an empty job;
		// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE requests termination but does not
		// prove descendants have exited before managed files are touched.
		return;
	}
	NwiroCodex::ReleaseRuntimeFileLock(AdapterProcess.ManagedRuntimeUseLockHandle);
#else
	AdapterProcess.ManagedRuntimeUseLockHandle = nullptr;
	AdapterProcess.CodexProcessJobHandle = nullptr;
#endif
}

bool UNwiroIKBridge::StopCodexProcessTreeAndWait(
	FAdapterProcess& AdapterProcess,
	const FString& Reason)
{
#if PLATFORM_WINDOWS
	if (AdapterProcess.bCodexShutdownUnproven)
	{
		UE_LOG(LogTemp, Error,
			TEXT("Nwiro IK: refusing Codex shutdown because prior tree ownership is unproven (reason=%s)"),
			*Reason);
		AdapterProcess.LastStage = TEXT("launching_process");
		AdapterProcess.LastErrorCode = TEXT("runtime_shutdown_incomplete");
		AdapterProcess.LastError =
			TEXT("The previous Codex process tree could not be proven terminated.");
		return false;
	}

	if (!AdapterProcess.CodexProcessJobHandle)
	{
		if (!AdapterProcess.Process.IsValid())
		{
			ReleaseCodexRuntimeGuards(AdapterProcess);
			return true;
		}
		UE_LOG(LogTemp, Error,
			TEXT("Nwiro IK: refusing unsafe Codex shutdown without a process Job (reason=%s)"),
			*Reason);
		AdapterProcess.LastStage = TEXT("launching_process");
		AdapterProcess.LastErrorCode = TEXT("runtime_shutdown_incomplete");
		AdapterProcess.LastError =
			TEXT("The Codex process tree is not owned by a protected Windows process job.");
		return false;
	}

	if (!NwiroCodex::TerminateCodexProcessJobAndWait(
		AdapterProcess.CodexProcessJobHandle))
	{
		UE_LOG(LogTemp, Error,
			TEXT("Nwiro IK: Codex process tree did not terminate before timeout (reason=%s)"),
			*Reason);
		AdapterProcess.LastStage = TEXT("launching_process");
		AdapterProcess.LastErrorCode = TEXT("runtime_shutdown_incomplete");
		AdapterProcess.LastError =
			TEXT("The Codex process tree could not be terminated safely.");
		return false;
	}

	// The Job is now positively empty. Cancel tells FInteractiveProcess's
	// worker to stop; its destructor may overwrite bKillTree with false in
	// UE5.7, but that is harmless because no process remains in the Job.
	if (AdapterProcess.Process.IsValid())
	{
		AdapterProcess.Process->Cancel(true);
		AdapterProcess.Process.Reset();
	}
	AdapterProcess.bCodexShutdownUnproven = false;
	ReleaseCodexRuntimeGuards(AdapterProcess);
	return true;
#else
	if (AdapterProcess.Process.IsValid())
	{
		AdapterProcess.Process->Cancel(true);
		AdapterProcess.Process.Reset();
	}
	ReleaseCodexRuntimeGuards(AdapterProcess);
	return true;
#endif
}

bool UNwiroIKBridge::QuiesceCodexProcessForRuntimeChange(const FString& RuntimeSource)
{
	FAdapterProcess* AP = AdapterProcesses.Find(TEXT("codex"));
	if (!AP || AP->RuntimeSource != RuntimeSource)
	{
		return true;
	}

	// The process about to be replaced is the only one that could answer its cards.
	// Settle them while it is still alive rather than leaving them to expire.
	RetireAcpPermissionState(TEXT("codex"), FString());

	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: quiescing Codex ACP process for verified runtime change (source=%s)"),
		*RuntimeSource);
	if (!StopCodexProcessTreeAndWait(*AP, TEXT("managed_runtime_change")))
	{
		return false;
	}
	AP->bInitialized = false;
	AP->StdoutBuffer.Empty();
	AP->BytesFromStdout = 0;
	AP->InitRpcId = 0;
	AP->InitRpcStartedAt = 0.0;
	AP->LastStage.Empty();
	AP->LastError.Empty();
	AP->LastErrorCode.Empty();
	AP->RejectedAcpSessionIds.Empty();
	AP->AgentName.Empty();
	AP->AgentVersion.Empty();
	AP->ExpectedPackageVersion.Empty();
	AP->RuntimeSource.Empty();
	AP->NegotiatedProtocolVersion = 0.0;

	// Sessions minted by the stopped process cannot be reused by the runtime
	// selected after replacement. Sweep only Codex sessions; other adapters
	// and their active work remain untouched.
	for (auto& Pair : ChatSessions)
	{
		FChatSession& Session = Pair.Value;
		const FString SessionAdapter = Session.AdapterId.IsEmpty()
			? CurrentAdapter : Session.AdapterId;
		if (SessionAdapter != TEXT("codex")) continue;

		const bool bWasProcessing = Session.bProcessing;
		if (!Session.SessionId.IsEmpty()) SessionToChatId.Remove(Session.SessionId);
		ClearChatRpcState(Session);
		Session.SessionId.Empty();
		Session.PendingMessage.Empty();
		Session.bProcessing = false;
		Session.LastModel.Empty();
		Session.AvailableModelIds.Empty();
		Session.AvailableModeIds.Empty();
		Session.BoundProcessGeneration = 0;
		Session.LastSentImageBlocks.Empty();
		Session.PendingRetryImageBlocks.Empty();
		if (bWasProcessing)
		{
			EnqueueResponse(TEXT("system"),
				TEXT("Codex was stopped to safely update its integration runtime. Send the interrupted prompt again when the update finishes."),
				Pair.Key);
			EnqueueResponse(TEXT("done"), TEXT(""), Pair.Key);
		}
	}
	return true;
}

void UNwiroIKBridge::DoRestartAdapter(const FString& AdapterId, const FString& Reason)
{
	UE_LOG(LogTemp, Warning,
		TEXT("Nwiro IK: restarting adapter adapter=%s reason=%s"),
		*AdapterId, *Reason);

	// codex-review-fix: previously bailed when AdapterProcesses had no entry
	// for AdapterId, which is exactly the state after `adapter_exited` (the
	// OnCompleted path Removes the entry). The user-action restart button
	// would then no-op silently while JS reported success. Now we reset
	// per-entry state when one exists, then unconditionally call
	// EnsureProcess to spawn a fresh process — both "stuck" and "missing"
	// adapters end up in the same recovered state.
	if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
	{
		if (AdapterId == TEXT("codex"))
		{
			if (!StopCodexProcessTreeAndWait(*AP, Reason)) return;
		}
		else
		{
			if (AP->Process.IsValid()) AP->Process->Cancel(true);
			AP->Process.Reset();
		}
		AP->bInitialized = false;
		AP->StdoutBuffer.Empty();
		// adapter-reliability-w5: reset BytesFromStdout alongside the buffer —
		// it lives on the per-process struct and would otherwise carry across
		// the restart, producing negative bytesThisTurn deltas on the first
		// post-restart prompt (snapshot taken AFTER restart but counter is
		// still at the pre-restart cumulative).
		AP->BytesFromStdout = 0;
		AP->InitRpcId = 0;
		AP->InitRpcStartedAt = 0.0;
		AP->LastStage = TEXT("");
		AP->LastError = TEXT("");
		AP->LastErrorCode = TEXT("");
		AP->RestartCount++;
		// Fresh process, fresh id space — drop the rejected set so we don't
		// stale-match a session id the new process happens to mint.
		AP->RejectedAcpSessionIds.Empty();
	}

	// unknown-session-recovery: sweep per-chat ACP session ids for this
	// adapter — mirrors the OnCompleted exit sweep, which the restart path
	// never reaches (Process.Reset() above makes the old process's completion
	// fail the TWeakPtr identity check and bail). Ids minted by the killed
	// process are meaningless to its replacement; keeping them would wedge
	// every chat on "unknown session" after the restart. Clear, do NOT
	// blacklist (dead id space; the set was just emptied above), and clear
	// LastModel so DoSendPrompt re-pushes the model config to the fresh
	// session. Chats on other adapters are left untouched.
	const bool bPreservePendingTurn = Reason == TEXT("session_new_timeout");

	// B1 claims are bound to the process generation that minted them, so a survivor
	// could not authorize anything after this restart. Purge anyway: dead evidence
	// should not be reachable at all, and this map must not grow without bound.
	RetireAcpPermissionState(AdapterId, FString());

	for (auto& Pair : ChatSessions)
	{
		FChatSession& Swept = Pair.Value;
		const FString SweptAdapterId = Swept.AdapterId.IsEmpty() ? AdapterId : Swept.AdapterId;
		if (SweptAdapterId != AdapterId) continue;
		const bool bWasProcessing = Swept.bProcessing;
		if (!Swept.SessionId.IsEmpty()) SessionToChatId.Remove(Swept.SessionId);
		ClearChatRpcState(Swept);
		Swept.SessionId.Empty();
		Swept.LastModel.Empty();
		Swept.AvailableModelIds.Empty();
		Swept.AvailableModeIds.Empty();
		Swept.BoundProcessGeneration = 0;
		// unknown-session-recovery (image-aware): retained wire blocks minted
		// against the killed process belong to a dead turn — drop them (the
		// bLastSentHadImages flag deliberately survives so a recovery that
		// somehow still fires for that turn fails closed, not reduced).
		Swept.LastSentImageBlocks.Empty();
		Swept.PendingRetryImageBlocks.Empty();
		if (!bPreservePendingTurn)
		{
			Swept.PendingMessage.Empty();
			Swept.bProcessing = false;
			if (bWasProcessing)
			{
				EnqueueResponse(TEXT("system"),
					TEXT("Adapter restarted. The interrupted prompt was not sent; send it again when ready."),
					Pair.Key);
				EnqueueResponse(TEXT("done"), TEXT(""), Pair.Key);
			}
		}
	}

	// Same temp-CurrentAdapter trick StartAdapter uses — EnsureProcess only
	// manages CurrentAdapter, but we may be restarting a non-active one.
	// EnsureProcess uses FindOrAdd, so it works whether the entry existed or
	// we're starting from scratch.
	const FString PrevAdapter = CurrentAdapter;
	CurrentAdapter = AdapterId;
	EnsureProcess();
	CurrentAdapter = PrevAdapter;
}

// adapter-reliability-w1: idempotent ticker registration. The ticker fires
// once per second on the game thread (where ChatSessions/AdapterProcesses are
// otherwise mutated, so no extra synchronization is needed). Captures
// TWeakObjectPtr — without this, an editor hot-reload that GC's the bridge
// would leave a raw `this` in the global ticker queue and crash on next tick.
void UNwiroIKBridge::EnsureTimeoutTickerStarted()
{
	if (TimeoutTickerHandle.IsValid()) return;

	// The coordinator's sinks close over `this`, and this is the earliest point every
	// bridge entry path passes through. Configure is idempotent.
	ConfigurePermissionCoordinator();

	TWeakObjectPtr<UNwiroIKBridge> WeakThis(this);
	TimeoutTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float /*DeltaTime*/) -> bool
		{
			if (UNwiroIKBridge* Strong = WeakThis.Get())
			{
				Strong->CheckAdapterTimeouts();
				// A card nobody ever answers is a turn that never ends. Settling on a
				// deadline gives every request a terminal owner even when the user simply
				// walks away.
				Strong->PermissionCoordinator.SettleExpired();
				return true; // keep ticking
			}
			return false; // bridge GC'd — drop the ticker
		}),
		1.0f
	);
	UE_LOG(LogTemp, Log, TEXT("Nwiro IK: timeout ticker started"));
}

// adapter-reliability-w1: BeginDestroy override removes the ticker so the
// lambda's TWeakObjectPtr never even gets dereferenced post-destruction. We
// also clear the handle so a re-instantiation in the same session (rare but
// possible during dev) starts fresh.
void UNwiroIKBridge::BeginDestroy()
{
	// Clear the apiKey from memory before the UObject is GC'd. Defends
	// against memory-scrubbing on plugin hot-reload -- without this, a
	// detached UObject in the GC pipeline could be inspected via debugger
	// or dumped to a crash report.
	LocalLlmApiKey.Empty();

	// Nothing can answer a card after this point, and the expiry ticker is about to be
	// removed — so settle everything now. RetireAll rather than an empty RetireMatching:
	// global scope has to be stated, not arrived at by passing no filters.
	PermissionCoordinator.RetireAll(EAcpPermissionOutcome::Retired);

	if (TimeoutTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TimeoutTickerHandle);
		TimeoutTickerHandle.Reset();
	}
	if (FAdapterProcess* CodexProcess = AdapterProcesses.Find(TEXT("codex")))
	{
		StopCodexProcessTreeAndWait(*CodexProcess, TEXT("bridge_destroyed"));
	}
	Super::BeginDestroy();
}

// adapter-reliability-w1: per-RPC timeout enforcement. Three independent
// thresholds, one ticker. Order matters: initialize timeouts first (because
// session/new can't start until init succeeds), session/new before prompt
// (because a stuck session/new would block prompt anyway). On timeout, every
// state field that gates retry is cleared — without that, even with a
// classified error the chat would still be wedged behind SessionRpcId != 0.
void UNwiroIKBridge::CheckAdapterTimeouts()
{
	const double Now = FPlatformTime::Seconds();
	// Resolved live each tick (UI override → NWIRO_*_TIMEOUT_SECONDS env → default).
	// See the resolver definitions near the top of this file. FIRST_TOKEN is NOT
	// hoisted here: it is per-adapter (localllm vs cloud) and computed inside the
	// ChatSessions loop below. A function-scope copy here was previously shadowed
	// by that inner one, which silently made the first-token override dead.
	const double INITIALIZE_TIMEOUT  = GetInitializeTimeoutSeconds();
	const double SESSION_NEW_TIMEOUT = GetSessionNewTimeoutSeconds();

	// initialize timeouts — adapter process up but never replied to initialize.
	for (auto& Pair : AdapterProcesses)
	{
		FAdapterProcess& AP = Pair.Value;
		if (AP.bInitialized || AP.InitRpcId == 0 || AP.InitRpcStartedAt <= 0.0) continue;
		if (Now - AP.InitRpcStartedAt < INITIALIZE_TIMEOUT) continue;

		const FString AdapterId = Pair.Key;
		const int32 TimedOutId = AP.InitRpcId;
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: adapter timeout adapter=%s stage=initializing_acp rpc=%d elapsedMs=%d"),
			*AdapterId, TimedOutId, (int32)((Now - AP.InitRpcStartedAt) * 1000.0));

		// adapter-reliability-w1 codex-review-fix: terminate the stuck adapter
		// process. Without this, the next SendMessage hits EnsureProcess which
		// short-circuits on `Process.IsRunning()` and never re-sends initialize
		// — the chat gets a classified error but every retry queues a message
		// that can never reach an ACP session. Killing forces respawn on the
		// next send, which sends a fresh `initialize` with a new timestamp.
		bool bShutdownComplete = true;
		if (AdapterId == TEXT("codex"))
		{
			if (!StopCodexProcessTreeAndWait(AP, TEXT("initialize_timeout")))
			{
				bShutdownComplete = false;
			}
		}
		else if (AP.Process.IsValid())
		{
			AP.Process->Cancel(true);
			AP.Process.Reset();
		}
		AP.bInitialized = false;
		if (bShutdownComplete) AP.StdoutBuffer.Empty();
		// adapter-reliability-w5: mirror the BytesFromStdout reset done in
		// DoRestartAdapter — same rationale (counter survives the process
		// teardown and would corrupt the next session's diagnostic delta).
		if (bShutdownComplete) AP.BytesFromStdout = 0;
		AP.InitRpcId = 0;
		AP.InitRpcStartedAt = 0.0;
		RpcToChatId.Remove(TimedOutId);

		// Every chat that was waiting on this adapter's init is stranded.
		// Fail each individually — they may have different ChatIds the user
		// can retry from. Also clear PendingMessage so retry doesn't replay
		// a stale message into a fresh adapter.
		for (auto& SessPair : ChatSessions)
		{
			if (SessPair.Value.AdapterId == AdapterId && SessPair.Value.bProcessing)
			{
				SessPair.Value.PendingMessage.Empty();
				SessPair.Value.PendingRetryImageBlocks.Empty();
				FailChatWithAdapterError(
					SessPair.Key,
					AdapterId,
					TEXT("initializing_acp"),
					bShutdownComplete
						? TEXT("initialize_timeout")
						: TEXT("runtime_shutdown_incomplete"),
					bShutdownComplete
						? FString::Printf(
							TEXT("Adapter process started but never responded to initialize within %ds. The process has been terminated; the next send will respawn it."),
							(int32)INITIALIZE_TIMEOUT)
						: TEXT("The timed-out Codex process tree could not be terminated safely. Restart Unreal Editor before retrying."));
			}
		}
	}

	// adapter-reliability-w2 codex-review-fix: collect adapters that need a
	// restart instead of calling RestartAdapter inline. RestartAdapter calls
	// EnsureProcess synchronously, which sends an `initialize` RPC — and if
	// any path ever pumps the adapter's response synchronously (or a future
	// contributor adds one), HandleRpcResponse's init-success branch would
	// iterate ChatSessions and call DoCreateSession while we're still inside
	// the for-loop below. Even if today's UE event loop happens to keep that
	// safe, the assumption is fragile. Process pending restarts after the
	// loop ends. AddUnique because multiple chats sharing the same adapter
	// only need one restart.
	TArray<FString> AdaptersToRestart;

	// session/new + first-token timeouts. We iterate ChatSessions once and
	// branch — a session can't be in both states simultaneously (session/new
	// must succeed before session/prompt fires). `continue` after handling
	// session/new ensures we don't double-evaluate the same session this tick.
	for (auto& Pair : ChatSessions)
	{
		FChatSession& Session = Pair.Value;
		const FString ChatId = Pair.Key;
		const FString AdapterId = Session.AdapterId.IsEmpty() ? CurrentAdapter : Session.AdapterId;

		if (Session.SessionRpcId != 0 && Session.SessionId.IsEmpty()
			&& Session.SessionRpcStartedAt > 0.0
			&& Now - Session.SessionRpcStartedAt >= SESSION_NEW_TIMEOUT)
		{
			const int32 TimedOutId = Session.SessionRpcId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: adapter timeout adapter=%s chat=%s stage=creating_session rpc=%d elapsedMs=%d retried=%s"),
				*AdapterId, *ChatId, TimedOutId,
				(int32)((Now - Session.SessionRpcStartedAt) * 1000.0),
				Session.bRetriedSessionWithoutMcp ? TEXT("true") : TEXT("false"));

			// Clear in-flight state in either branch so DoCreateSession's
			// SessionRpcId-non-zero guard doesn't silently no-op later.
			ClearChatRpcState(Session);

			// adapter-reliability-w2 §5+§6: first session_new_timeout doesn't
			// fail the chat — it kicks off a restart + MCP-isolation retry.
			// If the second attempt also times out we land in the else branch
			// and surface the classified error.
			//
			// Why combine restart and MCP-isolation in one retry: we don't
			// know which is broken (adapter wedged vs MCP unhealthy), and
			// the restart cost is small relative to the MCP-isolation
			// information value. If MCP-less retry succeeds, MCP is the
			// likely blocker (notice emitted from HandleRpcResponse). If it
			// fails too, we know it's the adapter/auth/trust path.
			if (!Session.bRetriedSessionWithoutMcp)
			{
				Session.bRetriedSessionWithoutMcp = true;
				// Keep PendingMessage + bProcessing so the post-init auto-fire
				// of DoCreateSession (with bAttachMcp=false) followed by
				// DoSendPrompt picks up the queued message naturally.
				EnqueueResponse(TEXT("system"),
					AdapterId == TEXT("codex")
						? TEXT("Codex session creation timed out — restarting the integration and retrying...")
						: TEXT("Adapter session creation timed out — restarting and retrying without MCP attached..."),
					ChatId);
				// Defer the restart — see AdaptersToRestart comment above the loop.
				AdaptersToRestart.AddUnique(AdapterId);
				continue;
			}

			// Second timeout — both with and without MCP have failed. Reset
			// the retry flag so a future fresh send (after the user hits
			// retry) starts from default MCP-attached behaviour.
			Session.PendingMessage.Empty();
			Session.PendingRetryImageBlocks.Empty();
			Session.bProcessing = false;
			Session.bRetriedSessionWithoutMcp = false;

			FailChatWithAdapterError(ChatId, AdapterId, TEXT("creating_session"),
				TEXT("session_new_timeout"),
				AdapterId == TEXT("codex")
					? TEXT("Codex session creation timed out twice after a clean integration restart.")
					: TEXT("Adapter session creation timed out twice (with MCP and without). The adapter is likely wedged or Claude Code auth/trust is blocking it. Try Restart Adapter from Settings, or open this project in Claude Code once to grant trust."));
			continue;
		}

		// adapter-reliability-w6: session/load (resume) timeout. Mirrors the
		// session/new arm above but degrades silently to a fresh session rather
		// than failing the chat — a stuck resume is recoverable by starting
		// clean. SESSION_NEW_TIMEOUT is reused (resume lives in the same
		// creating_session lifecycle bucket). Do NOT re-arm: clear bInReplay +
		// SessionLoadRpcId/SessionLoadStartedAt (ClearChatRpcState does not touch
		// the load fields) and latch bResumeFailed so the fresh create can't
		// bounce back into another resume.
		if (Session.SessionLoadRpcId != 0 && Session.bInReplay
			&& Session.SessionLoadStartedAt > 0.0
			&& Now - Session.SessionLoadStartedAt >= SESSION_NEW_TIMEOUT)
		{
			const int32 TimedOutId = Session.SessionLoadRpcId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: adapter timeout adapter=%s chat=%s stage=resuming_session rpc=%d elapsedMs=%d"),
				*AdapterId, *ChatId, TimedOutId,
				(int32)((Now - Session.SessionLoadStartedAt) * 1000.0));

			RpcToChatId.Remove(TimedOutId);
			Session.SessionLoadRpcId = 0;
			Session.SessionLoadStartedAt = 0.0;
			Session.bInReplay = false;
			Session.bResumeFailed = true;

			TSharedPtr<FJsonObject> ResumeFailedPayload = MakeShareable(new FJsonObject);
			ResumeFailedPayload->SetStringField(TEXT("reason"), TEXT("timeout"));
			EnqueueResponse(TEXT("session_resume_failed"), JsonToString(ResumeFailedPayload), ChatId);

			DoCreateSession(ChatId);
			continue;
		}

		// Codex prompts are retained until the official ACP confirms both mode
		// and model. A timeout must destroy the retained params.
		if (Session.ModelConfigRpcId != 0
			&& Session.ModelConfigRpcStartedAt > 0.0
			&& Now - Session.ModelConfigRpcStartedAt >= NwiroCodex::ModelConfigTimeoutSeconds)
		{
			const int32 TimedOutId = Session.ModelConfigRpcId;
			const bool bModeStage = Session.PendingConfigId == TEXT("mode");
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: Codex configuration timed out adapter=%s chat=%s config=%s rpc=%d elapsedMs=%d"),
				*AdapterId, *ChatId, *Session.PendingConfigId, TimedOutId,
				(int32)((Now - Session.ModelConfigRpcStartedAt) * 1000.0));
			ClearCodexModelBarrier(Session);
			Session.bProcessing = false;
			FailChatWithAdapterError(ChatId, AdapterId,
				bModeStage ? TEXT("configuring_mode") : TEXT("configuring_model"),
				bModeStage ? TEXT("mode_config_timeout") : TEXT("model_config_timeout"),
				bModeStage
					? TEXT("Codex did not confirm the required safe mode, so the prompt was not sent.")
					: TEXT("Codex did not confirm the selected model, so the prompt was not sent."));
			continue;
		}

		// Per-adapter first-token timeout: local LLMs cold-load multi-GB
		// weights on first prompt after idle (Ollama default keepalive is
		// 5min — second-or-later prompts are warm). Cold-load takes 30-60s
		// disk read + VRAM/RAM allocation + prompt prefill before the model
		// can stream the first token. Cloud adapters (claude/codex) have
		// SLA-bounded latency and stay at 90s. Documented v1 trade-off: an
		// Ollama-side stall (process alive but inference wedged) now surfaces
		// after 300s instead of 90s. Shim process death is detected via the
		// process-exit path, so only this rare silent-hang case waits longer.
		// localllm honours the live override / NWIRO_FIRST_TOKEN_TIMEOUT_SECONDS
		// (default 300s — the cold-load ceiling); cloud adapters keep the tight
		// 90s SLA bound. This is the ONLY first-token enforcement site (the
		// function-scope hoist was removed to kill the shadowing that made the
		// override dead). Resolver order: UI override → env → 300s default.
		const double FIRST_TOKEN_TIMEOUT = (AdapterId == TEXT("localllm")) ? GetFirstTokenTimeoutSeconds() : 90.0;
		if (Session.PromptRpcId != 0 && Session.bProcessing
			&& Session.PromptRpcStartedAt > 0.0
			&& Now - Session.PromptRpcStartedAt >= FIRST_TOKEN_TIMEOUT)
		{
			const int32 TimedOutId = Session.PromptRpcId;
			// adapter-reliability-w5: bytes-during-this-turn diagnostic.
			//   0   → adapter went silent (crashed / pipe stuck / never wrote)
			//   >0  → adapter is/was writing; partition further by whether parsed
			//         events arrived (look at the surrounding stream lines)
			// -1 if AP lookup fails — should not happen in practice; logged for
			// completeness so a missing value is distinguishable from zero bytes.
			FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
			const int64 BytesThisTurn = AP ? (AP->BytesFromStdout - Session.BytesAtPromptSendStart) : -1;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: adapter timeout adapter=%s chat=%s stage=waiting_for_first_token rpc=%d elapsedMs=%d bytesThisTurn=%lld"),
				*AdapterId, *ChatId, TimedOutId,
				(int32)((Now - Session.PromptRpcStartedAt) * 1000.0),
				BytesThisTurn);

			// adapter-reliability-w1 codex-review-fix: tell the adapter to stop
			// processing the cancelled turn. Best-effort — the adapter may
			// already be hung, but well-behaved adapters will cease emitting
			// further chunks for this prompt.
			if (!Session.SessionId.IsEmpty())
			{
				TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
				CancelParams->SetStringField(TEXT("sessionId"), Session.SessionId);
				SendRpcNotification(AdapterId, TEXT("session/cancel"), CancelParams);
			}

			ClearChatRpcState(Session);
			Session.bProcessing = false;

			// adapter-reliability-w1 codex-review-fix: nuke the ACP session
			// mapping AND remember the id so HandleSessionUpdate's
			// bProcessing-fallback can't re-attach late chunks to a retry.
			// SessionToChatId.Remove alone isn't enough — that handler has a
			// "find first waiting chat with empty SessionId" branch that
			// would happily latch the stale id onto a fresh DoCreateSession-
			// in-flight chat. The rejected set lives on FAdapterProcess
			// (codex-review-fix-2) so adapter removal clears it for free.
			if (!Session.SessionId.IsEmpty())
			{
				// The turn died here. Settle its cards while the session id still
				// resolves — dropping them left the agent waiting on a request that
				// nothing would ever answer.
				++Session.TurnGeneration;
				RetireAcpPermissionState(FString(), Session.SessionId);
				if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
				{
					AP->RejectedAcpSessionIds.Add(Session.SessionId);
				}
				SessionToChatId.Remove(Session.SessionId);
				Session.SessionId.Empty();
			}

			FailChatWithAdapterError(ChatId, AdapterId, TEXT("waiting_for_first_token"),
				TEXT("first_token_timeout"),
				FString::Printf(TEXT("Adapter accepted the prompt but never produced a first response within %ds. The session has been reset; the next send will create a fresh one."),
					(int32)FIRST_TOKEN_TIMEOUT));
		}
	}

	// adapter-reliability-w2 codex-review-fix: process deferred restarts AFTER
	// the ChatSessions loop has completed. RestartAdapter → EnsureProcess
	// can fire `initialize` and indirectly trigger HandleRpcResponse, which
	// would mutate ChatSessions if it ran reentrantly. Doing the work here
	// keeps the loop body purely state-mutating and reentrancy-free.
	for (const FString& AdapterToRestart : AdaptersToRestart)
	{
		DoRestartAdapter(AdapterToRestart, TEXT("session_new_timeout"));
	}

	// mcp-health: verify the embedded MCP server is still bound; auto-rebind if it
	// dropped. Placed AFTER the AdaptersToRestart loop so Restart()'s HttpRouter
	// teardown can never interleave with the ChatSessions/AdapterProcesses iteration.
	CheckMcpServerHealth();
}

// mcp-health: liveness check + auto-rebind for the embedded MCP server, hung off
// the existing 1s game-thread ticker. Hysteresis (kMcpFailuresBeforeRebind) +
// exponential backoff (capped) prevent thrash; a re-entrancy guard plus the
// game-thread confinement keep Restart()'s router teardown safe. The call site is
// after the AdaptersToRestart loop, so the router Reset never runs mid-iteration.
void UNwiroIKBridge::RestartCodexForMcpPortChangeIfNeeded()
{
	FAdapterProcess* Codex = AdapterProcesses.Find(TEXT("codex"));
	const int32 CurrentPort = FNwiroIKMCPServer::GetPort();
	if (!Codex
		|| !Codex->Process.IsValid()
		|| !Codex->Process->IsRunning()
		|| Codex->CodexMcpPort <= 0
		|| CurrentPort <= 0
		|| Codex->CodexMcpPort == CurrentPort)
	{
		return;
	}

	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: restarting Codex after MCP port changed from %d to %d"),
		Codex->CodexMcpPort,
		CurrentPort);
	DoRestartAdapter(TEXT("codex"), TEXT("mcp_port_changed"));
}

void UNwiroIKBridge::CheckMcpServerHealth()
{
	const double Now = FPlatformTime::Seconds();

	if (FNwiroIKMCPServer::IsHealthy())
	{
		McpConsecutiveFailures = 0;
		McpNextHealthCheckAt = 0.0;
		return;
	}

	// In a post-failure backoff window: return BEFORE counting, so the failure
	// counter (which drives both the hysteresis gate and the backoff shift) cannot
	// inflate once per tick and collapse the exponential schedule. A recovery is
	// still detected by the IsHealthy() check above.
	if (Now < McpNextHealthCheckAt) return;

	// Require N consecutive (non-backoff) failing ticks before the first
	// destructive rebind, so a transient game-thread stall doesn't trigger one.
	++McpConsecutiveFailures;
	if (McpConsecutiveFailures < kMcpFailuresBeforeRebind) return;

	// No re-entrancy guard needed: this runs on the game thread, at most once per
	// second, and Restart() is synchronous and never re-enters the ticker.
	UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: MCP server unhealthy (running=%s) - rebinding (failures=%d)"),
		FNwiroIKMCPServer::IsRunning() ? TEXT("true") : TEXT("false"), McpConsecutiveFailures);

	if (FNwiroIKMCPServer::Restart())
	{
		UE_LOG(LogTemp, Log, TEXT("Nwiro IK: MCP server rebound on port %d"), FNwiroIKMCPServer::GetPort());
		RestartCodexForMcpPortChangeIfNeeded();
		McpConsecutiveFailures = 0;
		McpNextHealthCheckAt = 0.0;
	}
	else
	{
		const int32 Shift = FMath::Min(McpConsecutiveFailures - kMcpFailuresBeforeRebind + 1, kMcpMaxBackoffShift);
		const double BackoffSeconds = (double)(1 << Shift);
		McpNextHealthCheckAt = Now + BackoffSeconds;
		UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: MCP rebind failed - next attempt in %.0fs"), BackoffSeconds);
	}
}

// adapter-reliability-w4b: public UFUNCTION wrapper for the user-action
// "Restart Adapter" button in the chat error bubble. Reason tag distinguishes
// from automatic restart-on-timeout in telemetry/logs.
//
// codex-review-fix: returns bool so JS can render an honest toast. False
// covers two cases the previous void-return version couldn't surface to the
// user:
//   1. Preflight (Wave 0 binary check) refused to spawn — e.g. CLI missing.
//      AdapterProcesses entry exists but Process stays null.
//   2. FInteractiveProcess->Launch() failed at the OS level — e.g. permissions.
// In both cases JS now shows a "could not restart, see Settings" toast
// instead of falsely claiming success.
bool UNwiroIKBridge::RestartAdapter(const FString& Adapter)
{
	UE_LOG(LogTemp, Log, TEXT("Nwiro IK: user-triggered RestartAdapter adapter=%s"), *Adapter);
	DoRestartAdapter(Adapter, TEXT("user_action"));

	const FAdapterProcess* AP = AdapterProcesses.Find(Adapter);
	const bool bRunning = AP && AP->Process.IsValid() && AP->Process->IsRunning();
	UE_LOG(LogTemp, Log, TEXT("Nwiro IK: RestartAdapter result adapter=%s running=%s"),
		*Adapter, bRunning ? TEXT("true") : TEXT("false"));
	return bRunning;
}

FString UNwiroIKBridge::GetChatIdForToolUse(const FString& ToolUseId) const
{
	const FString* Found = ToolUseToChatId.Find(ToolUseId);
	return Found ? *Found : ActiveChatId;
}

void UNwiroIKBridge::PushEvent(const FString& Type, const FString& Data, const FString& ChatId)
{
	// Build the JS string on the calling thread (often a background pipe reader)
	// so the game thread only has to hand the ready-made string to CEF.
	FString EscapedData = Data;
	EscapedData.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedData.ReplaceInline(TEXT("'"), TEXT("\\'"));
	EscapedData.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	EscapedData.ReplaceInline(TEXT("\r"), TEXT("\\r"));

	FString EscapedChatId = ChatId;
	EscapedChatId.ReplaceInline(TEXT("'"), TEXT("\\'"));

	FString JSCode = FString::Printf(
		TEXT("window.dispatchEvent(new CustomEvent('ue:response', { detail: { type: '%s', data: '%s', chatId: '%s' } }));"),
		*Type, *EscapedData, *EscapedChatId
	);

	// ExecuteJavascript must run on the game thread (CEF requirement), but
	// the escaping + string formatting above already happened off-thread.
	// If we're already on the game thread, call directly to avoid a frame
	// delay; otherwise dispatch.
	if (IsInGameThread())
	{
		if (FNwiroIKPanel::WebBrowserWidget.IsValid())
		{
			FNwiroIKPanel::WebBrowserWidget->ExecuteJavascript(JSCode);
		}
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [JSCode]()
		{
			if (FNwiroIKPanel::WebBrowserWidget.IsValid())
			{
				FNwiroIKPanel::WebBrowserWidget->ExecuteJavascript(JSCode);
			}
		});
	}

}

FString UNwiroIKBridge::PollResponse()
{
	FScopeLock Lock(&ResponseLock);
	if (ResponseQueue.Num() == 0) return TEXT("[]");
	FString Result = TEXT("[");
	for (int32 i = 0; i < ResponseQueue.Num(); ++i)
	{
		if (i > 0) Result += TEXT(",");
		Result += ResponseQueue[i];
	}
	Result += TEXT("]");
	ResponseQueue.Empty();
	return Result;
}

// ============================================================
// Image save (base64 → temp file)
// ============================================================

FString UNwiroIKBridge::SaveImageBase64(const FString& Base64Data, const FString& FileName)
{
	FString Pure = Base64Data;
	int32 CommaIdx;
	if (Pure.FindChar(',', CommaIdx)) Pure = Pure.Mid(CommaIdx + 1);
	TArray<uint8> Decoded;
	FBase64::Decode(Pure, Decoded);
	if (Decoded.Num() == 0) return TEXT("");
	FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroImages"));
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*TempDir);
	FString FilePath = FPaths::Combine(TempDir, FileName);
	FFileHelper::SaveArrayToFile(Decoded, *FilePath);
	FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::MakePlatformFilename(FullPath);
	return FullPath;
}

void UNwiroIKBridge::OpenUrl(const FString& Url)
{
	if (!Url.IsEmpty()) FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
}

// Legacy entry point. Stages at pick time because renderers that predate the addressed
// picker skip `source:'picker'` images at send — they rely on this having happened.
void UNwiroIKBridge::OpenImagePicker() { OpenImagePickerImpl(ActiveChatId, /*bAddressed*/ false); }

// OpenFileDialog below is MODAL and can sit open for as long as the user browses. The
// owning chat is therefore taken as a parameter and captured here, at the moment the "+"
// button was pressed — resolving it after the dialog returns would attribute the photo to
// whichever conversation is open by then, and ActiveChatId is not even that: it is the
// last chat that SENT. Both the `image_added` event and the native staging are addressed
// to the chat named here.
void UNwiroIKBridge::OpenImagePickerForChat(const FString& ChatId)
{
	OpenImagePickerImpl(ChatId, /*bAddressed*/ true);
}

void UNwiroIKBridge::OpenImagePickerImpl(const FString& ChatId, bool bAddressed)
{
	const FString OwnerChatId = ChatId.IsEmpty() ? ActiveChatId : ChatId;

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform) return;

	TArray<FString> OutFiles;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	bool bOpened = DesktopPlatform->OpenFileDialog(
		ParentWindow,
		TEXT("Select Images"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Image Files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp"),
		EFileDialogFlags::Multiple,
		OutFiles
	);

	if (bOpened && OutFiles.Num() > 0)
	{
		for (const FString& FilePath : OutFiles)
		{
			// Read file, convert to base64, send to frontend as image_added event
			TArray<uint8> FileData;
			if (FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				FString Base64 = FBase64::Encode(FileData);
				FString FileName = FPaths::GetCleanFilename(FilePath);
				FString Ext = FPaths::GetExtension(FilePath).ToLower();
				FString MimeType = Ext == TEXT("png") ? TEXT("image/png") :
					Ext == TEXT("jpg") || Ext == TEXT("jpeg") ? TEXT("image/jpeg") :
					TEXT("image/bmp");

				// Build data URL
				FString DataUrl = FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *Base64);

				// Send to frontend
				TSharedPtr<FJsonObject> ImgInfo = MakeShareable(new FJsonObject);
				ImgInfo->SetStringField(TEXT("name"), FileName);
				ImgInfo->SetStringField(TEXT("path"), FilePath);
				ImgInfo->SetStringField(TEXT("preview"), DataUrl);
				// Whether the file is ALREADY staged natively.
				//
				// The addressed picker does NOT stage. Staging at pick time put the file
				// beyond the reach of everything the composer decides afterwards: the
				// per-model size/dimension check runs on the encoded preview and can REJECT
				// the image — no chip is shown, the user sees nothing — yet the hidden entry
				// stayed staged and went out with the next prompt. Removing a chip, picking
				// the same file twice, or a dialog that returns after the chat closed all
				// had the same shape.
				//
				// So the visible, validated tray is authoritative: the composer treats this
				// like any pasted image and the send path stages exactly what is on screen.
				// The legacy parameterless picker keeps staging, because renderers that
				// predate this flag skip picker images at send and would otherwise drop them.
				ImgInfo->SetBoolField(TEXT("nativeStaged"), !bAddressed);
				EnqueueResponse(TEXT("image_added"), JsonToString(ImgInfo), OwnerChatId);

				if (!bAddressed) AddImagePathForChat(OwnerChatId, FilePath);
			}
		}
	}
}

FString UNwiroIKBridge::GetPluginInfo() const
{
	TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject);
	FString PluginVersion = TEXT("unknown");
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NwiroIntegrationKit"));
	if (Plugin.IsValid()) PluginVersion = Plugin->GetDescriptor().VersionName;
	Info->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Info->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Info->SetStringField(TEXT("mcpProtocol"), TEXT("2025-03-26"));
	Info->SetNumberField(TEXT("mcpPort"), FNwiroIKMCPServer::GetPort());
	Info->SetBoolField(TEXT("mcpRunning"), FNwiroIKMCPServer::IsRunning());
	Info->SetStringField(TEXT("adapter"), CurrentAdapter);
	Info->SetStringField(TEXT("acpProtocol"), TEXT("ACP v1"));
	return JsonToString(Info);
}

// ============================================================
// ACP JSON-RPC send
// ============================================================

void UNwiroIKBridge::SendRaw(const FString& AdapterId, const FString& Json)
{
#if WITH_DEV_AUTOMATION_TESTS
	// Deliberately BEFORE the liveness check. Without a live adapter process every frame is
	// dropped here, so a spec that asserted on the wire would pass whether or not the code
	// under test built the right frame — or built one at all. The tap records what the
	// bridge DECIDED to send, which is the property those specs are about.
	if (TestOutboundTap) TestOutboundTap(AdapterId, Json);
#endif
	FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
	if (!AP || !AP->Process.IsValid() || !AP->Process->IsRunning()) return;
	// Off by default; enable in console with: Log LogTemp Verbose
	UE_LOG(LogTemp, Verbose, TEXT("ACP send [%s]: %s"), *AdapterId, *Json);
	AP->Process->SendWhenReady(Json + TEXT("\n"));
}

int32 UNwiroIKBridge::SendRpc(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	int32 Id = NextRpcId++;
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetNumberField(TEXT("id"), Id);
	Msg->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
		Msg->SetObjectField(TEXT("params"), Params);
	else
		Msg->SetObjectField(TEXT("params"), MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
	return Id;
}

void UNwiroIKBridge::SendRpcNotification(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
		Msg->SetObjectField(TEXT("params"), Params);
	SendRaw(AdapterId, JsonToString(Msg));
}

void UNwiroIKBridge::SendRpcResult(const FString& AdapterId, int32 Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Msg->SetNumberField(TEXT("id"), Id);
	Msg->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
}

// ─── codex-acp-string-id ─────────────────────────────────────────────────────
// Echo the original wire-type id back to the agent. Preserves Number | String |
// Null from the inbound `request` so JSON-RPC matching works on the peer side.
// ─────────────────────────────────────────────────────────────────────────────
void UNwiroIKBridge::SendRpcResult(const FString& AdapterId, const Acp::FRequestId& ReqId, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (ReqId.Value.IsValid())
		Msg->SetField(TEXT("id"), ReqId.Value);
	else
		Msg->SetField(TEXT("id"), MakeShareable(new FJsonValueNull()));
	Msg->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShareable(new FJsonObject));
	SendRaw(AdapterId, JsonToString(Msg));
}

void UNwiroIKBridge::SendRpcError(const FString& AdapterId, const Acp::FRequestId& ReqId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject);
	Msg->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (ReqId.Value.IsValid())
		Msg->SetField(TEXT("id"), ReqId.Value);
	else
		Msg->SetField(TEXT("id"), MakeShareable(new FJsonValueNull()));
	TSharedPtr<FJsonObject> ErrObj = MakeShareable(new FJsonObject);
	ErrObj->SetNumberField(TEXT("code"), Code);
	ErrObj->SetStringField(TEXT("message"), Message);
	Msg->SetObjectField(TEXT("error"), ErrObj);
	SendRaw(AdapterId, JsonToString(Msg));
}

// Build the ACP `selected` outcome envelope and ship it back to the adapter.
// Payload shape preserved as `outcome.outcome = "selected"` — this is the live
// adapter contract (claude-code-acp parses response.outcome.outcome). Do not
// rename to `outcome.type` even if a future spec disagrees.
void UNwiroIKBridge::RespondToAcpPermission(const FString& AdapterId, const Acp::FRequestId& ReqId, const FString& OptionId)
{
	TSharedPtr<FJsonObject> Outcome = MakeShareable(new FJsonObject);
	// An EMPTY option id means we have no option we can name — the agent offered a
	// vocabulary we could not classify, or none at all. `selected` with an empty optionId
	// is not a valid answer, and sending nothing leaves the adapter blocked forever on a
	// request nwiro has already forgotten.
	//
	// ACP's `cancelled` is the vocabulary-INDEPENDENT terminal response for exactly this:
	// it needs no option id, so schema drift can still be answered. Authorization-safe
	// (it grants nothing) and lifecycle-complete (the turn ends).
	if (OptionId.IsEmpty())
	{
		Outcome->SetStringField(TEXT("outcome"), TEXT("cancelled"));
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: answering permission with ACP 'cancelled' — no nameable option "
				 "(adapter vocabulary unrecognised or empty)"));
	}
	else
	{
		Outcome->SetStringField(TEXT("outcome"), TEXT("selected"));
		Outcome->SetStringField(TEXT("optionId"), OptionId);
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetObjectField(TEXT("outcome"), Outcome);

	SendRpcResult(AdapterId, ReqId, Result);
}

#if WITH_EDITOR
void UNwiroIKBridge::DebugInjectFrame(const FString& RawJsonLine)
{
	UE_LOG(LogTemp, Warning, TEXT("Nwiro: DebugInjectFrame: %s"), *RawJsonLine);
	ProcessLine(CurrentAdapter, RawJsonLine);
}
#endif

// ============================================================
// ACP protocol steps
// ============================================================

void UNwiroIKBridge::DoInitialize()
{
	// Identity and capabilities belong to one initialize handshake. Clear any
	// values retained on a reused process slot before parsing the new response;
	// a peer that omits protocolVersion must never inherit a prior valid 1.0.
	if (FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter))
	{
		AP->NegotiatedProtocolVersion = 0.0;
		AP->AgentName.Empty();
		AP->AgentVersion.Empty();
		AP->bSupportsLoadSession = false;
		AP->bSupportsImageInput = false;
	}

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetNumberField(TEXT("protocolVersion"), 1);

	// Client capabilities
	TSharedPtr<FJsonObject> Caps = MakeShareable(new FJsonObject);
	TSharedPtr<FJsonObject> FsCaps = MakeShareable(new FJsonObject);
	FsCaps->SetBoolField(TEXT("readTextFile"), true);
	FsCaps->SetBoolField(TEXT("writeTextFile"), false);
	Caps->SetObjectField(TEXT("fs"), FsCaps);
	// Stripping the terminal capability when shell is blocked removes shell
	// from the model's tool surface entirely — Codex can no longer choose
	// what isn't advertised. Prompt-only mitigation ("Do NOT run shell
	// commands") competes against the model's training prior; surface-level
	// removal is deterministic.
	//
	// Note: capability is fixed at ACP `initialize` time per spec — toggling
	// Block Shell mid-session requires adapter relaunch to take effect.
	// This is intentional; document in release notes.
	// Council (Phase-0 review): do NOT advertise terminal until the real exec path
	// + policy/approval enforcement exist (Phase 4) — otherwise enabling the toggle
	// would offer a terminal that create still denies (misleading). Stays false here;
	// Phase 4 flips this to `!bBlockCommandExecution`. Fixed at ACP initialize per spec,
	// so the toggle relaunches the adapter to re-advertise.
	Caps->SetBoolField(TEXT("terminal"), false);
	if (NwiroCodex::UsesOfficialRuntime(CurrentAdapter))
	{
		// The maintained successor uses this capability to choose the typed
		// boolean representation for session configuration options. Advertising
		// it also matches the ACP contract exercised by the Windows spike.
		TSharedPtr<FJsonObject> BooleanCaps = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> ConfigOptionCaps = MakeShareable(new FJsonObject);
		ConfigOptionCaps->SetObjectField(TEXT("boolean"), BooleanCaps);
		TSharedPtr<FJsonObject> SessionCaps = MakeShareable(new FJsonObject);
		SessionCaps->SetObjectField(TEXT("configOptions"), ConfigOptionCaps);
		Caps->SetObjectField(TEXT("session"), SessionCaps);
	}
	Params->SetObjectField(TEXT("clientCapabilities"), Caps);

	// Client info
	TSharedPtr<FJsonObject> ClientInfo = MakeShareable(new FJsonObject);
	ClientInfo->SetStringField(TEXT("name"), TEXT("nwiro"));
	ClientInfo->SetStringField(TEXT("title"), TEXT("Nwiro UE5"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

	// Inject localllm endpoint config into the ACP initialize params when the
	// active adapter is the local-LLM shim. Gated on CurrentAdapter so Claude
	// and Codex initialize messages stay free of unrelated fields. apiKey is
	// NOT in this object — it's delivered exclusively via the env var path
	// in EnsureProcess (see SetLocalLlmApiKey UFUNCTION).
	if (CurrentAdapter == TEXT("localllm")
		&& (!LocalLlmBaseUrl.IsEmpty() || !LocalLlmModel.IsEmpty()))
	{
		// Nesting: localLlm sits UNDER `context`, not at params root. The Rust
		// shim's serde struct (acp/messages.rs InitializeParams) reads from
		// params.context.localLlm; injecting at the root makes serde silently
		// drop the field and the shim falls back to its hardcoded defaults.
		// (Caught by reviewer audit — Bug B in SAVE-CONFIG-DEBUG.md.)
		TSharedPtr<FJsonObject> LocalLlmField = MakeShareable(new FJsonObject);
		LocalLlmField->SetStringField(TEXT("baseUrl"), LocalLlmBaseUrl);
		LocalLlmField->SetStringField(TEXT("model"), LocalLlmModel);
		TSharedPtr<FJsonObject> ContextField = MakeShareable(new FJsonObject);
		ContextField->SetObjectField(TEXT("localLlm"), LocalLlmField);
		Params->SetObjectField(TEXT("context"), ContextField);
	}

	// adapter-reliability-w0: stage = initializing_acp.
	SetAdapterStage(CurrentAdapter, ActiveChatId, TEXT("initializing_acp"));
	int32 RpcId = SendRpc(CurrentAdapter, TEXT("initialize"), Params);
	FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
	if (AP)
	{
		AP->InitRpcId = RpcId;
		// adapter-reliability-w1: arm initialize_timeout.
		AP->InitRpcStartedAt = FPlatformTime::Seconds();
	}
}

void UNwiroIKBridge::DoCreateSession(
	const FString& ChatId,
	bool bAttachMcp)
{
	FChatSession& Session = ChatSessions.FindOrAdd(ChatId);
	Session.ChatId = ChatId;
	if (Session.AdapterId.IsEmpty())
	{
		Session.AdapterId = CurrentAdapter;
	}

	// Re-entry guard. Without this, rapid setactivechat→sendmessage sequences
	// fire two session/new RPCs for the same chat: the first stores its RpcId
	// in Session.SessionRpcId, the second overwrites it, and the first
	// response can no longer be matched back to a chat — orphan ACP session.
	// SessionRpcId is cleared on the error path of HandleRpcResponse so a
	// failed session/new does not permanently lock the chat.
	if (!Session.SessionId.IsEmpty() || Session.SessionRpcId != 0)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: DoCreateSession skipped for chat %s — session already in flight or established (SessionRpcId=%d, SessionId=%s)"),
			*ChatId, Session.SessionRpcId,
			Session.SessionId.IsEmpty() ? TEXT("<empty>") : *Session.SessionId);
		return;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);
	const FString AdapterId =
		Session.AdapterId.IsEmpty() ? CurrentAdapter : Session.AdapterId;

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("cwd"), WorkingDir);

	// adapter-reliability-w2 §7: log MCP health right before session/new so
	// support can see in the UE log whether MCP was running + attached at the
	// moment session creation kicked off. Helpful when triaging session_new
	// failures — instantly distinguishes "MCP wasn't running" from "MCP was
	// running but hung the handshake."
	const bool bMcpRunning = FNwiroIKMCPServer::IsRunning();
	// The Windows official runtime receives the same endpoint through
	// process-local CODEX_CONFIG. Legacy Zed on macOS/Linux keeps the original
	// ACP mcpServers transport used before the official-runtime migration.
	const bool bUsesOfficialCodexRuntime =
		NwiroCodex::UsesOfficialRuntime(AdapterId);
	const bool bAttachSessionMcp =
		!bUsesOfficialCodexRuntime && bAttachMcp && bMcpRunning;
	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: MCP health before session/new running=%s port=%d attach=%s adapter=%s chat=%s"),
		bMcpRunning ? TEXT("true") : TEXT("false"),
		FNwiroIKMCPServer::GetPort(),
		bAttachSessionMcp ? TEXT("session") :
			(bUsesOfficialCodexRuntime ? TEXT("process") : TEXT("false")),
		*Session.AdapterId, *ChatId);

	TArray<TSharedPtr<FJsonValue>> McpServers;
	// adapter-reliability-w2 §6: bAttachMcp gate. After a session_new_timeout,
	// the retry sets this false so we can prove whether MCP attachment was the
	// blocker — if the retry succeeds without it, MCP is the issue.
	if (bAttachSessionMcp)
	{
		TSharedPtr<FJsonObject> McpServer = MakeShareable(new FJsonObject);
		McpServer->SetStringField(TEXT("type"), TEXT("http"));
		McpServer->SetStringField(TEXT("name"), TEXT("nwiro"));
		McpServer->SetStringField(TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FNwiroIKMCPServer::GetPort()));

		// No headers — MCP server is loopback-only, no auth needed.
		McpServer->SetArrayField(TEXT("headers"), TArray<TSharedPtr<FJsonValue>>());
		McpServers.Add(MakeShareable(new FJsonValueObject(McpServer)));
	}
	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	// Execute preamble — authored backend-side, pushed by the React app via
	// SetPromptPreambles. Falls back to a baked-in universal default when the
	// app hasn't pushed anything (e.g. backend was unreachable on first send).
	// Extensions snapshot + safety policy (below) are runtime state, not
	// authored prompt content — they stay inline.
	FString PromptText = CurrentExecutePreamble.IsEmpty()
		? FString(TEXT("You are Nwiro AI assistant embedded in Unreal Engine 5 Editor. ")
		          TEXT("Use Nwiro MCP tools for all UE work. Do not run shell commands. ")
		          TEXT("If a tool returns \"extension not enabled\", ask the user to enable it from the Extensions menu."))
		: CurrentExecutePreamble;

	// Capability-first prompt redesign (council verdict 2026-05-21):
	// the prior structure led with "## SHELL EXECUTION" prohibitions and
	// only named 4 extension-gated tools, causing local LLMs to describe
	// themselves as generic restricted assistants ("I cannot run shell
	// commands ... I can help with text processing"). The redesign teaches
	// capabilities first, gating second, prohibitions last — so the model
	// answers questions like "can you see the nwiro tools?" with real tool
	// names instead of disclaimers. Token cost is comparable (~600 chars)
	// because the SHELL EXECUTION section shrinks to a one-line footnote.
	PromptText += TEXT("\n\n## AVAILABLE NWIRO CAPABILITIES");
	PromptText += TEXT("\nYou are connected to real Nwiro tools that inspect and modify Unreal project state. Use them whenever the user's request maps to one. Tool names are case-sensitive.");
	PromptText += TEXT("\n- Blueprints: find_blueprints, read_blueprint, edit_blueprint, create_blueprint, find_blueprint_nodes.");
	PromptText += TEXT("\n- Actors and scene state: get_level_actors, spawn_actor, transform_actor, delete_actor, select_actor.");
	PromptText += TEXT("\n- Assets and materials: find_materials, inspect_material, create_material, edit_material, find_static_meshes, find_assets.");
	PromptText += TEXT("\n- Diagnostics: read_log, take_screenshot, get_world_settings, get_project_settings.");
	PromptText += TEXT("\n- Project files: read_file (read any file inside the project directory — always available, no extension needed).");
	PromptText += TEXT("\n- Editor automation: execute_python (run any editor command via the unreal Python module — most powerful tool).");
	PromptText += TEXT("\n- Optional extensions when enabled (see below): file writing/editing, 3D model generation.");
	PromptText += TEXT("\nIf the user asks what tools you have or what you can do, answer with these categories and name real tools — do NOT describe yourself as a generic assistant.");
	PromptText += TEXT("\nWhen a request matches a tool, prefer a tool call over a prose-only answer. If required arguments are missing, ask one short clarifying question. If a tool fails, report the failure briefly and try the closest safe alternative.");

	// Extensions can be toggled by the user at any moment during the conversation,
	// so the snapshot below may be stale by the time you read it. Always ATTEMPT
	// the tool when the user requests its functionality — the runtime gate will
	// either let it through or return an explicit "extension not enabled" error.
	// Only after that error appears should you ask the user to enable it.
	PromptText += TEXT("\n\n## EXTENSION TOOLS");
	PromptText += TEXT("\nThese optional write/generation tools are gated behind per-chat extensions that the user can toggle on/off in real time:");
	PromptText += FString::Printf(TEXT("\n- File Editor writes (write_file, delete_file, rename_file) — currently: %s"),
		IsChatExtensionEnabled(TEXT("fileEditor")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += TEXT("\n  read_file is not gated by File Editor and is always available for project files.");
	PromptText += FString::Printf(TEXT("\n- Meshy 3D (generate_3d_model_meshy) — currently: %s"),
		IsChatExtensionEnabled(TEXT("meshy")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += FString::Printf(TEXT("\n- Tripo 3D (generate_3d_model_tripo) — currently: %s"),
		IsChatExtensionEnabled(TEXT("tripo")) ? TEXT("ENABLED") : TEXT("disabled (try anyway if user asks)"));
	PromptText += TEXT("\nRule: when the user asks for one of these features, ALWAYS call the tool. If the tool returns an \"extension not enabled\" error, only THEN tell the user to enable it from the Extensions menu. Do NOT refuse or invent workarounds based on the snapshot above — it is only a hint and may be out of date.");

	PromptText += TEXT("\n\n## LIMITS");
	PromptText += TEXT("\nShell/terminal execution is unavailable in Nwiro. Never claim shell access. If the user asks to run shell commands, say shell is disabled and offer the closest Nwiro tool instead (for example, execute_python for Python-driven automation).");

	if (AdapterId == TEXT("claude") || AdapterId == TEXT("localllm"))
	{
		// Both Claude Code and the local-llm-acp shim (v0.1.13+) read the
		// system prompt from `session/new._meta.systemPrompt.append`. Sharing
		// the same wire shape keeps the adapter logic identical and avoids a
		// per-host divergence in PromptText handling.
		TSharedPtr<FJsonObject> Meta = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonObject> SystemPrompt = MakeShareable(new FJsonObject);
		SystemPrompt->SetStringField(TEXT("append"), PromptText);
		Meta->SetObjectField(TEXT("systemPrompt"), SystemPrompt);
		Params->SetObjectField(TEXT("_meta"), Meta);
	}
	// Codex deliberately omitted: codex-acp destructures `session/new` to
	// {cwd, mcp_servers, ..} and drops every other field, including
	// `instructions`. The model gets PromptText via the per-turn prepend
	// in DoSendPrompt instead — that path always reaches Codex.

	// adapter-reliability-w0: stage = creating_session.
	SetAdapterStage(AdapterId, ChatId, TEXT("creating_session"));
	Session.SessionRpcId = SendRpc(AdapterId, TEXT("session/new"), Params);
	// adapter-reliability-w1: arm session_new_timeout.
	Session.SessionRpcStartedAt = FPlatformTime::Seconds();
	RpcToChatId.Add(Session.SessionRpcId, ChatId);
}

// adapter-reliability-w6: session/load counterpart to DoCreateSession. Resumes
// Session.PriorSessionId on adapters that advertise loadSession instead of
// minting a fresh session. Gated by the caller (TryCreateOrLoadSession) but
// re-checks the cap + PriorSessionId here defensively. Params mirror
// DoCreateSession's {cwd, mcpServers} build and add the sessionId to resume.
void UNwiroIKBridge::DoLoadSession(const FString& ChatId)
{
	FChatSession& Session = ChatSessions.FindOrAdd(ChatId);
	Session.ChatId = ChatId;
	if (Session.AdapterId.IsEmpty())
	{
		Session.AdapterId = CurrentAdapter;
	}

	const FString AdapterId = Session.AdapterId.IsEmpty() ? CurrentAdapter : Session.AdapterId;

	// Cap-check gate: skip if the adapter can't load sessions or there's
	// nothing to resume. Either way fall back to a fresh session so the chat
	// is never left without one.
	const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
	if (!AP || !AP->bSupportsLoadSession || Session.PriorSessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: DoLoadSession skipped for chat %s — supportsLoad=%s prior=%s; falling back to session/new"),
			*ChatId, (AP && AP->bSupportsLoadSession) ? TEXT("true") : TEXT("false"),
			Session.PriorSessionId.IsEmpty() ? TEXT("<empty>") : *Session.PriorSessionId);
		DoCreateSession(ChatId);
		return;
	}

	// Re-entry guard, matching DoCreateSession: a session already in flight or
	// established must not fire a second load/new RPC for the same chat.
	if (!Session.SessionId.IsEmpty() || Session.SessionRpcId != 0 || Session.SessionLoadRpcId != 0)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: DoLoadSession skipped for chat %s — session already in flight or established (SessionRpcId=%d, SessionLoadRpcId=%d, SessionId=%s)"),
			*ChatId, Session.SessionRpcId, Session.SessionLoadRpcId,
			Session.SessionId.IsEmpty() ? TEXT("<empty>") : *Session.SessionId);
		return;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("sessionId"), Session.PriorSessionId);
	Params->SetStringField(TEXT("cwd"), WorkingDir);

	// Mirror DoCreateSession's platform-aware MCP transport split.
	const bool bMcpRunning = FNwiroIKMCPServer::IsRunning();
	const bool bUsesOfficialCodexRuntime =
		NwiroCodex::UsesOfficialRuntime(AdapterId);
	TArray<TSharedPtr<FJsonValue>> McpServers;
	if (!bUsesOfficialCodexRuntime && bMcpRunning)
	{
		TSharedPtr<FJsonObject> McpServer = MakeShareable(new FJsonObject);
		McpServer->SetStringField(TEXT("type"), TEXT("http"));
		McpServer->SetStringField(TEXT("name"), TEXT("nwiro"));
		McpServer->SetStringField(TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FNwiroIKMCPServer::GetPort()));

		// No headers — MCP server is loopback-only, no auth needed.
		McpServer->SetArrayField(TEXT("headers"), TArray<TSharedPtr<FJsonValue>>());
		McpServers.Add(MakeShareable(new FJsonValueObject(McpServer)));
	}
	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: session/load resuming sessionId=%s mcpRunning=%s adapter=%s chat=%s"),
		*Session.PriorSessionId, bMcpRunning ? TEXT("true") : TEXT("false"), *AdapterId, *ChatId);

	// adapter-reliability-w0: stage = creating_session (same lifecycle bucket;
	// the resume/replay events distinguish load from new for the frontend).
	SetAdapterStage(AdapterId, ChatId, TEXT("creating_session"));
	// Race-safety: arm bInReplay + SessionLoadStartedAt BEFORE SendRpc so an
	// inbound replay chunk that arrives between dispatch and the next line is
	// already classified as replay, not live.
	Session.bInReplay = true;
	Session.bResumeAttempted = true;
	Session.SessionLoadStartedAt = FPlatformTime::Seconds();
	Session.SessionLoadRpcId = SendRpc(AdapterId, TEXT("session/load"), Params);
	RpcToChatId.Add(Session.SessionLoadRpcId, ChatId);
	// PriorSessionId is the id being resumed and is already known, so map it to
	// this chat up-front (DoCreateSession can only do this on success, where the
	// new id is first learned).
	SessionToChatId.Add(Session.PriorSessionId, ChatId);
}

// adapter-reliability-w6: single decision point for every session-kickoff
// site. Resume via session/load when the adapter supports it, a prior session
// id is known, and no prior resume has failed; otherwise mint a fresh session.
// Reads bRetriedSessionWithoutMcp off the chat itself so the MCP-isolation
// retry signal survives (the init fan-out used to pass it explicitly).
void UNwiroIKBridge::TryCreateOrLoadSession(const FString& ChatId)
{
	FChatSession& Session = ChatSessions.FindOrAdd(ChatId);
	Session.ChatId = ChatId;
	if (Session.AdapterId.IsEmpty())
	{
		Session.AdapterId = CurrentAdapter;
	}

	const FAdapterProcess* AP = AdapterProcesses.Find(Session.AdapterId);
	// Existing persisted Codex sessions do not record which ACP family minted
	// them. Windows must not offer an archived-Zed id to the official successor;
	// macOS/Linux remain on Zed and retain their existing resume behavior.
	if (!NwiroCodex::UsesOfficialRuntime(Session.AdapterId)
		&& AP && AP->bSupportsLoadSession && !Session.PriorSessionId.IsEmpty() && !Session.bResumeFailed)
	{
		DoLoadSession(ChatId);
	}
	else
	{
		DoCreateSession(ChatId, !Session.bRetriedSessionWithoutMcp);
	}
}

void UNwiroIKBridge::DoSendPrompt(const FString& ChatId, const FString& Message)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (!Session) return;
	const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
	if (Session->SessionId.IsEmpty())
	{
		// adapter-reliability-w0: this used to silently return, leaving the
		// chat hanging at "thinking..." until the watchdog fired. Empty
		// SessionId here means session/new either never returned or returned
		// malformed — both are now caught upstream by the schema check, but
		// keep this guard as defence-in-depth and surface a classified error.
		FailChatWithAdapterError(ChatId, AdapterId, TEXT("sending_prompt"),
			TEXT("session_missing"),
			TEXT("Cannot send: session was never created (session/new returned no sessionId or failed earlier)."));
		return;
	}

	Session->bProcessing = true;
	// NOTE: TurnGeneration and ModeSnapshot are deliberately NOT captured here. This
	// function runs after an asynchronous session-creation boundary, and re-capturing
	// would read whatever CurrentMode has become in the meantime. SendMessage owns the
	// turn context; every attempt for that turn reuses it. A retry or unknown-session
	// recovery re-entering here must keep the ORIGINAL context, not mint a new one.
	// unknown-session-recovery: retain the raw outbound message so the
	// prompt-error arm of HandleRpcResponse can requeue it after recreating a
	// session the adapter no longer recognises. Overwritten on every send —
	// PromptRpcId matching at the error site guarantees the retained text is
	// the one that failed.
	Session->LastSentPrompt = Message;
	// Ensure session mapping exists
	if (!SessionToChatId.Contains(Session->SessionId))
		SessionToChatId.Add(Session->SessionId, ChatId);

	// Set model via config option — skip "default" (adapter uses its own default)
	// M-1 (describer trap): for localllm the shim grants the warmed tool-tier
	// only when the set_config model id matches the warmed id — and the warmed
	// id is LocalLlmModel (the value WarmupLocalLlm sent), NOT the generic
	// CurrentModel. Pushing CurrentModel (which may be empty, "default", or a
	// divergent string) leaves the shim session at tool-tier None, which STRIPS
	// every tool, so the model describes tools as code instead of calling them.
	// Push the concrete warmed id for localllm; other adapters keep CurrentModel.
	const FString ModelToPush =
		(AdapterId == TEXT("localllm") && !LocalLlmModel.IsEmpty()) ? LocalLlmModel : CurrentModel;
	// Council review (M-1): preferring LocalLlmModel is safe because it is the id
	// both DoInitialize and WarmupLocalLlm send (so the shim can only serve it),
	// and a real model switch runs through SetAdapterContext, which kills the shim
	// and clears Session->SessionId — the empty-session guard above then fails a
	// post-switch prompt before this push. Still, surface any CurrentModel vs
	// warmed-id divergence so a genuine selection-state bug is never silent.
	// Gate on a non-empty LocalLlmModel: only then is the warmed id actually the
	// value pushed (otherwise ModelToPush falls back to CurrentModel and the
	// message below would misstate what was sent) — council re-review accuracy fix.
	if (AdapterId == TEXT("localllm") && !LocalLlmModel.IsEmpty() && !CurrentModel.IsEmpty()
		&& CurrentModel != TEXT("default") && CurrentModel != LocalLlmModel)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: localllm CurrentModel='%s' != warmed LocalLlmModel='%s'; pushing the warmed id to keep tools enabled"),
			*CurrentModel, *LocalLlmModel);
	}

	const bool bUsesCodexConfigBarrier = AdapterId == TEXT("codex");
	const bool bUsesOfficialCodexRuntime =
		NwiroCodex::UsesOfficialRuntime(AdapterId);
	const FString CodexModeToPush = CurrentMode == TEXT("plan") ? TEXT("read-only") : TEXT("agent");
	if (bUsesCodexConfigBarrier)
	{
		if (!NwiroCodex::IsModelAllowedForRuntime(AdapterId, ModelToPush))
		{
			// This terminal failure happens before the normal one-shot image
			// consumption below. Discard THIS CHAT'S staged attachments now so
			// they cannot leak into the next otherwise-valid prompt.
			PendingImagesByChat.Remove(ChatId);
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_model"),
				TEXT("model_requires_official_runtime"),
				TEXT("GPT-5.6 requires the official Codex ACP runtime, which is currently supported on Windows only. The prompt was not sent."));
			return;
		}

		const FAdapterProcess* CodexProcess = AdapterProcesses.Find(AdapterId);
		if (!CodexProcess)
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_model"),
				TEXT("process_generation_mismatch"),
				TEXT("The active Codex process is no longer available, so the prompt was not sent."));
			return;
		}
		if (bUsesOfficialCodexRuntime
			&& !NwiroCodex::IsOfficialIdentity(
				CodexProcess->AgentName,
				CodexProcess->AgentVersion,
				CodexProcess->ExpectedPackageVersion,
				CodexProcess->NegotiatedProtocolVersion))
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_model"),
				TEXT("runtime_identity_mismatch"),
				TEXT("The active Codex runtime is not the supported official ACP package, so the prompt was not sent."));
			return;
		}
		if (Session->BoundProcessGeneration == 0
			|| Session->BoundProcessGeneration != CodexProcess->ProcessGeneration)
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_model"),
				TEXT("process_generation_mismatch"),
				TEXT("Codex restarted before the selected model could be confirmed, so the prompt was not sent."));
			return;
		}
		if (ModelToPush.IsEmpty() || ModelToPush == TEXT("default")
			|| !Session->AvailableModelIds.Contains(ModelToPush))
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_model"),
				TEXT("model_unavailable"),
				TEXT("This Codex session did not advertise the selected model, so the prompt was not sent."));
			return;
		}
		if (bUsesOfficialCodexRuntime
			&& !Session->AvailableModeIds.Contains(CodexModeToPush))
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("configuring_mode"),
				TEXT("mode_unavailable"),
				TEXT("This Codex session did not advertise the required safe mode, so the prompt was not sent."));
			return;
		}
	}
	else if (!ModelToPush.IsEmpty() && ModelToPush != TEXT("default") && ModelToPush != Session->LastModel)
	{
		Session->LastModel = ModelToPush;
		TSharedPtr<FJsonObject> ModelParams = MakeShareable(new FJsonObject);
		ModelParams->SetStringField(TEXT("sessionId"), Session->SessionId);
		ModelParams->SetStringField(TEXT("configId"), TEXT("model"));
		ModelParams->SetStringField(TEXT("value"), ModelToPush);
		SendRpc(AdapterId, TEXT("session/set_config_option"), ModelParams);
	}

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("sessionId"), Session->SessionId);

	TArray<TSharedPtr<FJsonValue>> Prompt;

	// Phase-1 capability gate: only emit image blocks when the adapter advertised
	// promptCapabilities.image at initialize. The shim degrades cleanly if images
	// are sent to a text-only model, so this is a bandwidth optimisation; it is
	// best-effort (a mid-session model swap after initialize can make the flag
	// stale — the shim's clean-degrade is the safety net then).
	bool bAdapterSupportsImages = true;
	if (const FAdapterProcess* GateAP = AdapterProcesses.Find(AdapterId))
	{
		bAdapterSupportsImages = GateAP->bSupportsImageInput;
	}

	// unknown-session-recovery (image-aware): pick this send's image payload.
	// The one-shot recovery replay resends the RETAINED wire blocks VERBATIM —
	// byte-identical to the failed send (re-reading paths could silently skip
	// a deleted temp file or ship different bytes) — and never touches the
	// bridge-global PendingImages, which another chat may have refilled since
	// the failure and which stays reserved for the next genuine send.
	// UnknownSessionRetryCount >= 1 here can only mean this send IS the
	// replay: every genuine send path resets the count in SendMessage before
	// any DoSendPrompt can run, and only the recovery arm increments it.
	TArray<TSharedPtr<FJsonValue>> ImageBlocks;
	if (Session->UnknownSessionRetryCount >= 1)
	{
		ImageBlocks = MoveTemp(Session->PendingRetryImageBlocks);
		Session->PendingRetryImageBlocks.Empty();
	}
	else
	{
		// This chat's staged attachments only — an interleaved attach in another
		// conversation is not part of this prompt.
		TArray<FString> ImagesToSend;
		if (TArray<FString>* Staged = PendingImagesByChat.Find(ChatId))
		{
			ImagesToSend = MoveTemp(*Staged);
		}
		PendingImagesByChat.Remove(ChatId);
		if (bAdapterSupportsImages)
		{
			for (const FString& ImgPath : ImagesToSend)
			{
				TArray<uint8> FileData;
				if (FFileHelper::LoadFileToArray(FileData, *ImgPath))
				{
					// Derive the MIME type from the file extension (mirrors the picker
					// at OpenImagePicker) instead of hardcoding image/png — otherwise a
					// JPEG/BMP is mislabelled and a strict vision backend may reject or
					// mis-decode the data URL the shim builds from this.
					const FString Ext = FPaths::GetExtension(ImgPath).ToLower();
					const FString MimeType =
						Ext == TEXT("png") ? TEXT("image/png") :
						(Ext == TEXT("jpg") || Ext == TEXT("jpeg")) ? TEXT("image/jpeg") :
						Ext == TEXT("webp") ? TEXT("image/webp") :
						Ext == TEXT("gif") ? TEXT("image/gif") :
						TEXT("image/bmp");
					TSharedPtr<FJsonObject> ImgBlock = MakeShareable(new FJsonObject);
					ImgBlock->SetStringField(TEXT("type"), TEXT("image"));
					ImgBlock->SetStringField(TEXT("data"), FBase64::Encode(FileData));
					ImgBlock->SetStringField(TEXT("mimeType"), MimeType);
					ImageBlocks.Add(MakeShareable(new FJsonValueObject(ImgBlock)));
				}
			}
		}
		else if (ImagesToSend.Num() > 0)
		{
			UE_LOG(LogTemp, Log,
				TEXT("Nwiro IK: adapter '%s' does not advertise image support (promptCapabilities.image=false); dropping %d pending image(s)"),
				*AdapterId, ImagesToSend.Num());
		}
	}

	// Retain the EXACT wire blocks (and whether any went out) so a recovery
	// resend is byte-identical — or fails closed in the recovery arm when the
	// retention was lost. The blocks are immutable after build, so sharing
	// the pointers between the retained list and the outgoing prompt is safe.
	Session->LastSentImageBlocks = ImageBlocks;
	Session->bLastSentHadImages = ImageBlocks.Num() > 0;

	for (const TSharedPtr<FJsonValue>& ImgBlock : ImageBlocks)
	{
		Prompt.Add(ImgBlock);
	}

	// Build the user-text block. Two prepends, in order:
	//   1. ExecutePreamble (Codex only): codex-acp dropped session/new
	//      `instructions`, so this is the only path that reaches the model
	//      with our authored system prompt. Claude already got it via
	//      session/new._meta.systemPrompt.append in DoCreateSession; adding
	//      it here too would just double the tokens.
	//   2. PlanPreamble (both adapters, when CurrentMode == "plan"): per-turn
	//      reminder that tools are denied this turn so the model produces a
	//      plan instead of retry-looping when the host gate rejects calls.
	FString PromptText = Message;
	if (CurrentMode == TEXT("plan"))
	{
		const FString PlanText = CurrentPlanPreamble.IsEmpty()
			? FString(TEXT("[PLAN MODE] Do not call any tools — they will be denied. ")
			          TEXT("Describe a step-by-step plan from your UE5 knowledge. ")
			          TEXT("Ask the user to disable Plan Mode to apply."))
			: CurrentPlanPreamble;
		PromptText = PlanText + TEXT("\n\n---\nUser message:\n") + PromptText;
	}
	// Skip the execute prepend in Plan Mode: it tells the model to use Nwiro
	// MCP tools, which contradicts the plan-mode "do not call any tools" rule
	// and was confusing Codex into hedging instead of producing a real plan.
	if (AdapterId == TEXT("codex")
		&& CurrentMode != TEXT("plan")
		&& !CurrentExecutePreamble.IsEmpty())
	{
		PromptText = CurrentExecutePreamble + TEXT("\n\n---\n") + PromptText;
	}
	PromptText = TEXT("[SHELL DISABLED] Shell execution is disabled in Nwiro. ")
		TEXT("If the user asks whether you can run shell commands, say shell execution is disabled in Nwiro. ")
		TEXT("Do not claim you can run shell commands.")
		TEXT("\n\n---\n") + PromptText;

	TSharedPtr<FJsonObject> TextBlock = MakeShareable(new FJsonObject);
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), PromptText);
	Prompt.Add(MakeShareable(new FJsonValueObject(TextBlock)));
	Params->SetArrayField(TEXT("prompt"), Prompt);

	// v0.2.0: populate OpenAI-spec `tools` array for the localllm
	// adapter. claude-acp and codex-acp self-discover tools via MCP
	// `tools/list` on the in-process FNwiroIKMCPServer — they don't
	// need this push-style delivery. The local-llm-acp shim (v0.1.17+)
	// consumes the OpenAI `tools` field to (a) populate its
	// tool_names allow-list for the Emulated-tier parser, (b) trigger
	// the format-directive system message. Without this push,
	// tool_names is empty in the shim and every Emulated tool
	// invocation silently drops — exactly the failure mode the user
	// observed in the v0.1.19 LM Studio Qwen test.
	//
	// Shape transformation: GetToolDefinitions() returns the MCP
	// shape `[{name, description, inputSchema}]`. The OpenAI chat-
	// completions endpoint expects `[{type: "function", function:
	// {name, description, parameters}}]`. We wrap each MCP entry
	// per OpenAI's spec.
	//
	// Plan mode suppresses tools entirely (matches the prose-level
	// `CurrentPlanPreamble` that tells the model "do not call any
	// tools"). Without this gate, Plan Mode would send tools and
	// then contradict itself via the preamble — confusing the model.
	if (AdapterId == TEXT("localllm") && CurrentMode != TEXT("plan"))
	{
		const FString McpToolsJson = GetToolDefinitions();
		TSharedPtr<FJsonValue> McpToolsValue;
		TSharedRef<TJsonReader<>> ToolsReader = TJsonReaderFactory<>::Create(McpToolsJson);
		if (FJsonSerializer::Deserialize(ToolsReader, McpToolsValue)
			&& McpToolsValue.IsValid()
			&& McpToolsValue->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& McpTools = McpToolsValue->AsArray();
			TArray<TSharedPtr<FJsonValue>> OpenAiTools;
			OpenAiTools.Reserve(McpTools.Num());

			// localllm ToolSelector: size and filter the pushed tool array per
			// the warmup-reported tier. ToolBudget == -1 is the native no-op:
			// we iterate the full McpTools array exactly as before (no
			// selection, byte-for-byte unchanged). For emulated/none we compute
			// the surviving indices once and wrap ONLY those — we never build
			// ~220 JSON objects just to discard most of them. The raw user
			// `Message` (not the [SHELL DISABLED]-prefixed PromptText) is the
			// BM25 query so scores aren't skewed toward shell/plan tokens.
			const int32 ToolBudget = NwiroToolSelector::ResolveToolBudget(LocalLlmToolTier);
			TArray<int32> KeepIndices;
			const bool bFilterTools = (ToolBudget != -1);
			if (bFilterTools)
			{
				NwiroToolSelector::SelectTools(Message, McpTools, ToolBudget, KeepIndices);
			}
			const int32 NumToWrap = bFilterTools ? KeepIndices.Num() : McpTools.Num();

			for (int32 WrapSlot = 0; WrapSlot < NumToWrap; ++WrapSlot)
			{
				const int32 McpIdx = bFilterTools ? KeepIndices[WrapSlot] : WrapSlot;
				const TSharedPtr<FJsonValue>& McpToolValue = McpTools[McpIdx];
				if (!McpToolValue.IsValid() || McpToolValue->Type != EJson::Object)
					continue;
				const TSharedPtr<FJsonObject> McpTool = McpToolValue->AsObject();
				if (!McpTool.IsValid()) continue;

				FString Name;
				if (!McpTool->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
					continue;

				TSharedPtr<FJsonObject> FunctionSpec = MakeShareable(new FJsonObject);
				FunctionSpec->SetStringField(TEXT("name"), Name);

				FString Description;
				if (McpTool->TryGetStringField(TEXT("description"), Description))
				{
					FunctionSpec->SetStringField(TEXT("description"), Description);
				}

				const TSharedPtr<FJsonObject>* InputSchemaPtr = nullptr;
				if (McpTool->TryGetObjectField(TEXT("inputSchema"), InputSchemaPtr)
					&& InputSchemaPtr != nullptr)
				{
					FunctionSpec->SetObjectField(TEXT("parameters"), *InputSchemaPtr);
				}

				TSharedPtr<FJsonObject> ToolWrapper = MakeShareable(new FJsonObject);
				ToolWrapper->SetStringField(TEXT("type"), TEXT("function"));
				ToolWrapper->SetObjectField(TEXT("function"), FunctionSpec);
				OpenAiTools.Add(MakeShareable(new FJsonValueObject(ToolWrapper)));
			}

			if (OpenAiTools.Num() > 0)
			{
				Params->SetArrayField(TEXT("tools"), OpenAiTools);
				UE_LOG(LogTemp, Verbose,
					TEXT("Nwiro IK: localllm session/prompt — pushed %d tools to adapter (tier='%s' budget=%d)"),
					OpenAiTools.Num(),
					LocalLlmToolTier.IsEmpty() ? TEXT("<empty→emulated>") : *LocalLlmToolTier,
					ToolBudget);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: failed to parse GetToolDefinitions() JSON; localllm session/prompt will have no tools field"));
		}
	}

	if (bUsesCodexConfigBarrier)
	{
		// Retain the fully-built immutable prompt while the selected model is
		// confirmed. The Windows official runtime additionally confirms its
		// safe mode first; legacy Zed keeps its ACP auto mode and proceeds
		// directly to the exact model acknowledgement.
		ClearCodexModelBarrier(*Session);
		Session->PendingConfigId =
			bUsesOfficialCodexRuntime ? TEXT("mode") : TEXT("model");
		Session->PendingMode =
			bUsesOfficialCodexRuntime ? CodexModeToPush : TEXT("");
		Session->PendingModel = ModelToPush;
		Session->PreparedPromptParams = Params;
		if (const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
			Session->ModelConfigProcessGeneration = AP->ProcessGeneration;
		SetAdapterStage(AdapterId, ChatId,
			bUsesOfficialCodexRuntime ? TEXT("configuring_mode") : TEXT("configuring_model"));

		TSharedPtr<FJsonObject> ConfigParams = MakeShareable(new FJsonObject);
		ConfigParams->SetStringField(TEXT("sessionId"), Session->SessionId);
		ConfigParams->SetStringField(
			TEXT("configId"),
			bUsesOfficialCodexRuntime ? TEXT("mode") : TEXT("model"));
		ConfigParams->SetStringField(
			TEXT("value"),
			bUsesOfficialCodexRuntime ? CodexModeToPush : ModelToPush);
		Session->ModelConfigRpcId = SendRpc(
			AdapterId, TEXT("session/set_config_option"), ConfigParams);
		Session->ModelConfigRpcStartedAt = FPlatformTime::Seconds();
		RpcToChatId.Add(Session->ModelConfigRpcId, ChatId);
		return;
	}

	DispatchPromptParams(ChatId, Params);
}

void UNwiroIKBridge::DispatchPromptParams(const FString& ChatId, const TSharedPtr<FJsonObject>& Params)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (!Session || !Params.IsValid()) return;
	const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
	if (!Session->bProcessing)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: dropping prepared prompt for inactive chat=%s adapter=%s"),
			*ChatId, *AdapterId);
		return;
	}

	FString PreparedSessionId;
	Params->TryGetStringField(TEXT("sessionId"), PreparedSessionId);
	if (PreparedSessionId.IsEmpty() || PreparedSessionId != Session->SessionId)
	{
		FailChatWithAdapterError(ChatId, AdapterId, TEXT("sending_prompt"),
			TEXT("prepared_session_mismatch"),
			TEXT("The prepared prompt no longer belongs to the active session, so it was not sent."));
		return;
	}

	if (AdapterId == TEXT("codex"))
	{
		const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
		if (!AP || Session->BoundProcessGeneration == 0
			|| Session->BoundProcessGeneration != AP->ProcessGeneration)
		{
			FailChatWithAdapterError(ChatId, AdapterId, TEXT("sending_prompt"),
				TEXT("process_generation_mismatch"),
				TEXT("Codex restarted before it accepted the prompt, so the prompt was not sent."));
			return;
		}
	}

	// adapter-reliability-w0: stage = sending_prompt.
	SetAdapterStage(AdapterId, ChatId, TEXT("sending_prompt"));
	Session->PromptRpcId = SendRpc(AdapterId, TEXT("session/prompt"), Params);
	// adapter-reliability-w1: arm first_token_timeout. Cleared either when
	// the first agent_message_chunk arrives (HandleSessionUpdate) or when the
	// prompt response itself returns (HandleRpcResponse — for adapters that
	// never stream and reply in one shot).
	Session->PromptRpcStartedAt = FPlatformTime::Seconds();
	// F4: new turn — clear the streamed-activity latch so a genuine PRE-streaming
	// failure (auth/trust/MCP at session start) still classifies. This helper is
	// the sole session/prompt sender, so this reset covers every turn start,
	// including the unknown-session-recovery resends that route back through here.
	Session->bHasStreamed = false;
	// adapter-reliability-w5: snapshot the adapter's lifetime byte counter at
	// the exact moment we arm the timeout, so the ticker can compute
	// bytes-during-this-turn = AP->BytesFromStdout - Session->BytesAtPromptSendStart.
	// AP lookup is cheap (TMap) and we're already past the SendRpc hot path.
	if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
	{
		Session->BytesAtPromptSendStart = AP->BytesFromStdout;
	}
	RpcToChatId.Add(Session->PromptRpcId, ChatId);
}

// ============================================================
// Process stdout — parse ACP messages
// ============================================================

void UNwiroIKBridge::ProcessLine(const FString& AdapterId, const FString& Line)
{
	if (Line.IsEmpty()) return;
	// Off by default; enable in console with: Log LogTemp Verbose
	UE_LOG(LogTemp, Verbose, TEXT("ACP recv: %s"), *Line);

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		// Some adapters (codex-acp v0.16.0+) stream Rust `tracing`/otel telemetry to STDOUT
		// — ANSI-colored spans, or Rust Debug structs that print as "{ ident: Value }" (a space
		// after the brace + unquoted keys) — interleaved with the real JSON-RPC. That floods the
		// log at Warning every turn. Treat those as adapter telemetry → Verbose; genuine non-JSON
		// (no ANSI, not a "{ " debug struct) still Warns. Real JSON-RPC frames start with `{"`
		// (quoted key, no space) so they never match and are parsed above. Classification below
		// runs either way, so a failure phrase carried on a telemetry line is never missed.
		const bool bAdapterTelemetry = Line.Contains(TEXT("\x1b")) || Line.StartsWith(TEXT("{ "));
		if (bAdapterTelemetry)
		{
			UE_LOG(LogTemp, Verbose, TEXT("Nwiro IK: ACP stdout (adapter telemetry, non-JSON): %s"), *Line);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: ACP stdout line was not JSON: %s"), *Line);
		}

		// adapter-reliability-w2 §8: try to classify the bare-text line. Many
		// adapter failure paths (auth, trust, MCP, launch) print a recognisable
		// phrase to stdout/stderr before any RPC could time out — catching them
		// here gives the user a specific code in 0s instead of 30s.
		const FStdoutKeywordPattern* Match = MatchStdoutKeyword(Line.ToLower());
		if (Match)
		{
			// Snapshot processing chats so we can iterate while
			// FailChatWithAdapterError mutates each session's bProcessing.
			//
			// codex-review-fix: filter to chats whose adapter actually
			// produced this line. Without the filter, a Claude "unauthorized"
			// stdout line would also fail any concurrent Codex chat — wrong
			// adapter id pinned to the wrong failure.
			TArray<FString> ProcessingChats;
			for (auto& Pair : ChatSessions)
			{
				if (!Pair.Value.bProcessing) continue;
				const FString SessionAdapterId = Pair.Value.AdapterId.IsEmpty()
					? CurrentAdapter : Pair.Value.AdapterId;
				if (SessionAdapterId != AdapterId) continue;
				ProcessingChats.Add(Pair.Key);
			}
			// F3: the STDOUT_KEYWORD_TABLE phrases are authored with "Claude Code"
			// as a placeholder; name the adapter that actually produced this line
			// (already filtered above to SessionAdapterId == AdapterId), so a
			// Codex/local-model auth error no longer says "Claude Code". Same
			// message for every chat in this batch → build it once.
			FString ClassifiedMsg(Match->Message);
			ClassifiedMsg.ReplaceInline(TEXT("Claude Code"),
				*AdapterDisplayLabel(AdapterId), ESearchCase::CaseSensitive);
			for (const FString& ChatId : ProcessingChats)
			{
				FChatSession* Session = ChatSessions.Find(ChatId);
				if (!Session) continue;
				// F4: once THIS chat's turn has produced genuine agent activity
				// (bHasStreamed), a bare-text stdout keyword match is no longer
				// authoritative enough to tear down the live prompt — codex-acp can
				// print a recovered "unauthorized" mid-stream while the turn completes
				// fine. Tearing down here zeroes PromptRpcId, so the real session/prompt
				// result can't match -> no 'done' -> a false 90s stall. Log and skip; the
				// RESULT / process-exit / watchdogs still backstop a genuinely-dead
				// stream. Suppress ALL table codes once streamed (the defect is generic,
				// not auth-specific); the PRE-streaming classification is preserved for
				// real session-start auth/trust/MCP failures.
				if (Session->bHasStreamed)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: suppressed mid-stream stdout classification for streaming chat %s on adapter %s (code=%s) — keeping the live turn's RPC state"),
						*ChatId, *AdapterId, Match->Code);
					continue;
				}
				// codex-review-fix: clear stuck RPC state before failing.
				// FailChatWithAdapterError only flips bProcessing → false; if
				// SessionRpcId/PromptRpcId/timestamps stay populated, the
				// timeout ticker can later fire a SECOND classified error for
				// the same chat, and DoCreateSession's re-entry guard will
				// silently no-op a retry.
				ClearChatRpcState(*Session);
				Session->PendingMessage.Empty();
				Session->PendingRetryImageBlocks.Empty();
				FailChatWithAdapterError(ChatId, AdapterId,
					Match->Stage, Match->Code, ClassifiedMsg);
			}
		}
		return;
	}

	// Check if frame carries an id (response, error, or agent-originated request)
	TSharedPtr<FJsonValue> IdVal = Acp::ExtractRpcId(Json);
	if (IdVal.IsValid())
	{
		const bool bIsResponse = Json->HasField(TEXT("result")) || Json->HasField(TEXT("error"));
		if (bIsResponse)
		{
			// Responses match against bridge-minted numeric ids in RpcToChatId.
			// If the peer ever sends a non-numeric response id we cannot match
			// it anyway; log and drop rather than silently coercing to 0.
			if (IdVal->Type != EJson::Number)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro: dropping ACP response with non-numeric id (type=%d) — bridge only mints numeric ids"),
					(int32)IdVal->Type);
				return;
			}
			int32 Id = (int32)IdVal->AsNumber();
			if (Json->HasField(TEXT("result")))
				HandleRpcResponse(Id, Json->GetObjectField(TEXT("result")), nullptr);
			else
				HandleRpcResponse(Id, nullptr, Json->GetObjectField(TEXT("error")));
			return;
		}

		// Agent-originated request (id + method). The id may be Number | String
		// | Null per JSON-RPC 2.0 — HandleMethod re-extracts via Acp::ExtractRpcId
		// to round-trip the original wire type on the response.
		if (Json->HasField(TEXT("method")))
		{
			FString Method = Json->GetStringField(TEXT("method"));
			HandleMethod(AdapterId, Method, Json);
			return;
		}
	}

	// Notification from agent (has "method" but no "id")
	if (Json->HasField(TEXT("method")))
	{
		FString Method = Json->GetStringField(TEXT("method"));
		HandleMethod(AdapterId, Method, Json);
	}
}

void UNwiroIKBridge::HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error)
{
	// Warmup responses are adapter-scoped, not chat-scoped — handle and
	// return early before the chat-id lookup below. We walk all adapters
	// rather than just localllm because the slot is per-adapter (cheap to
	// check) and this keeps the dispatch open if other adapters add
	// warmup support later.
	for (auto& Pair : AdapterProcesses)
	{
		if (Pair.Value.PendingWarmupRpcId == Id && Id != 0)
		{
			Pair.Value.PendingWarmupRpcId = 0;
			RpcToChatId.Remove(Id);
			FString PayloadJson;
			if (Error.IsValid())
			{
				// Synthesise a WarmupResult-shaped failure envelope from the
				// JSON-RPC error object so the JS settings store sees the
				// same shape regardless of whether the failure originated
				// in the shim's warmup logic or in the RPC transport itself.
				const FString ErrMsg = Error->HasField(TEXT("message"))
					? Error->GetStringField(TEXT("message")) : TEXT("Unknown error");
				PayloadJson = FString::Printf(
					TEXT("{\"status\":\"failed\",\"errorKind\":\"unknown\",\"elapsedMs\":0,\"message\":\"%s\"}"),
					*ErrMsg.Replace(TEXT("\""), TEXT("\\\"")));
			}
			else if (Result.IsValid())
			{
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
				FJsonSerializer::Serialize(Result.ToSharedRef(), W);
			}
			else
			{
				PayloadJson = TEXT("{\"status\":\"failed\",\"errorKind\":\"unknown\",\"elapsedMs\":0,\"message\":\"warmup response had neither result nor error\"}");
			}
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: warmup result rpc=%d payload=%s"), Id, *PayloadJson);
			// Capture the tool-call capability tier for the localllm
			// ToolSelector. The WarmupResult struct is serialised camelCase
			// (#[serde(rename_all="camelCase")]) so the field is `toolTier`;
			// the value is snake_case native/emulated/none. The raw payload log
			// above lets a single live warmup visually confirm the field name.
			// Only overwrite on a successful parse — a missing field (older
			// shim) leaves the tier empty, which ResolveToolBudget treats as
			// emulated (the safe under-send direction).
			if (Result.IsValid())
			{
				FString TierValue;
				if (Result->TryGetStringField(TEXT("toolTier"), TierValue))
				{
					LocalLlmToolTier = TierValue;
				}
			}
			// Empty ChatId — warmup is not associated with any specific chat.
			// JS chat-store dispatcher must handle the "warmup" case at the
			// top level and forward the payload to the settings store.
			EnqueueResponse(TEXT("warmup"), PayloadJson, TEXT(""));
			return;
		}
	}

	FString ChatId = FindChatIdByRpcId(Id);
	RpcToChatId.Remove(Id);

	// Official Codex prompt barrier. Handle it before the generic error path:
	// mode and model are configured in sequence and the retained prompt is
	// destroyed on every non-exact outcome.
	FChatSession* ModelConfigSession = ChatId.IsEmpty() ? nullptr : ChatSessions.Find(ChatId);
	if (ModelConfigSession && ModelConfigSession->ModelConfigRpcId == Id)
	{
		const FString AdapterId = ModelConfigSession->AdapterId.IsEmpty() ? CurrentAdapter : ModelConfigSession->AdapterId;
		const FString RequestedConfigId = ModelConfigSession->PendingConfigId;
		const FString RequestedMode = ModelConfigSession->PendingMode;
		const FString RequestedModel = ModelConfigSession->PendingModel;
		const TSharedPtr<FJsonObject> PreparedPrompt = ModelConfigSession->PreparedPromptParams;
		const uint64 ExpectedGeneration = ModelConfigSession->ModelConfigProcessGeneration;
		const bool bModeStage = RequestedConfigId == TEXT("mode");
		const FString Stage = bModeStage ? TEXT("configuring_mode") : TEXT("configuring_model");

		if (Error.IsValid())
		{
			const FString ErrMsg = Error->HasField(TEXT("message"))
				? Error->GetStringField(TEXT("message")) : TEXT("Unknown error");
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: Codex rejected configuration adapter=%s chat=%s config=%s mode=%s model=%s error=%s"),
				*AdapterId, *ChatId, *RequestedConfigId, *RequestedMode, *RequestedModel, *ErrMsg);
			ClearCodexModelBarrier(*ModelConfigSession);
			FailChatWithAdapterError(ChatId, AdapterId, Stage,
				bModeStage ? TEXT("mode_config_rejected") : TEXT("model_config_rejected"),
				bModeStage
					? TEXT("Codex rejected the required safe mode, so the prompt was not sent.")
					: TEXT("Codex rejected the selected model, so the prompt was not sent."));
			return;
		}

		const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
		if (!AP || ExpectedGeneration == 0 || AP->ProcessGeneration != ExpectedGeneration
			|| ModelConfigSession->BoundProcessGeneration != ExpectedGeneration)
		{
			ClearCodexModelBarrier(*ModelConfigSession);
			FailChatWithAdapterError(ChatId, AdapterId, Stage,
				TEXT("process_generation_mismatch"),
				TEXT("Codex restarted before it confirmed the selected configuration, so the prompt was not sent."));
			return;
		}

		if (!PreparedPrompt.IsValid())
		{
			ClearCodexModelBarrier(*ModelConfigSession);
			FailChatWithAdapterError(ChatId, AdapterId, Stage,
				TEXT("prepared_prompt_missing"),
				TEXT("The held prompt was no longer available after configuration, so nothing was sent."));
			return;
		}

		if (bModeStage)
		{
			NwiroCodex::FSelectConfigCatalog ModeAck;
			if (!NwiroCodex::IsExactSelectConfigAck(Result, TEXT("mode"), RequestedMode, ModeAck))
			{
				ClearCodexModelBarrier(*ModelConfigSession);
				FailChatWithAdapterError(ChatId, AdapterId, Stage,
					TEXT("mode_ack_mismatch"),
					TEXT("Codex did not acknowledge the exact safe mode requested, so the prompt was not sent."));
				return;
			}

			ModelConfigSession->AvailableModeIds = ModeAck.Values;
			ModelConfigSession->ModelConfigRpcId = 0;
			ModelConfigSession->ModelConfigRpcStartedAt = 0.0;
			ModelConfigSession->PendingConfigId = TEXT("model");
			SetAdapterStage(AdapterId, ChatId, TEXT("configuring_model"));

			TSharedPtr<FJsonObject> ModelParams = MakeShareable(new FJsonObject);
			ModelParams->SetStringField(TEXT("sessionId"), ModelConfigSession->SessionId);
			ModelParams->SetStringField(TEXT("configId"), TEXT("model"));
			ModelParams->SetStringField(TEXT("value"), RequestedModel);
			ModelConfigSession->ModelConfigRpcId = SendRpc(
				AdapterId, TEXT("session/set_config_option"), ModelParams);
			ModelConfigSession->ModelConfigRpcStartedAt = FPlatformTime::Seconds();
			RpcToChatId.Add(ModelConfigSession->ModelConfigRpcId, ChatId);
			return;
		}

		NwiroCodex::FModelCatalog AckCatalog;
		if (RequestedConfigId != TEXT("model")
			|| !NwiroCodex::IsExactModelAck(Result, RequestedModel, AckCatalog))
		{
			ClearCodexModelBarrier(*ModelConfigSession);
			FailChatWithAdapterError(ChatId, AdapterId, Stage,
				TEXT("model_ack_mismatch"),
				TEXT("Codex did not acknowledge the exact model requested, so the prompt was not sent."));
			return;
		}

		ModelConfigSession->AvailableModelIds = AckCatalog.ModelIds;
		ModelConfigSession->LastModel = RequestedModel;
		ClearCodexModelBarrier(*ModelConfigSession);
		DispatchPromptParams(ChatId, PreparedPrompt);
		return;
	}

	if (Error.IsValid())
	{
		FString ErrMsg = Error->HasField(TEXT("message")) ? Error->GetStringField(TEXT("message")) : TEXT("Unknown error");
		UE_LOG(LogTemp, Error, TEXT("Nwiro: ACP error (id=%d, chat=%s): %s"), Id, *ChatId, *ErrMsg);

		// Only push error to UI for critical RPCs (session/new, prompt)
		// Non-critical RPCs (set_mode etc.) just log
		bool bIsCritical = false;
		FChatSession* Session = ChatId.IsEmpty() ? nullptr : ChatSessions.Find(ChatId);
		if (Session)
		{
			if (Session->SessionRpcId == Id || Session->PromptRpcId == Id)
				bIsCritical = true;
		}

		// adapter-reliability-w6: session/load (resume) failure. Handled before
		// the generic critical-error path: a failed resume must NOT surface as a
		// terminal error/done pair — it silently degrades to a fresh session/new.
		// CLEAN path per A0 wire-test: read the numeric JSON-RPC `code` (no
		// message-substring matching). claude-agent-acp returns -32002
		// (resource not found) for an unknown/expired sessionId.
		if (Session && Session->SessionLoadRpcId == Id)
		{
			double CodeNum = 0.0;
			Error->TryGetNumberField(TEXT("code"), CodeNum);
			const int32 Code = (int32)CodeNum;

			// Clear replay tagging + the in-flight load markers so the watchdog
			// can't double-fire and the fresh create's guards aren't blocked.
			Session->bInReplay = false;
			Session->SessionLoadRpcId = 0;
			Session->SessionLoadStartedAt = 0.0;
			Session->bResumeFailed = true;

			FString FailReason = TEXT("load_failed");
			if (Code == -32002)
			{
				// Resource not found — the prior session id is gone/expired.
				FailReason = TEXT("resource_not_found");
			}
			// Draft/unverified arms — these JSON-RPC-standard codes are NOT yet
			// observed from claude-agent-acp's session/load, so they are left
			// commented until a live wire-test confirms the exact shape. Each
			// would map to its own reason but the fallback (fresh session/new)
			// is identical, so the default path below already covers them.
			// else if (Code == -32000) { FailReason = TEXT("server_error"); }
			// else if (Code == -32602) { FailReason = TEXT("invalid_params"); }
			// else if (Code == -32603) { FailReason = TEXT("internal_error"); }

			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: session/load failed (id=%d, chat=%s, code=%d, reason=%s): %s — falling back to session/new"),
				Id, *ChatId, Code, *FailReason, *ErrMsg);

			TSharedPtr<FJsonObject> ResumeFailedPayload = MakeShareable(new FJsonObject);
			ResumeFailedPayload->SetStringField(TEXT("reason"), FailReason);
			EnqueueResponse(TEXT("session_resume_failed"), JsonToString(ResumeFailedPayload), ChatId);

			DoCreateSession(ChatId);
			return;
		}

		// unknown-session-recovery: the shim answers session/prompt with
		// -32000 "ACP framing error: unknown session: <id>" when its side of
		// the session died (internal restart, evicted state) while we kept the
		// id. Without recovery the id is never cleared, so SendMessage's
		// session-exists fast path resends it forever — every subsequent send
		// in the chat fails identically. Newer shims also tag the error with
		// data.reason == "unknown_session"; match that first and fall back to
		// the message substring so shipped v0.4.0 shims (message-only) are
		// covered too.
		bool bUnknownSession = false;
		if (Session && Session->PromptRpcId == Id)
		{
			TSharedPtr<FJsonObject> ErrorData = GetOptionalObjectField(Error, TEXT("data"));
			FString Reason;
			if (ErrorData.IsValid() && ErrorData->TryGetStringField(TEXT("reason"), Reason))
			{
				bUnknownSession = (Reason == TEXT("unknown_session"));
			}
			if (!bUnknownSession)
			{
				bUnknownSession = ErrMsg.Contains(TEXT("ACP framing error: unknown session:"));
			}
		}
		if (bUnknownSession)
		{
			const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
			const FString StaleSessionId = Session->SessionId;

			// Clear the in-flight prompt markers first: the first-token ticker
			// must not fire mid-recovery, and DoCreateSession's re-entry guard
			// needs SessionRpcId == 0.
			ClearChatRpcState(*Session);

			// Same pattern as the first_token_timeout path: blacklist + unmap
			// the stale id so HandleSessionUpdate's bProcessing-fallback can't
			// latch late chunks from it onto the recreated session.
			if (!StaleSessionId.IsEmpty())
			{
				// Recovery abandons the stale session, so its cards are settled with it.
				++Session->TurnGeneration;
				RetireAcpPermissionState(FString(), StaleSessionId);
				if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
				{
					AP->RejectedAcpSessionIds.Add(StaleSessionId);
				}
				SessionToChatId.Remove(StaleSessionId);
				Session->SessionId.Empty();
			}

			// Loop guard: ONE transparent recreate+resend per user send (the
			// count resets in SendMessage). A second unknown-session for the
			// same send means the recreate path itself is broken — surface the
			// normal terminal error instead of looping.
			if (Session->UnknownSessionRetryCount >= 1 || Session->LastSentPrompt.IsEmpty())
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro IK: unknown-session recovery exhausted (adapter=%s, chat=%s, retries=%d) — failing turn"),
					*AdapterId, *ChatId, Session->UnknownSessionRetryCount);
				if (Session->UnknownSessionRetryCount == 0)
				{
					// Retry impossible (no retained prompt) and no notice has
					// been shown this turn — still tell the user the session
					// is gone before the error bubble. On the retries>=1 path
					// the first recovery attempt already showed it.
					EnqueueResponse(TEXT("system"),
						TEXT("Session was reset — earlier conversation context may be lost."),
						ChatId);
				}
				EnqueueResponse(TEXT("error"), ErrMsg, ChatId);
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
				return;
			}

			// P2-C fail-closed: the failed send carried image blocks but the
			// retained copies are gone (an adapter-exit sweep cleared them) —
			// a text-only resend would be a silent reduced request. Surface
			// the terminal error instead; the user re-sends the attachment.
			if (Session->bLastSentHadImages && Session->LastSentImageBlocks.Num() == 0)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro IK: unknown-session recovery aborted (adapter=%s, chat=%s) — retained image payload lost; failing turn instead of resending a reduced request"),
					*AdapterId, *ChatId);
				EnqueueResponse(TEXT("system"),
					TEXT("Session was reset — the attached image(s) could not be preserved. Please re-send the message with its attachment(s)."),
					ChatId);
				EnqueueResponse(TEXT("error"), ErrMsg, ChatId);
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
				return;
			}

			Session->UnknownSessionRetryCount++;

			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: unknown session (adapter=%s, chat=%s, staleId=%s) — recreating session and retrying prompt"),
				*AdapterId, *ChatId, *StaleSessionId);

			// Force the model re-push on the recreated session — DoSendPrompt
			// skips session/set_config_option when ModelToPush == LastModel,
			// but the fresh session has never seen the config. For localllm a
			// missing model push would land the session at tool-tier None
			// (M-1 describer trap), so this is load-bearing, not cosmetic.
			Session->LastModel.Empty();

			// Visible notice. "system" is in chat.ts's TRANSIENT set, so it
			// bumps — not trips — the JS stall watchdog. No error/done pair
			// here: the retry replaces the terminal path, and bProcessing
			// stays true so SendMessage's guard blocks a double-send
			// mid-recovery.
			EnqueueResponse(TEXT("system"),
				TEXT("Session was reset — earlier conversation context may be lost."),
				ChatId);

			// Queue the failed prompt and mint a fresh session. The session/new
			// success handler resends PendingMessage via DoSendPrompt —
			// transport-level, so the user message is not duplicated in the
			// app store.
			Session->PendingMessage = Session->LastSentPrompt;
			// Image-aware retry: stage the exact wire blocks the failed send
			// shipped so the resend is the COMPLETE, byte-identical request —
			// not a silent text-only reduction. DoSendPrompt consumes this
			// per-chat list in preference to the bridge-global PendingImages,
			// so interleaved attaches from other chats can't leak in.
			Session->PendingRetryImageBlocks = Session->LastSentImageBlocks;
			DoCreateSession(ChatId);
			return;
		}

		if (bIsCritical)
		{
			EnqueueResponse(TEXT("error"), ErrMsg, ChatId);
			if (Session && Session->PromptRpcId == Id)
			{
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
			// Clear the in-flight marker so DoCreateSession's re-entry guard
			// allows a retry. Without this, a single failed session/new
			// (network blip, codex-acp crash, bad params) would leave
			// SessionRpcId non-zero forever and the chat could never recover.
			if (Session && Session->SessionRpcId == Id)
			{
				Session->SessionRpcId = 0;
				// unknown-session-recovery: a failed session/new is terminal
				// for the turn — release bProcessing (set by SendMessage or
				// the recovery path above) and emit done, otherwise the error
				// bubble alone leaves bProcessing latched true and SendMessage's
				// guard blocks every future send in this chat. Drop the queued
				// message too so a later initialize fan-out can't replay a
				// prompt the user already watched fail.
				if (Session->bProcessing)
				{
					Session->PendingMessage.Empty();
					Session->PendingRetryImageBlocks.Empty();
					Session->bProcessing = false;
					EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro: Non-critical ACP error ignored (id=%d): %s"), Id, *ErrMsg);
		}
		return;
	}

	// Initialize response — check all adapter processes
	bool isInitResponse = false;
	FString InitializedAdapter;
	for (auto& APair : AdapterProcesses)
	{
		if (APair.Value.InitRpcId == Id)
		{
			isInitResponse = true;
			InitializedAdapter = APair.Key;
			break;
		}
	}
	if (isInitResponse)
	{
		// adapter-reliability: parse per-adapter capabilities from the
		// initialize result. Shape: { protocolVersion: number,
		// agentCapabilities: { loadSession: bool, ... },
		// agentInfo: { name, version } }. Parse defensively — missing
		// fields fall back to the FAdapterProcess defaults.
		if (FAdapterProcess* AP = AdapterProcesses.Find(InitializedAdapter))
		{
			double ProtocolVersion = 0.0;
			if (Result.IsValid() && Result->TryGetNumberField(TEXT("protocolVersion"), ProtocolVersion))
				AP->NegotiatedProtocolVersion = ProtocolVersion;

			TSharedPtr<FJsonObject> AgentCaps = GetOptionalObjectField(Result, TEXT("agentCapabilities"));
			if (AgentCaps.IsValid())
				AgentCaps->TryGetBoolField(TEXT("loadSession"), AP->bSupportsLoadSession);
				// Phase-1: whether this adapter's current model accepts image input.
				{
					TSharedPtr<FJsonObject> PromptCaps = GetOptionalObjectField(AgentCaps, TEXT("promptCapabilities"));
					if (PromptCaps.IsValid())
						PromptCaps->TryGetBoolField(TEXT("image"), AP->bSupportsImageInput);
				}

			TSharedPtr<FJsonObject> AgentInfo = GetOptionalObjectField(Result, TEXT("agentInfo"));
			AP->AgentName = GetOptionalStringField(AgentInfo, TEXT("name"));
			AP->AgentVersion = GetOptionalStringField(AgentInfo, TEXT("version"));

			if (NwiroCodex::UsesOfficialRuntime(InitializedAdapter)
				&& !NwiroCodex::IsOfficialIdentity(
					AP->AgentName,
					AP->AgentVersion,
					AP->ExpectedPackageVersion,
					AP->NegotiatedProtocolVersion))
			{
				UE_LOG(LogTemp, Error,
					TEXT("Nwiro IK: rejected Codex ACP identity name=%s liveVersion=%s expectedVersion=%s protocolVersion=%.17g"),
					*AP->AgentName, *AP->AgentVersion, *AP->ExpectedPackageVersion,
					AP->NegotiatedProtocolVersion);
				AP->bInitialized = false;
				AP->InitRpcStartedAt = 0.0;
				for (auto& SessionPair : ChatSessions)
				{
					if (SessionPair.Value.AdapterId != InitializedAdapter || !SessionPair.Value.bProcessing) continue;
					SessionPair.Value.PendingMessage.Empty();
					ClearChatRpcState(SessionPair.Value);
					FailChatWithAdapterError(SessionPair.Key, InitializedAdapter, TEXT("initializing_acp"),
						TEXT("runtime_identity_mismatch"),
						TEXT("The discovered Codex runtime is not the supported official ACP package."));
				}
				StopCodexProcessTreeAndWait(*AP, TEXT("runtime_identity_mismatch"));
				return;
			}

			AP->bInitialized = true;
			// On Windows, disarm only after the official live identity contract
			// passes. Legacy Zed is selected by the non-Windows platform route.
			AP->InitRpcStartedAt = 0.0;

			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: ACP adapter initialized (adapter=%s, rpc=%d, protocolVersion=%.17g, loadSession=%s, name=%s, version=%s)"),
				*InitializedAdapter, Id, AP->NegotiatedProtocolVersion,
				AP->bSupportsLoadSession ? TEXT("true") : TEXT("false"),
				*AP->AgentName, *AP->AgentVersion);
		}

		// adapter-reliability: surface the negotiated capabilities to the
		// frontend now that they're known, before any sessions are created.
		EnqueueAdapterCapabilities(InitializedAdapter);

		// Create sessions for any chats that were waiting (or just opened).
		// adapter-reliability-w2: respect bRetriedSessionWithoutMcp — after a
		// session_new_timeout we restart the adapter; once initialize comes
		// back here, the auto-fire below would otherwise re-attach MCP and
		// undo the isolation retry. Pass !flag so the retry actually exercises
		// the MCP-less path that the timeout handler wanted to test.
		for (auto& Pair : ChatSessions)
		{
			if (Pair.Value.AdapterId == InitializedAdapter && Pair.Value.SessionId.IsEmpty())
				TryCreateOrLoadSession(Pair.Key);
		}
		return;
	}

	// Session/new response — find which chat this was for
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.SessionRpcId == Id)
		{
			// adapter-reliability-w0: schema-validate before storing. Older
			// versions of claude-agent-acp return `sessionId` as a string at
			// the top level; a future schema change (rename, nest under
			// `session.id`, drop entirely) would have left us with an empty
			// string and a downstream silent abort in DoSendPrompt. Now we
			// fail loudly with `protocol_mismatch` and the raw response in
			// the log so the version drift is visible in support tickets.
			const TSharedPtr<FJsonValue>* SessionIdField = Result.IsValid()
				? Result->Values.Find(TEXT("sessionId")) : nullptr;
			const bool bHasValidSessionId =
				SessionIdField != nullptr && SessionIdField->IsValid()
				&& (*SessionIdField)->Type == EJson::String
				&& !(*SessionIdField)->AsString().IsEmpty();
			const FString FailedAdapterId = Pair.Value.AdapterId.IsEmpty() ? CurrentAdapter : Pair.Value.AdapterId;
			if (!bHasValidSessionId)
			{
				const FString RawJson = Result.IsValid() ? JsonToString(Result) : TEXT("<no result>");
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro IK: session/new returned no valid sessionId, raw=%s"),
					*RawJson);
				Pair.Value.SessionRpcId = 0;
				FailChatWithAdapterError(Pair.Key, FailedAdapterId, TEXT("creating_session"),
					TEXT("protocol_mismatch"),
					FailedAdapterId == TEXT("codex")
						? TEXT("Codex returned an invalid session response, so the prompt was not sent.")
						: TEXT("Adapter session/new response is missing a valid sessionId — likely a protocol version mismatch. Update the plugin or the adapter binary."));
				return;
			}
			const FString NewSessionId = (*SessionIdField)->AsString();
			if (FailedAdapterId == TEXT("codex"))
			{
				const FAdapterProcess* CodexProcess = AdapterProcesses.Find(FailedAdapterId);
				NwiroCodex::FModelCatalog Catalog;
				NwiroCodex::FSelectConfigCatalog ModeCatalog;
				const uint64 ProcessGeneration =
					CodexProcess ? CodexProcess->ProcessGeneration : 0;
				const bool bUsesOfficialCodexRuntime =
					NwiroCodex::UsesOfficialRuntime(FailedAdapterId);
				const bool bHasRequiredContract = bUsesOfficialCodexRuntime
					? NwiroCodex::TryParseRequiredSessionContract(
						Result, ProcessGeneration, Catalog, ModeCatalog)
					: NwiroCodex::TryParseRequiredModelContract(
						Result, ProcessGeneration, Catalog);
				if (!bHasRequiredContract)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: Codex session contract missing required config selectors (runtime=%s models=%d modes=%d)"),
						bUsesOfficialCodexRuntime ? TEXT("official") : TEXT("legacy"),
						Catalog.ModelIds.Num(), ModeCatalog.Values.Num());
					Pair.Value.PendingMessage.Empty();
					ClearChatRpcState(Pair.Value);
					FailChatWithAdapterError(Pair.Key, FailedAdapterId, TEXT("creating_session"),
						TEXT("protocol_mismatch"),
						bUsesOfficialCodexRuntime
							? TEXT("Codex did not provide the required model catalog and safe agent mode, so the prompt was not sent.")
							: TEXT("Codex did not provide the required model catalog, so the prompt was not sent."));
					return;
				}
				Pair.Value.AvailableModelIds = Catalog.ModelIds;
				Pair.Value.AvailableModeIds =
					bUsesOfficialCodexRuntime ? ModeCatalog.Values : TSet<FString>();
				Pair.Value.BoundProcessGeneration = CodexProcess->ProcessGeneration;
			}
			Pair.Value.SessionId = NewSessionId;
			// The successful response has been consumed and unmapped above. Clear
			// the per-chat guard too so a later process exit/restart cannot leave
			// this chat permanently stuck behind a stale session/new RPC id.
			Pair.Value.SessionRpcId = 0;
			// adapter-reliability-w6: snapshot the adapter version this session
			// was created against (for JS-side version-skew gating per D8) and
			// tell the frontend a fresh session now exists so it can persist the
			// id for a future resume.
			if (const FAdapterProcess* CreatedAP = AdapterProcesses.Find(FailedAdapterId))
				Pair.Value.AdapterVersionAtCreate = CreatedAP->AgentVersion;
			{
				TSharedPtr<FJsonObject> CreatedPayload = MakeShareable(new FJsonObject);
				CreatedPayload->SetStringField(TEXT("sessionId"), NewSessionId);
				CreatedPayload->SetStringField(TEXT("adapter"), FailedAdapterId);
				EnqueueResponse(TEXT("session_created"), JsonToString(CreatedPayload), Pair.Key);
			}
			// adapter-reliability-w1: disarm session_new_timeout.
			Pair.Value.SessionRpcStartedAt = 0.0;
			SessionToChatId.Add(NewSessionId, Pair.Key);
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: ACP session created for chat %s (sessionId=%s)"),
				*Pair.Key, *NewSessionId);
			const FString AdapterId = FailedAdapterId;

			// adapter-reliability-w2 §6: if this session was created via the
			// MCP-isolation retry, the original attempt-with-MCP must have been
			// the blocker. Tell the user that succinctly so they can act on it
			// (restart MCP, check port, etc.) rather than chalking it up to a
			// transient hiccup. Reset the flag so subsequent prompts in this
			// chat go back to the default MCP-attached path.
			if (Pair.Value.bRetriedSessionWithoutMcp)
			{
				if (NwiroCodex::UsesOfficialRuntime(AdapterId))
				{
					UE_LOG(LogTemp, Log,
						TEXT("Nwiro IK: Codex session recovered after integration restart chat=%s"),
						*Pair.Key);
				}
				else
				{
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: session created only after MCP isolation retry — Nwiro MCP likely unhealthy adapter=%s chat=%s"),
						*AdapterId, *Pair.Key);
					EnqueueResponse(TEXT("system"),
						TEXT("Adapter session created only after disabling MCP attachment. The Nwiro MCP connection may be blocked or unhealthy — try Restart MCP from Settings."),
						Pair.Key);
				}
				Pair.Value.bRetriedSessionWithoutMcp = false;
			}

			// Claude rejects session/set_mode. Windows Codex confirms its safe
			// mode through the exact config-option ACK barrier; legacy Zed keeps
			// the original auto-mode request.
			if (AdapterId != TEXT("claude")
				&& !NwiroCodex::UsesOfficialRuntime(AdapterId))
			{
				TSharedPtr<FJsonObject> ModeParams = MakeShareable(new FJsonObject);
				ModeParams->SetStringField(TEXT("sessionId"), NewSessionId);
				ModeParams->SetStringField(TEXT("modeId"), TEXT("auto"));
				SendRpc(AdapterId, TEXT("session/set_mode"), ModeParams);
			}


			if (!Pair.Value.PendingMessage.IsEmpty())
			{
				FString Msg = Pair.Value.PendingMessage;
				Pair.Value.PendingMessage.Empty();
				DoSendPrompt(Pair.Key, Msg);
			}
			return;
		}
	}

	// adapter-reliability-w6: session/load (resume) response — find which chat
	// this was for. Unlike session/new, the result carries no sessionId (the
	// id is the one we sent in DoLoadSession), so the established session id is
	// PriorSessionId. On success the chat is live again: disarm replay, mark
	// the session established, and dispatch any queued prompt.
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.SessionLoadRpcId == Id)
		{
			const FString ResumedSessionId = Pair.Value.PriorSessionId;
			const FString AdapterId = Pair.Value.AdapterId.IsEmpty() ? CurrentAdapter : Pair.Value.AdapterId;
			if (AdapterId == TEXT("codex"))
			{
				const FAdapterProcess* CodexProcess = AdapterProcesses.Find(AdapterId);
				NwiroCodex::FModelCatalog Catalog;
				NwiroCodex::FSelectConfigCatalog ModeCatalog;
				const uint64 ProcessGeneration =
					CodexProcess ? CodexProcess->ProcessGeneration : 0;
				const bool bUsesOfficialCodexRuntime =
					NwiroCodex::UsesOfficialRuntime(AdapterId);
				const bool bHasRequiredContract = bUsesOfficialCodexRuntime
					? NwiroCodex::TryParseRequiredSessionContract(
						Result, ProcessGeneration, Catalog, ModeCatalog)
					: NwiroCodex::TryParseRequiredModelContract(
						Result, ProcessGeneration, Catalog);
				if (!bHasRequiredContract)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: resumed Codex session contract missing required config selectors (runtime=%s models=%d modes=%d)"),
						bUsesOfficialCodexRuntime ? TEXT("official") : TEXT("legacy"),
						Catalog.ModelIds.Num(), ModeCatalog.Values.Num());
					if (!ResumedSessionId.IsEmpty())
						SessionToChatId.Remove(ResumedSessionId);
					Pair.Value.SessionId.Empty();
					Pair.Value.SessionLoadRpcId = 0;
					Pair.Value.SessionLoadStartedAt = 0.0;
					Pair.Value.bInReplay = false;
					Pair.Value.bResumeAttempted = true;
					Pair.Value.bResumeFailed = true;
					Pair.Value.AvailableModelIds.Empty();
					Pair.Value.AvailableModeIds.Empty();
					Pair.Value.BoundProcessGeneration = 0;
					Pair.Value.PendingMessage.Empty();
					FailChatWithAdapterError(Pair.Key, AdapterId, TEXT("creating_session"),
						TEXT("protocol_mismatch"),
						bUsesOfficialCodexRuntime
							? TEXT("Codex did not provide the required model catalog and safe agent mode, so the prompt was not sent.")
							: TEXT("Codex did not provide the required model catalog, so the prompt was not sent."));
					return;
				}
				Pair.Value.AvailableModelIds = Catalog.ModelIds;
				Pair.Value.AvailableModeIds =
					bUsesOfficialCodexRuntime ? ModeCatalog.Values : TSet<FString>();
				Pair.Value.BoundProcessGeneration = CodexProcess->ProcessGeneration;
			}
			// The resumed id is now the live session id (DoLoadSession only
			// recorded it as PriorSessionId + mapped SessionToChatId).
			Pair.Value.SessionId = ResumedSessionId;
			// Disarm the resume watchdog + replay tagging; latch the attempt.
			Pair.Value.SessionLoadRpcId = 0;
			Pair.Value.SessionLoadStartedAt = 0.0;
			Pair.Value.bInReplay = false;
			Pair.Value.bResumeAttempted = true;
			// DoLoadSession already added PriorSessionId → chat, but guard
			// against an empty id (the cap-gate would have skipped, but be safe).
			if (!ResumedSessionId.IsEmpty())
				SessionToChatId.Add(ResumedSessionId, Pair.Key);
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: ACP session resumed for chat %s (sessionId=%s)"),
				*Pair.Key, *ResumedSessionId);

			TSharedPtr<FJsonObject> ResumedPayload = MakeShareable(new FJsonObject);
			ResumedPayload->SetStringField(TEXT("sessionId"), ResumedSessionId);
			ResumedPayload->SetStringField(TEXT("adapter"), AdapterId);
			EnqueueResponse(TEXT("session_resumed"), JsonToString(ResumedPayload), Pair.Key);

			if (!Pair.Value.PendingMessage.IsEmpty())
			{
				FString Msg = Pair.Value.PendingMessage;
				Pair.Value.PendingMessage.Empty();
				DoSendPrompt(Pair.Key, Msg);
			}
			return;
		}
	}

	// Prompt response (done) — find which chat
	for (auto& Pair : ChatSessions)
	{
		if (Pair.Value.PromptRpcId == Id)
		{
			UE_LOG(LogTemp, Verbose, TEXT("Nwiro IK: ACP prompt completed for chat %s (rpc=%d)"), *Pair.Key, Id);
			Pair.Value.bProcessing = false;
			// adapter-reliability-w5: log bytes-before-response for the one-shot
			// (non-streaming) success path. Guarded by `> 0.0` so the streaming
			// path doesn't double-log — when HandleSessionUpdate already cleared
			// this on first chunk, the guard skips us. Verbose level matches the
			// streaming-path log for grep consistency.
			if (Pair.Value.PromptRpcStartedAt > 0.0)
			{
				const FAdapterProcess* APLog = AdapterProcesses.Find(Pair.Value.AdapterId);
				const int64 BytesBeforeFirstChunk = APLog
					? (APLog->BytesFromStdout - Pair.Value.BytesAtPromptSendStart)
					: -1;
				UE_LOG(LogTemp, Verbose,
					TEXT("Nwiro IK: first chunk received (one-shot) adapter=%s chat=%s elapsedMs=%d bytesBeforeFirstChunk=%lld"),
					*Pair.Value.AdapterId, *Pair.Key,
					(int32)((FPlatformTime::Seconds() - Pair.Value.PromptRpcStartedAt) * 1000.0),
					BytesBeforeFirstChunk);
			}
			// adapter-reliability-w1: disarm first_token_timeout. Belt-and-
			// braces — HandleSessionUpdate already cleared this on first
			// stream chunk, but adapters that reply in one shot (no streaming)
			// jump straight here, so clear again for that case.
			Pair.Value.PromptRpcStartedAt = 0.0;
			EnqueueResponse(TEXT("done"), TEXT(""), Pair.Key);
			return;
		}
	}
}

void UNwiroIKBridge::HandleMethod(const FString& AdapterId, const FString& Method, const TSharedPtr<FJsonObject>& FullMsg)
{
	TSharedPtr<FJsonObject> Params = FullMsg->HasField(TEXT("params"))
		? FullMsg->GetObjectField(TEXT("params")) : nullptr;
	// Union-aware id read (codex-acp-string-id fix). Replaces the prior
	// `(int32)GetNumberField("id")` which logged a `String used as a Number`
	// LogJson error and produced 0 whenever codex emitted a string id.
	//
	// JSON-RPC 2.0: `"id": null` is a request that REQUIRES a response with id
	// echoed as null — distinct from a missing `id` field (notification, no
	// response). HasId reflects field-present, not field-non-null; the new
	// SendRpcResult overload round-trips the null wire value verbatim.
	Acp::FRequestId ReqId(Acp::ExtractRpcId(FullMsg));
	const bool HasId = ReqId.IsValid();

	// session/update — streaming content (includes sessionId for routing)
	if (Method == TEXT("session/update") && Params.IsValid())
	{
		FString AcpSessionId = Params->HasField(TEXT("sessionId"))
			? Params->GetStringField(TEXT("sessionId")) : TEXT("");
		TSharedPtr<FJsonObject> Update = Params->HasField(TEXT("update"))
			? Params->GetObjectField(TEXT("update")) : nullptr;
		if (Update.IsValid()) HandleSessionUpdate(AdapterId, AcpSessionId, Update);
		return;
	}

	// session/request_permission — permission popup
	//
	// ReqId.IsValid() required: JSON-RPC without an `id` is a NOTIFICATION, which by
	// definition cannot be responded to. Accepting one would mint permission state (and
	// possibly a card) for a request that can never be answered or settled on the wire.
	if (Method == TEXT("session/request_permission") && Params.IsValid() && ReqId.IsValid())
	{
		// Extract options from ACP
		TArray<TSharedPtr<FJsonValue>> OptArr;
		if (Params->HasField(TEXT("options")))
			OptArr = Params->GetArrayField(TEXT("options"));

		// ── Mode-based auto-response ──

		// Helper: find first option matching a kind
		auto FindOption = [&](const FString& KindMatch) -> FString {
			for (const auto& OptVal : OptArr) {
				TSharedPtr<FJsonObject> Opt = OptVal->AsObject();
				if (!Opt.IsValid()) continue;
				FString Kind = Opt->HasField(TEXT("kind")) ? Opt->GetStringField(TEXT("kind")) : TEXT("");
				if (Kind.Contains(KindMatch, ESearchCase::IgnoreCase)) return Opt->GetStringField(TEXT("optionId"));
			}
			return TEXT("");
		};

		// Bypass Mode → auto-approve all permission prompts
		// Shell safety outranks mode-level auto-approval.
		TSharedPtr<FJsonObject> PermissionToolCall = GetOptionalObjectField(Params, TEXT("toolCall"));
		FString PermissionToolCallId = GetOptionalStringField(PermissionToolCall, TEXT("toolCallId"));
		FString PermissionSessionId = GetOptionalStringField(Params, TEXT("sessionId"));
		FString PermissionToolCallKey = MakeToolCallKey(PermissionSessionId, PermissionToolCallId);

		// Resolve the owning chat for DISPLAY ONLY — which chat should show the card, the
		// system message and the "done" event. This chain intentionally falls back to
		// ToolUseToChatId and finally ActiveChatId so a card is never lost.
		//
		// It must NEVER inform the authorization decision below. Those fallbacks answer
		// "where would the user be looking", which is a UI question; treating a UI answer
		// as proof of provenance is how the original kind:"execute" bug happened. The
		// claim lookup keys off PermissionSessionId directly and fails closed without it.
		FString PermissionChatId = !PermissionSessionId.IsEmpty()
			? SessionToChatId.FindRef(PermissionSessionId) : TEXT("");
		if (PermissionChatId.IsEmpty() && !PermissionToolCallId.IsEmpty())
			PermissionChatId = ToolUseToChatId.FindRef(PermissionToolCallId);
		if (PermissionChatId.IsEmpty())
			PermissionChatId = ActiveChatId;

		// ── B1: distinguish an MCP TOOL approval from a real SHELL approval ──────────
		// codex-acp emits both with kind:"execute". They differ only outside the nested
		// toolCall, which this handler previously never looked at:
		//   shell approval : toolCall.rawInput = {command, cwd}   _meta.codex = {params}
		//   MCP  approval  : toolCall has NO rawInput/title       _meta.is_mcp_tool_approval
		// Reading only the nested object, every MCP approval looked like a command-less
		// shell request and was denied as "empty command" — ~157 approval-requiring
		// tools, every one of them.
		TSharedPtr<FJsonObject> PermissionMeta = GetOptionalObjectField(Params, TEXT("_meta"));
		// Boolean TRUE required, not mere presence: HasField would also accept
		// "is_mcp_tool_approval": false / null / "no", i.e. a payload asserting the
		// opposite of what we would read it as.
		bool bMcpApprovalFlag = false;
		const bool bMcpApprovalMarker =
			PermissionMeta.IsValid()
			&& PermissionMeta->TryGetBoolField(TEXT("is_mcp_tool_approval"), bMcpApprovalFlag)
			&& bMcpApprovalFlag;
		// The real-shell emitter's own signature. Asserting its ABSENCE is a second,
		// independent discriminator: if a future build ever set both, we fail closed.
		const bool bRealShellMarker =
			PermissionMeta.IsValid() && PermissionMeta->HasField(TEXT("codex"));

		// The marker alone means "an MCP approval" — NOT "one of ours". A third-party
		// MCP server carries the identical marker. Identity comes from the claim recorded
		// when the rich tool_call arrived.
		//
		// Both key parts are required. A valid 1.1.7 approval always carries the session
		// id it was minted under; if it is missing, or names a session we no longer hold
		// a claim for, we fail closed rather than widening the search. Anything we cannot
		// correlate stays denied exactly as today — for an uncorrelated envelope we know
		// neither the server nor the tool, so there is nothing to base an exemption on.
		FNwiroMcpApprovalInputs ApprovalInputs;
		ApprovalInputs.bMcpApprovalMarker    = bMcpApprovalMarker;
		ApprovalInputs.bRealShellMarker      = bRealShellMarker;
		ApprovalInputs.bOfficialCodexRuntime = NwiroCodex::UsesOfficialRuntime(AdapterId);
		ApprovalInputs.bSessionLive          = IsAcpSessionLive(PermissionSessionId);
		ApprovalInputs.RequestAdapterId      = AdapterId;

		FString CorrelatedTitle;
		FNwiroMcpToolClaim ClaimOrigin;
		if (!PermissionSessionId.IsEmpty() && !PermissionToolCallId.IsEmpty())
		{
			FNwiroMcpToolClaim Claim;
			// Consumed BEFORE it is judged, and regardless of how this attempt ends: a
			// malformed, spoofed or denied first attempt must not leave evidence that a
			// better-formed second attempt could reuse.
			if (NwiroMcpToolClaims.RemoveAndCopyValue(PermissionToolCallKey, Claim))
			{
				ClaimOrigin                            = Claim;
				CorrelatedTitle                        = Claim.Title;
				ApprovalInputs.bClaimFound             = true;
				ApprovalInputs.ClaimAdapterId          = Claim.AdapterId;
				ApprovalInputs.ClaimProcessGeneration  = Claim.ProcessGeneration;
			}
		}
		if (const FAdapterProcess* ApprovalProcess = AdapterProcesses.Find(AdapterId))
		{
			ApprovalInputs.CurrentProcessGeneration = ApprovalProcess->ProcessGeneration;
		}

		// Defence in depth: shell evidence on the envelope, or an id already poisoned by
		// a blocked tool_call, always outranks the exemption.
		//
		// BeyondKind, not IsShellToolCall: the correlated approval frame is bare apart
		// from kind:"execute", which real shell approvals carry too. Using the full
		// predicate here makes this term unconditionally true and the exemption below
		// dead code — the guard would read as present and behave as absent.
		ApprovalInputs.bEnvelopeShellEvidence = HasShellEvidenceBeyondKind(PermissionToolCall);
		const bool bPreviouslyBlocked =
			!PermissionToolCallKey.IsEmpty() && BlockedShellToolCallIds.Contains(PermissionToolCallKey);
		ApprovalInputs.bPreviouslyBlocked = bPreviouslyBlocked;

		const bool bExemptAsNwiroMcp = IsExemptNwiroMcpApproval(ApprovalInputs);

		// The correlated title is the ONLY place this tool's name exists at approval
		// time. Logging it makes the decision self-evidencing in a user's log: the
		// original report could say no more than "commands get blocked", because a denial
		// here had nothing but an opaque tool-call id to name.
		if (ApprovalInputs.bClaimFound)
		{
			UE_LOG(LogTemp, Log,
				TEXT("Nwiro IK: MCP approval '%s' matched a claim (exempt=%d, gen %llu vs %llu)"),
				CorrelatedTitle.IsEmpty() ? TEXT("(untitled)") : *CorrelatedTitle,
				bExemptAsNwiroMcp ? 1 : 0,
				ApprovalInputs.ClaimProcessGeneration, ApprovalInputs.CurrentProcessGeneration);
		}

		const bool bShellPermission =
			!bExemptAsNwiroMcp && (IsShellToolCall(PermissionToolCall) || bPreviouslyBlocked);

		// The turn this request BELONGS to.
		//
		// Preferred source is the consumed claim, which stamped chat/turn/mode when the
		// rich tool_call arrived. Reading the chat's *current* values instead labels a
		// delayed frame with whatever turn is running by the time it lands — turn A's
		// approval arriving after A was cancelled would be stamped turn B, and then pass
		// a liveness check that is internally consistent and simply wrong.
		//
		// Only when there is no claim (a non-MCP approval — Codex's own shell or edit
		// requests, which have no preceding nwiro frame) do we fall back to the chat's
		// current turn. Those arrive within their own turn, so that is their real origin.
		const FChatSession* OriginChat = ChatSessions.Find(PermissionChatId);
		const FString OriginChatId = ApprovalInputs.bClaimFound && !ClaimOrigin.ChatId.IsEmpty()
			? ClaimOrigin.ChatId : PermissionChatId;
		const uint64 OriginTurn = ApprovalInputs.bClaimFound && ClaimOrigin.TurnGeneration != 0
			? ClaimOrigin.TurnGeneration
			: (OriginChat ? OriginChat->TurnGeneration : 0);
		FString RequestMode = ApprovalInputs.bClaimFound && !ClaimOrigin.ModeSnapshot.IsEmpty()
			? ClaimOrigin.ModeSnapshot
			: (OriginChat ? OriginChat->ModeSnapshot : FString());
		// No snapshot means no turn we can attribute this request to: ask the user.
		if (RequestMode.IsEmpty()) RequestMode = TEXT("default");

		// Build the immutable origin record ONCE. Everything downstream — auto-allow,
		// auto-deny, shell rejection, the card, the eventual click — settles against this,
		// never against live bridge state.
		FAcpPermissionRequest PermissionRequest;
		PermissionRequest.AcpReqId          = ReqId;
		PermissionRequest.AdapterId         = AdapterId;
		PermissionRequest.ProcessGeneration = ApprovalInputs.CurrentProcessGeneration;
		PermissionRequest.AcpSessionId      = PermissionSessionId;
		PermissionRequest.ChatId            = OriginChatId;
		PermissionRequest.TurnGeneration    = OriginTurn;
		PermissionRequest.ModeSnapshot      = RequestMode;
		PermissionRequest.ToolCallId        = PermissionToolCallId;
		PermissionRequest.ToolTitle         = CorrelatedTitle.IsEmpty()
			? GetOptionalStringField(PermissionToolCall, TEXT("title")) : CorrelatedTitle;
		// A real deadline, so the ticker's expiry sweep is not a no-op. Without this every
		// request carried 0 ("never expires") and a card the user simply walked away from
		// pinned its turn open forever.
		PermissionRequest.ExpiresAtSeconds  = FPlatformTime::Seconds() + NwiroPermissionTtlSeconds();
		for (const TSharedPtr<FJsonValue>& OptVal : OptArr)
		{
			TSharedPtr<FJsonObject> OptObj = OptVal.IsValid() ? OptVal->AsObject() : nullptr;
			if (!OptObj.IsValid()) continue;
			FAcpPermissionOption Option;
			Option.OptionId = GetOptionalStringField(OptObj, TEXT("optionId"));
			Option.Name     = GetOptionalStringField(OptObj, TEXT("name"));
			Option.Kind     = GetOptionalStringField(OptObj, TEXT("kind"));
			if (!Option.OptionId.IsEmpty()) PermissionRequest.Options.Add(Option);
		}

		// One value naming what happens next, decided once. The branches below implement
		// it; none of them re-derives it.
		const ENwiroPermissionDisposition Disposition =
			DecidePermissionDisposition(bShellPermission, RequestMode);
		// Log, NOT Verbose. Every other outcome of this handler was silent at default
		// verbosity, so an auto-allowed approval left no trace — and a log grep for it
		// then "proved" the path was never exercised. One line per permission decision is
		// cheap, and it is the only way a field report or a live test can show what the
		// gate actually did.
		static const TCHAR* DispositionNames[] = { TEXT("shell_reject"), TEXT("auto_allow"), TEXT("auto_deny"), TEXT("show_card") };
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK PERMISSION: tool='%s' chat='%s' mode='%s' turn=%llu disposition=%s exempt=%d claim=%d marker=%d"),
			PermissionRequest.ToolTitle.IsEmpty() ? TEXT("(untitled)") : *PermissionRequest.ToolTitle,
			*PermissionChatId, *RequestMode, PermissionRequest.TurnGeneration,
			DispositionNames[static_cast<int32>(Disposition)],
			bExemptAsNwiroMcp ? 1 : 0, ApprovalInputs.bClaimFound ? 1 : 0, bMcpApprovalMarker ? 1 : 0);

		if (Disposition == ENwiroPermissionDisposition::ShellReject)
		{
			FString Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("reject"));
			if (Opt.IsEmpty()) Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("deny"));
			// No literal "deny" fallback: that id is only correct by coincidence of the
			// vocabularies we happen to have seen. An empty Opt means the agent offered
			// no reject option, and the coordinator sends nothing rather than guess.

			FString Title = GetOptionalStringField(PermissionToolCall, TEXT("title"));
			if (Title.IsEmpty()) Title = PermissionToolCallId.IsEmpty() ? TEXT("(unknown)") : PermissionToolCallId;

			// Phase 3b.3 — extract the command across adapter payload shapes (codex: rawInput.command;
			// also cmd / commandLine / args[] / argv[]), classify it, and TRACE. Fail-closed: an empty
			// extraction feeds Assess("") which the policy returns as Block — "no command found" is
			// NEVER treated as allow. The trace (paired with the P2 trace) reveals the execution model
			// and command identity for the Phase 3b.5 handoff.
			const FString ShellCmd = ExtractShellCommand(PermissionToolCall);
			const NwiroIKShellPolicy::FAssessment ShellAssess = NwiroIKShellPolicy::Assess(ShellCmd);
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK SHELLTRACE P1 request_permission enabled=%d extracted=%d risk=%s hash=%s reason='%s'"),
				!bBlockCommandExecution, ShellCmd.IsEmpty() ? 0 : 1, NwiroIKShellPolicy::RiskToString(ShellAssess.Risk),
				*NwiroIKShellPolicy::NormalizedHash(ShellCmd), *ShellAssess.Reason);
			// Raw command only at Verbose (off by default) — keeps a persistent secret-exposure
			// channel out of normal UE logs (council). The hash above correlates P1↔P2.
			UE_LOG(LogTemp, Verbose, TEXT("Nwiro IK SHELLTRACE P1 cmd='%s'"), *ShellCmd);

			// Council #1 safety: hard-reject Block-tier here even when shell is ENABLED
			// (defense-in-depth — if the agent executes in its own process after P1, P2
			// never sees it, so P1 must refuse Block-tier regardless of the user). The
			// non-Block "enabled" path (enhanced card + ApproveShellCommand handoff) is
			// Phase 3b, gated on the runtime trace — until then deny to stay safe.
			const TCHAR* Why = bBlockCommandExecution ? TEXT("shell disabled")
				: (ShellAssess.Risk == NwiroIKShellPolicy::ERisk::Block ? TEXT("blocked by shell policy") : TEXT("approval handoff not yet wired"));
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: BLOCKED shell permission '%s' (%s)"), *Title, Why);
			PermissionCoordinator.SettleImmediate(
				PermissionRequest, EAcpPermissionOutcome::ShellBlocked, EAcpPermissionReason::ShellPolicy, Opt);

			// PermissionChatId resolved above, before the shell decision.
			if (!PermissionChatId.IsEmpty())
			{
				if (FChatSession* PermissionSession = ChatSessions.Find(PermissionChatId); PermissionSession && PermissionSession->bProcessing)
				{
					if (!PermissionSessionId.IsEmpty())
					{
						TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
						CancelParams->SetStringField(TEXT("sessionId"), PermissionSessionId);
						SendRpcNotification(AdapterId, TEXT("session/cancel"), CancelParams);
					}
					PermissionSession->bProcessing = false;
					// This cancels the whole turn, so its SIBLING cards die with it — and
					// the generation has to move, or a card raised earlier in this turn
					// would still validate against it.
					++PermissionSession->TurnGeneration;
					PermissionCoordinator.RetireMatching(
						FString(), FString(), PermissionChatId, EAcpPermissionOutcome::Retired);
					EnqueueResponse(TEXT("done"), TEXT(""), PermissionChatId);
				}
			}
			return;
		}

		// bypassPermissions / acceptEdits → auto-approve.
		//
		// Routed through the coordinator rather than answered inline, because an
		// auto-APPROVE is a positive authorization and must therefore be revalidated
		// against the origin context. Answering inline is what let a delayed request whose
		// chat had already been closed still reach an allow — and it applied to every
		// approval kind, not just the MCP ones the B1 exemption covers.
		if (Disposition == ENwiroPermissionDisposition::AutoAllow)
		{
			FString Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("allow"));
			if (Opt.IsEmpty()) Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("approve"));
			// Deliberately NO "just take the first option" fallback. That guessed an id
			// from an unknown vocabulary and could pick a REJECT option — or, worse, an
			// allow one the caller never meant to grant. No mappable option means we ask.
			if (Opt.IsEmpty())
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Nwiro IK: '%s' offers no allow-kind option; showing a card instead of guessing"),
					*PermissionRequest.ToolTitle);
			}
			else
			{
				PermissionCoordinator.SettleImmediate(
					PermissionRequest, EAcpPermissionOutcome::Allowed,
					EAcpPermissionReason::ModeAutoApprove, Opt);
				return;
			}
		}

		// plan / dontAsk → auto-deny. Only plan explains itself; dontAsk is deliberately
		// silent, which is the whole point of that mode.
		if (Disposition == ENwiroPermissionDisposition::AutoDeny)
		{
			FString Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("reject"));
			if (Opt.IsEmpty()) Opt = FAcpPermissionCoordinator::FindOptionByKind(PermissionRequest.Options, TEXT("deny"));
			// No literal "deny" fallback: that id is only correct by coincidence of the
			// vocabularies we happen to have seen. An empty Opt means the agent offered
			// no reject option, and the coordinator sends nothing rather than guess.
			PermissionCoordinator.SettleImmediate(PermissionRequest, EAcpPermissionOutcome::Denied,
				EAcpPermissionReason::ModeAutoDeny, Opt);

			if (RequestMode == TEXT("plan") && !PermissionChatId.IsEmpty())
			{
				// The chat that OWNS this request, not whichever is in the foreground.
				EnqueueResponse(TEXT("system"),
					TEXT("Plan mode: tool execution blocked — disable Plan Mode to act on the project."),
					PermissionChatId);
			}
			return;
		}

		// Default → show permission card to user.
		// Mint a local int32 id for the React contract and remember the original
		// ACP id (which may be string/null on the wire) so we can echo it back
		// verbatim when the user clicks Allow/Deny. Negative range keeps ACP
		// ids permanently disjoint from MCP's >= 1000 range.
		// The coordinator mints the id, supersedes any older card for this chat (settling
		// it on the wire and in the UI, which the renderer used to do locally and answer
		// nobody), and owns the record until exactly one terminal outcome.
		const int32 LocalPermId = PermissionCoordinator.AddPending(PermissionRequest);

		TSharedPtr<FJsonObject> PermJson = MakeShareable(new FJsonObject);
		PermJson->SetNumberField(TEXT("id"), LocalPermId);
		// Capability flag, per request. The renderer waits for `permission_settled` ONLY
		// when it sees this; a plugin older than the event never sends it, so that build's
		// cards keep resolving locally instead of hanging disabled forever. The Nwiro Pro
		// path, which has no settled-event, is unaffected for the same reason.
		PermJson->SetBoolField(TEXT("backendSettles"), true);
		// Which tool this gate belongs to. Without it the renderer flipped whichever tool
		// happened to be running to 'pending' — wrong as soon as a turn has more than one
		// tool in flight.
		PermJson->SetStringField(TEXT("toolCallId"), PermissionToolCallId);

		if (Params->HasField(TEXT("toolCall")))
		{
			TSharedPtr<FJsonObject> ToolCall = Params->GetObjectField(TEXT("toolCall"));
			// The MCP approval envelope carries no title, so this used to render every
			// nwiro tool prompt as "Unknown" — the user is asked to approve something the
			// card cannot name. The correlated claim is the only source of that name.
			FString CardToolName = GetOptionalStringField(ToolCall, TEXT("title"));
			if (CardToolName.IsEmpty()) CardToolName = CorrelatedTitle;
			if (CardToolName.IsEmpty()) CardToolName = TEXT("Unknown");
			PermJson->SetStringField(TEXT("toolName"), CardToolName);
		}

		TArray<TSharedPtr<FJsonValue>> OutOpts;
		for (const auto& OptVal : OptArr)
		{
			TSharedPtr<FJsonObject> Opt = OptVal->AsObject();
			if (!Opt.IsValid()) continue;
			TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
			O->SetStringField(TEXT("optionId"), Opt->GetStringField(TEXT("optionId")));
			O->SetStringField(TEXT("name"), Opt->GetStringField(TEXT("name")));
			O->SetStringField(TEXT("kind"), Opt->HasField(TEXT("kind")) ? Opt->GetStringField(TEXT("kind")) : TEXT(""));
			OutOpts.Add(MakeShareable(new FJsonValueObject(O)));
		}
		PermJson->SetArrayField(TEXT("options"), OutOpts);

		// Explicit chat, not the two-argument overload: that one targets ActiveChatId, so
		// a background chat's approval card surfaced in whichever chat happened to be in
		// the foreground — asking the user to approve one chat's action from inside
		// another's transcript.
		EnqueueResponse(TEXT("permission_request"), JsonToString(PermJson), PermissionChatId);
		return;
	}

	// fs/read_text_file — agent wants to read a file. Read scope: the PROJECT
	// directory or the ENGINE install (read-only; agents legitimately read engine
	// headers/config — deliberately preserved). Everything else is rejected: this
	// handler previously read ANY absolute path on the machine (e.g. ~/.ssh keys).
	// No File Editor gate — ACP reads have never required the extension (explicit
	// product decision, 2026-07-09).
	//
	// Resolution is done INLINE (not via NwiroIKPathSandbox::TryResolveSandboxed)
	// on purpose: ACP agents send ABSOLUTE paths, and that helper unconditionally
	// FPaths::Combine's its input under ProjectDir — which is naive concatenation
	// (FString::PathAppend does no re-root), so an absolute input glues into a
	// doubled, non-existent path ("D:/Proj/F:/Engine/..") that still lexically
	// passes IsUnderDirectory(ProjectDir) yet fails to open. The helper therefore
	// only works for RELATIVE inputs (all the MCP file tools ever receive); reusing
	// it here would silently break every project + engine read. So: resolve to a
	// full path (relative → combined under the project; absolute → as-is), THEN
	// require containment in one of the two roots AFTER `..`-collapse — which keeps
	// the exact anti-bypass property (paths outside both roots return rejected).
	if (Method == TEXT("fs/read_text_file") && HasId && Params.IsValid())
	{
		const FString FilePath = Params->HasField(TEXT("path")) ? Params->GetStringField(TEXT("path")) : TEXT("");
		const FString Candidate = FPaths::IsRelative(FilePath)
			? FPaths::Combine(FPaths::ProjectDir(), FilePath)
			: FilePath;
		const FString ResolvedPath = FPaths::ConvertRelativePathToFull(Candidate);
		const FString ProjectDirFull = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString EngineDirFull = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		if (FilePath.IsEmpty()
			|| (!FPaths::IsUnderDirectory(ResolvedPath, ProjectDirFull)
				&& !FPaths::IsUnderDirectory(ResolvedPath, EngineDirFull)))
		{
			SendRpcError(AdapterId, ReqId, -32000,
				TEXT("Path not allowed. Reads are limited to the project directory and the engine installation."));
			return;
		}
		auto ReadIntParam = [&](const TCHAR* Key) -> int32
		{
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
			if (!Value.IsValid()) return 0;
			if (Value->Type == EJson::Number) return static_cast<int32>(Value->AsNumber());
			if (Value->Type == EJson::String) return FCString::Atoi(*Value->AsString());
			return 0;
		};
		int32 Line = ReadIntParam(TEXT("line"));
		int32 Limit = ReadIntParam(TEXT("limit"));

		FString Content;
		if (FFileHelper::LoadFileToString(Content, *ResolvedPath))
		{
			// Apply line/limit if specified
			if (Line > 0 || Limit > 0)
			{
				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines);
				int32 Start = FMath::Max(0, Line - 1);
				int32 End = Limit > 0 ? FMath::Min(Start + Limit, Lines.Num()) : Lines.Num();
				Content.Empty();
				for (int32 i = Start; i < End; ++i)
				{
					Content += Lines[i];
					if (i < End - 1) Content += TEXT("\n");
				}
			}

			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			Res->SetStringField(TEXT("content"), Content);
			SendRpcResult(AdapterId, ReqId, Res);
		}
		else
		{
			SendRpcError(AdapterId, ReqId, -32002, FString::Printf(TEXT("File not found: %s"), *FilePath));
		}
		return;
	}

	// terminal/create — agent wants to run a command. The exec site is AUTHORITATIVE
	// (council #2): enforce shell-gate + policy Block + per-command approval HERE,
	// independent of session/request_permission. No real spawn yet (Phase 4).
	if (Method == TEXT("terminal/create") && HasId && Params.IsValid())
	{
		const FString Command = Params->HasField(TEXT("command"))
			? Params->GetStringField(TEXT("command")) : TEXT("(unknown)");

		// Phase 3 runtime trace (pairs with the P1 SHELLTRACE) — reveals whether/when a
		// terminal/create follows a request_permission and whether the command hash matches,
		// resolving the P1-vs-P2 execution model before wiring the approval handoff.
		UE_LOG(LogTemp, Log, TEXT("Nwiro IK SHELLTRACE P2 terminal/create enabled=%d hash=%s"),
			!bBlockCommandExecution, *NwiroIKShellPolicy::NormalizedHash(Command));
		UE_LOG(LogTemp, Verbose, TEXT("Nwiro IK SHELLTRACE P2 cmd='%s'"), *Command);

		// 1) Shell globally disabled (default) ⇒ block.
		if (bBlockCommandExecution)
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: BLOCKED terminal/create — command='%s' (shell disabled)"), *Command);
			SendRpcError(AdapterId, ReqId, -32002, TEXT("Shell execution is disabled in Nwiro. Use nwiro MCP tools instead."));
			if (!ActiveChatId.IsEmpty()) EnqueueResponse(TEXT("system"), FString::Printf(TEXT("Blocked shell command: %s"), *Command), ActiveChatId);
			return;
		}

		// 2) Policy engine — hard-block forbidden commands server-side, regardless of any approval.
		const NwiroIKShellPolicy::FAssessment Assessment = NwiroIKShellPolicy::Assess(Command);
		if (Assessment.Risk == NwiroIKShellPolicy::ERisk::Block)
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: BLOCKED terminal/create — policy: '%s' (%s)"), *Command, *Assessment.Reason);
			SendRpcError(AdapterId, ReqId, -32002, FString::Printf(TEXT("Command blocked by Nwiro shell policy: %s"), *Assessment.Reason));
			if (!ActiveChatId.IsEmpty()) EnqueueResponse(TEXT("system"), FString::Printf(TEXT("Blocked shell command (policy): %s"), *Command), ActiveChatId);
			return;
		}

		// 3) Require a user approval bound to this EXACT command (created by the per-command
		//    modal — Phase 3). PEEK here (don't consume) so this deny-only stub can't burn a
		//    real approval; Phase 4 consumes it (one-shot) immediately before spawn.
		const FString Hash = NwiroIKShellPolicy::NormalizedHash(Command);
		if (!HasValidShellApproval(Hash))
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: DENIED terminal/create — no per-command approval: '%s'"), *Command);
			SendRpcError(AdapterId, ReqId, -32002, TEXT("Shell command requires explicit per-command approval in Nwiro."));
			return;
		}

		// 4) Approved + non-Block ⇒ Phase 4 will ConsumeShellApproval(Hash) then spawn via the
		//    process supervisor. Not yet built — deny without consuming.
		UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: terminal/create approved; execution pending Phase 4: '%s'"), *Command);
		SendRpcError(AdapterId, ReqId, -32002, TEXT("Shell execution is not yet available in this build."));
		return;
	}

	// terminal/output
	if (Method == TEXT("terminal/output") && HasId)
	{
		if (bBlockCommandExecution)
		{
			// Clean spec-compliant "finished with error" so a non-compliant agent that
			// ignored terminal:false does not read an empty {} as "still running" and
			// infinite-poll → wedge. exitStatus present = command is done.
			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			Res->SetStringField(TEXT("output"), TEXT("Shell execution is disabled in Nwiro. Use Nwiro MCP tools (e.g. execute_python) instead."));
			Res->SetBoolField(TEXT("truncated"), false);
			TSharedPtr<FJsonObject> ExitStatus = MakeShareable(new FJsonObject);
			ExitStatus->SetNumberField(TEXT("exitCode"), 1);
			Res->SetObjectField(TEXT("exitStatus"), ExitStatus);
			SendRpcResult(AdapterId, ReqId, Res);
			return;
		}
		const FString TermId = Params.IsValid() && Params->HasField(TEXT("terminalId"))
			? Params->GetStringField(TEXT("terminalId")) : TEXT("");
		const FTerminalResult* TerminalResult = TerminalResults.Find(TermId);
		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		Res->SetStringField(TEXT("output"), TerminalResult ? TerminalResult->Output : TEXT(""));
		Res->SetBoolField(TEXT("truncated"), false);
		TSharedPtr<FJsonObject> ExitStatus = MakeShareable(new FJsonObject);
		ExitStatus->SetNumberField(TEXT("exitCode"), TerminalResult ? TerminalResult->ExitCode : 0);
		Res->SetObjectField(TEXT("exitStatus"), ExitStatus);
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// terminal/wait_for_exit, terminal/kill, terminal/release
	if ((Method == TEXT("terminal/wait_for_exit") || Method == TEXT("terminal/kill") || Method == TEXT("terminal/release")) && HasId)
	{
		if (bBlockCommandExecution)
		{
			// wait_for_exit must report a definitive exit (non-zero) so the agent stops
			// waiting; kill/release are no-ops. Avoids the empty-{} "still running" wedge.
			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			if (Method == TEXT("terminal/wait_for_exit")) { Res->SetNumberField(TEXT("exitCode"), 1); }
			SendRpcResult(AdapterId, ReqId, Res);
			return;
		}
		const FString TermId = Params.IsValid() && Params->HasField(TEXT("terminalId"))
			? Params->GetStringField(TEXT("terminalId")) : TEXT("");
		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		if (Method == TEXT("terminal/wait_for_exit"))
		{
			const FTerminalResult* TerminalResult = TerminalResults.Find(TermId);
			Res->SetNumberField(TEXT("exitCode"), TerminalResult ? TerminalResult->ExitCode : 0);
		}
		else if (Method == TEXT("terminal/release"))
		{
			TerminalResults.Remove(TermId);
		}
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// mcp/connect — agent wants to connect to our in-process MCP server
	if (Method == TEXT("mcp/connect") && HasId && Params.IsValid())
	{
		FString AcpId = Params->HasField(TEXT("acpId")) ? Params->GetStringField(TEXT("acpId")) : TEXT("");

		TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
		Res->SetStringField(TEXT("connectionId"), TEXT("nwiro-mcp-conn"));
		SendRpcResult(AdapterId, ReqId, Res);
		return;
	}

	// mcp/message — agent sends MCP JSON-RPC messages to our in-process server
	if (Method == TEXT("mcp/message") && HasId && Params.IsValid())
	{
		TSharedPtr<FJsonObject> McpMsg = Params->HasField(TEXT("message"))
			? Params->GetObjectField(TEXT("message")) : nullptr;

		if (McpMsg.IsValid())
		{
			// Process the MCP message through our existing MCP server dispatch.
			//
			// This is the SECOND transport into the same dispatcher (the first
			// is HTTP POST /mcp) and it also runs on the game thread, so it
			// needs the async form too — an HTTP-only continuation would fix
			// direct Codex MCP calls while silently leaving every ACP call on
			// the refusal path. The sink captures the ACP correlation by value
			// and answers with SendRpcResult instead of an HTTP callback.
			// Bind the reply to THIS bridge object AND to the adapter's current
			// process generation. Unlike Pro's UI (which mints UUID request ids),
			// the ACP peer chooses ReqId and a restarted adapter can reuse it —
			// so a generation-N completion must not be written into generation
			// N+1, where it could satisfy an unrelated request with the same id.
			const FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
			const uint64 CapturedGeneration = AP ? AP->ProcessGeneration : 0;
			TWeakObjectPtr<UNwiroIKBridge> WeakThis(this);

			TSharedPtr<FJsonObject> McpResult;
			if (!FNwiroIKMCPServer::ProcessJsonRpcAsync(McpMsg,
				[WeakThis, AdapterId, ReqId, CapturedGeneration](TSharedPtr<FJsonObject> LateMcpResult)
				{
					UNwiroIKBridge* Self = WeakThis.Get();
					if (!Self) { return; }                       // bridge destroyed
					const FAdapterProcess* Now = Self->AdapterProcesses.Find(AdapterId);
					if (!Now || Now->ProcessGeneration != CapturedGeneration)
					{
						UE_LOG(LogTemp, Verbose,
							TEXT("Nwiro: dropping late MCP reply for stale adapter generation (%s)"),
							*AdapterId);
						return;                                   // adapter restarted
					}
					TSharedPtr<FJsonObject> LateRes = MakeShareable(new FJsonObject);
					LateRes->SetObjectField(TEXT("message"), LateMcpResult);
					Self->SendRpcResult(AdapterId, ReqId, LateRes);
				},
				McpResult))
			{
				return;  // parked; the sink owns the single reply
			}

			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			Res->SetObjectField(TEXT("message"), McpResult);
			SendRpcResult(AdapterId, ReqId, Res);

		}
		else
		{
			TSharedPtr<FJsonObject> Res = MakeShareable(new FJsonObject);
			SendRpcResult(AdapterId, ReqId, Res);
		}
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("Nwiro: Unhandled ACP method: %s"), *Method);
}

void UNwiroIKBridge::HandleSessionUpdate(const FString& AdapterId, const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update)
{
	// adapter-reliability-w1 codex-review-fix: drop updates for sessions we
	// explicitly cancelled (first_token_timeout). Without this, the
	// bProcessing-fallback below would happily route a late chunk from the
	// timed-out session into a fresh retry chat and stream stale text into
	// the new prompt's bubble. Per-adapter so adapter restart/removal clears
	// the set for free (codex-review-fix-2).
	if (!AcpSessionId.IsEmpty())
	{
		if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId);
			AP && AP->RejectedAcpSessionIds.Contains(AcpSessionId))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("Nwiro IK: dropping session/update for rejected adapter=%s sessionId=%s"),
				*AdapterId, *AcpSessionId);
			return;
		}
	}

	// Resolve chatId from ACP sessionId
	FString ChatId;
	FString* FoundChatId = SessionToChatId.Find(AcpSessionId);
	if (FoundChatId)
	{
		ChatId = *FoundChatId;
	}
	else
	{
		// A peer may stream before session/new returns. Attach that early update
		// only to an in-flight session on the same adapter. Never fall back to
		// ActiveChatId: stale output from a cancelled session could otherwise be
		// attributed to whichever turn the user started next.
		for (auto& Pair : ChatSessions)
		{
			const FString SessionAdapterId = Pair.Value.AdapterId.IsEmpty()
				? CurrentAdapter : Pair.Value.AdapterId;
			if (SessionAdapterId == AdapterId
				&& Pair.Value.bProcessing
				&& Pair.Value.SessionId.IsEmpty()
				&& Pair.Value.SessionRpcId != 0)
			{
				ChatId = Pair.Key;
				// Also set the mapping now so future events are routed correctly
				if (!AcpSessionId.IsEmpty())
				{
					Pair.Value.SessionId = AcpSessionId;
					SessionToChatId.Add(AcpSessionId, Pair.Key);
				}
				break;
			}
		}
		if (ChatId.IsEmpty())
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("Nwiro IK: dropping update for unmapped adapter=%s session=%s"),
				*AdapterId, *AcpSessionId);
			return;
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("Nwiro: SessionUpdate routed to chat %s (session %s)"), *ChatId, *AcpSessionId);

	FChatSession* Session = ChatSessions.Find(ChatId);
	if (!Session)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: dropping update for missing chat=%s session=%s"),
			*ChatId, *AcpSessionId);
		return;
	}
	const FString SessionAdapterId = Session->AdapterId.IsEmpty()
		? CurrentAdapter : Session->AdapterId;
	if (SessionAdapterId != AdapterId
		|| (!Session->SessionId.IsEmpty() && Session->SessionId != AcpSessionId))
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: dropping cross-session update adapter=%s chat=%s session=%s"),
			*AdapterId, *ChatId, *AcpSessionId);
		return;
	}

	FString UpdateType = Update->HasField(TEXT("sessionUpdate"))
		? Update->GetStringField(TEXT("sessionUpdate")) : TEXT("");

	// F4: latch that this turn produced genuine AGENT activity (content /
	// reasoning / tool call), so a later stray classified stdout line can't tear
	// down a live turn (see the emit-site guard). The whitelist deliberately
	// EXCLUDES pre-content noise — config_option_update / available_commands_update
	// / usage_update fall through to the unrecognized default, and user_message_chunk
	// is the prompt echo — so a genuine session-start auth/trust/MCP failure (which
	// arrives before any agent output) still classifies. Gated on a live, matchable,
	// non-replay turn.
	if (Session && Session->bProcessing && Session->PromptRpcId != 0 && !Session->bInReplay
		&& (UpdateType == TEXT("agent_message_chunk")
			|| UpdateType == TEXT("agent_thought_chunk")
			|| UpdateType == TEXT("tool_call")
			|| UpdateType == TEXT("tool_call_update")))
	{
		Session->bHasStreamed = true;
	}

	if (UpdateType == TEXT("user_message_chunk"))
	{
		// adapter-reliability-w6: user_message_chunk is always dropped silently.
		// Outside replay it's an echo of the prompt we just sent. During a
		// session/load replay the adapter re-emits prior user turns, but the
		// frontend already holds the full conversation in local (IndexedDB)
		// history, so it does not need them rebuilt from the replay. Worse, the
		// codex-acp adapter prepends the system preamble to every user turn (it
		// drops the ACP `instructions` field), so a replayed turn is
		// "<system preamble> + <question>" — surfacing it would dump the entire
		// system prompt into a chat bubble. Resume is handled in the background:
		// the adapter restores its own context internally on session/load and
		// the client just swallows the replay notifications.
		return;
	}

	const bool bTurnOutput = UpdateType == TEXT("agent_message_chunk")
		|| UpdateType == TEXT("agent_thought_chunk")
		|| UpdateType == TEXT("compact_boundary")
		|| UpdateType == TEXT("tool_call")
		|| UpdateType == TEXT("tool_call_update");
	if (bTurnOutput
		&& (!Session->bProcessing || Session->PromptRpcId == 0 || Session->bInReplay))
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("Nwiro IK: dropping turn output without a live prompt adapter=%s chat=%s session=%s type=%s"),
			*AdapterId, *ChatId, *AcpSessionId, *UpdateType);
		return;
	}

	if (UpdateType == TEXT("agent_message_chunk"))
	{
		if (Update->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = Update->GetObjectField(TEXT("content"));
			FString ContentType = Content->HasField(TEXT("type")) ? Content->GetStringField(TEXT("type")) : TEXT("");
			if (ContentType == TEXT("text") && Content->HasField(TEXT("text")))
			{
				// adapter-reliability-w6: during a session/load replay the adapter
				// re-streams the prior conversation's agent turns. Those must NOT
				// flip the live stage to "streaming" nor disarm first_token_timeout
				// (no real prompt is in flight) — gate every stage/telemetry side
				// effect on !bInReplay — AND the stream emit itself. Replayed bytes
				// never reach the UI; the frontend already has the prior turns in
				// its local history.
				if (!(Session && Session->bInReplay))
				{
					// adapter-reliability-w0: stage = streaming. Idempotent helper
					// dedupes per-chunk repetition so the log stays low-volume.
					const FString StreamAdapterId = (Session && !Session->AdapterId.IsEmpty())
						? Session->AdapterId : CurrentAdapter;
					SetAdapterStage(StreamAdapterId, ChatId, TEXT("streaming"));
					// adapter-reliability-w5: happy-path bytes-before-first-chunk log,
					// mirror of the timeout-path bytesThisTurn line. Same field shape so
					// grep "bytesBeforeFirstChunk|bytesThisTurn" gives a unified
					// latency-vs-bytes distribution across success + failure. Verbose
					// because this fires on every turn — flip on with
					// `Log LogTemp Verbose` when collecting distributions.
					if (Session && Session->PromptRpcStartedAt > 0.0)
					{
						const FAdapterProcess* APLog = AdapterProcesses.Find(StreamAdapterId);
						const int64 BytesBeforeFirstChunk = APLog
							? (APLog->BytesFromStdout - Session->BytesAtPromptSendStart)
							: -1;
						UE_LOG(LogTemp, Verbose,
							TEXT("Nwiro IK: first chunk received adapter=%s chat=%s elapsedMs=%d bytesBeforeFirstChunk=%lld"),
							*StreamAdapterId, *ChatId,
							(int32)((FPlatformTime::Seconds() - Session->PromptRpcStartedAt) * 1000.0),
							BytesBeforeFirstChunk);
					}
					// adapter-reliability-w1: first token has arrived — disarm
					// first_token_timeout. Long-running streams (multi-minute
					// generations) are now legitimate; we no longer time them out.
					if (Session) Session->PromptRpcStartedAt = 0.0;
					EnqueueResponse(TEXT("stream"), Content->GetStringField(TEXT("text")), ChatId);
				}
			}
		}
		return;
	}

	if (UpdateType == TEXT("agent_thought_chunk"))
	{
		// Reasoning-stream models (DeepSeek-R1, Qwen3, Gemma reasoning,
		// gemma4:latest in Ollama) can spend minutes deliberating before
		// emitting their first answer token. The shim now surfaces those
		// reasoning tokens as agent_thought_chunk; receiving one is hard
		// proof the model is alive and progressing, so we treat it the same
		// as a real first token for timeout purposes — disarm
		// PromptRpcStartedAt so the 300s first_token_timer cannot fire
		// while the model is genuinely thinking. Symmetry with the
		// agent_message_chunk branch above (line ~2019).
		if (Session) Session->PromptRpcStartedAt = 0.0;
		EnqueueResponse(TEXT("thinking"), TEXT(""), ChatId);
		return;
	}

	if (UpdateType == TEXT("compact_boundary"))
	{
		// Claude Agent SDK fires this when it starts compacting conversation
		// history — a legitimate, sometimes-slow operation (upstream issue:
		// github.com/anthropics/claude-code/issues/19567 documents indefinite
		// hangs during compaction). Previously unhandled: fell through this
		// entire if-chain with no PushEvent, so the frontend's 90s midstream
		// watchdog had no signal that the silence was legitimate progress.
		// Route through the existing 'system' notice channel rather than a
		// new event kind — it's already in the JS TRANSIENT list (bumps the
		// watchdog for free) and already renders via store.addSystem with no
		// JSON parsing, so no frontend changes are needed.
		if (Session) Session->PromptRpcStartedAt = 0.0;
		FString Trigger = GetOptionalStringField(Update, TEXT("trigger"));
		FString Notice = Trigger.Equals(TEXT("manual"), ESearchCase::IgnoreCase)
			? TEXT("Compacting conversation history...")
			: TEXT("Compacting conversation history (automatic)...");
		EnqueueResponse(TEXT("system"), Notice, ChatId);
		return;
	}

	if (UpdateType == TEXT("tool_call"))
	{

		// Map toolCallId → chatId for MCP permission routing
		FString ToolCallIdForMap = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");
		if (!ToolCallIdForMap.IsEmpty()) ToolUseToChatId.Add(ToolCallIdForMap, ChatId);

		FString ToolCallId = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");
		FString ToolStatus = Update->HasField(TEXT("status")) ? Update->GetStringField(TEXT("status")) : TEXT("");

		const FString ClaimKey = MakeToolCallKey(AcpSessionId, ToolCallId);
		const TCHAR* ClaimShellReason = ShellClassificationReason(Update);

		if (const TCHAR* ShellReason = ClaimShellReason)
		{
			if (!ToolCallId.IsEmpty()) BlockedShellToolCallIds.Add(MakeToolCallKey(AcpSessionId, ToolCallId));
			// Blocked ids carry no authorization: drop any claim an earlier frame left.
			if (!ClaimKey.IsEmpty()) NwiroMcpToolClaims.Remove(ClaimKey);
			FString Title = GetOptionalStringField(Update, TEXT("title"));
			if (Title.IsEmpty()) Title = ToolCallId.IsEmpty() ? TEXT("(unknown)") : ToolCallId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: BLOCKED shell tool_call '%s' (predicate: %s) before UI display"), *Title, ShellReason);
			if (!ChatId.IsEmpty())
			{
				EnqueueResponse(TEXT("system"),
					FString::Printf(TEXT("Blocked shell command: %s"), *Title),
					ChatId);
			}
			if (Session && Session->bProcessing)
			{
				if (!AcpSessionId.IsEmpty())
				{
					const FString ChatAdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
					TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
					CancelParams->SetStringField(TEXT("sessionId"), AcpSessionId);
					SendRpcNotification(ChatAdapterId, TEXT("session/cancel"), CancelParams);
				}
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
			return;
		}

		// Skip if already seen OR if it's a status update (not initial "pending")
		if (Session)
		{
			if (!ToolCallId.IsEmpty() && Session->SeenToolCallIds.Contains(ToolCallId)) return;
			if (!ToolCallId.IsEmpty()) Session->SeenToolCallIds.Add(ToolCallId);
		}

		// B1: record a claim for ids arriving as a RICH nwiro MCP tool_call. The matching
		// approval envelope carries the same id but no rawInput/title, so this is the only
		// point at which it can be identified.
		//
		// Deliberately BELOW the duplicate suppression above. Recorded higher up, a repeat
		// rich frame for an id whose claim had already been consumed would mint a fresh
		// one, quietly re-arming a one-shot. Pinned Codex 1.1.7 does not send that frame,
		// so this is defensive — but "one-shot" has to be true of the code, not of the
		// current adapter's habits.
		//
		// Recorded ONLY if the frame is not itself shell-shaped (classified above). An
		// earlier draft recorded first and relied on BlockedShellToolCallIds as a
		// "monotonic" poison record to override it later; that invariant was false, since
		// the poison is dropped when the call reaches a terminal status. Classifying first
		// means a shell-shaped frame never produces a claim, so there is nothing to
		// override.
		if (!AcpSessionId.IsEmpty() && !ToolCallId.IsEmpty() && ClaimShellReason == nullptr
			&& IsNwiroMcpToolCall(Update) && !BlockedShellToolCallIds.Contains(ClaimKey))
		{
			FNwiroMcpToolClaim Claim;
			Claim.AdapterId = AdapterId;
			const FAdapterProcess* ClaimProcess = AdapterProcesses.Find(AdapterId);
			Claim.ProcessGeneration = ClaimProcess ? ClaimProcess->ProcessGeneration : 0;
			Claim.Title = GetOptionalStringField(Update, TEXT("title"));
			// The turn that produced this evidence, stamped NOW. See FNwiroMcpToolClaim:
			// resolving these when the approval arrives labels a delayed frame with a
			// later turn's identity.
			Claim.ChatId = ChatId;
			Claim.TurnGeneration = Session->TurnGeneration;
			Claim.ModeSnapshot = Session->ModeSnapshot;
			// A claim minted with no live process could never match at approval time
			// (generation 0 is rejected there); do not store one.
			if (Claim.ProcessGeneration != 0) NwiroMcpToolClaims.Add(ClaimKey, Claim);
		}
		// Skip completed/failed tool_call events (they're final status updates)
		if (ToolStatus == TEXT("completed") || ToolStatus == TEXT("failed")) return;

		FString Title = Update->HasField(TEXT("title")) ? Update->GetStringField(TEXT("title")) : TEXT("");
		// Generic: extract tool name after last "/" or "__" separator
		int32 SlashIdx = INDEX_NONE;
		Title.FindLastChar('/', SlashIdx);
		if (SlashIdx != INDEX_NONE)
			Title = Title.Mid(SlashIdx + 1);
		// Also handle "__" separator (e.g. "mcp__nwiro__create_blueprint")
		int32 DunderIdx = Title.Find(TEXT("__"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (DunderIdx != INDEX_NONE)
			Title = Title.Mid(DunderIdx + 2);
		Title.ReplaceInline(TEXT("_"), TEXT(" "));

		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
		Data->SetStringField(TEXT("name"), Title);
		Data->SetStringField(TEXT("id"), ToolCallId);
		EnqueueResponse(TEXT("tool_start"), JsonToString(Data), ChatId);

		// Extract rawInput if present
		if (Update->HasField(TEXT("rawInput")))
		{
			TSharedPtr<FJsonObject> RawInput = Update->GetObjectField(TEXT("rawInput"));
			// Use "arguments" sub-object if present, otherwise whole rawInput
			FString InputStr;
			if (RawInput.IsValid() && RawInput->HasField(TEXT("arguments")))
				InputStr = JsonToString(RawInput->GetObjectField(TEXT("arguments")));
			else if (RawInput.IsValid())
				InputStr = JsonToString(RawInput);
			if (!InputStr.IsEmpty())
			{
				TSharedPtr<FJsonObject> UpdData = MakeShareable(new FJsonObject);
				UpdData->SetStringField(TEXT("id"), ToolCallId);
				UpdData->SetStringField(TEXT("input"), InputStr);
				EnqueueResponse(TEXT("tool_update"), JsonToString(UpdData), ChatId);
			}
		}
		return;
	}

	if (UpdateType == TEXT("tool_call_update"))
	{
		FString Status = Update->HasField(TEXT("status")) ? Update->GetStringField(TEXT("status")) : TEXT("");
		FString ToolCallId = Update->HasField(TEXT("toolCallId")) ? Update->GetStringField(TEXT("toolCallId")) : TEXT("");

		FString ToolCallKey = MakeToolCallKey(AcpSessionId, ToolCallId);

		// A finished call cannot be pending approval. Dropping the claim here bounds the
		// map and closes the window in which a late or replayed approval could still
		// find evidence for work that already completed.
		if (!ToolCallKey.IsEmpty() && (Status == TEXT("completed") || Status == TEXT("failed")))
		{
			NwiroMcpToolClaims.Remove(ToolCallKey);
		}

		if (!ToolCallKey.IsEmpty() && BlockedShellToolCallIds.Contains(ToolCallKey))
		{
			if (Status == TEXT("completed") || Status == TEXT("failed"))
			{
				BlockedShellToolCallIds.Remove(ToolCallKey);
			}
			return;
		}

		if (const TCHAR* ShellReason = ShellClassificationReason(Update))
		{
			if (!ToolCallKey.IsEmpty()) BlockedShellToolCallIds.Add(ToolCallKey);
			// Shell evidence on any later frame revokes the claim its first frame made.
			if (!ToolCallKey.IsEmpty()) NwiroMcpToolClaims.Remove(ToolCallKey);
			FString Title = GetOptionalStringField(Update, TEXT("title"));
			if (Title.IsEmpty()) Title = ToolCallId.IsEmpty() ? TEXT("(unknown)") : ToolCallId;
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: BLOCKED shell tool_call_update '%s' (predicate: %s) before UI display"), *Title, ShellReason);
			if (!ChatId.IsEmpty())
			{
				EnqueueResponse(TEXT("system"),
					FString::Printf(TEXT("Blocked shell command: %s"), *Title),
					ChatId);
			}
			if (Session && Session->bProcessing)
			{
				if (!AcpSessionId.IsEmpty())
				{
					const FString ChatAdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
					TSharedPtr<FJsonObject> CancelParams = MakeShareable(new FJsonObject);
					CancelParams->SetStringField(TEXT("sessionId"), AcpSessionId);
					SendRpcNotification(ChatAdapterId, TEXT("session/cancel"), CancelParams);
				}
				Session->bProcessing = false;
				EnqueueResponse(TEXT("done"), TEXT(""), ChatId);
			}
			return;
		}

		// Extract rawInput (comes as object, serialize to string)
		if (Update->HasField(TEXT("rawInput")))
		{
			TSharedPtr<FJsonObject> RawInput = Update->GetObjectField(TEXT("rawInput"));
			if (RawInput.IsValid() && RawInput->Values.Num() > 0)
			{
				FString InputStr = JsonToString(RawInput);
				TSharedPtr<FJsonObject> UpdData = MakeShareable(new FJsonObject);
				UpdData->SetStringField(TEXT("id"), ToolCallId);
				UpdData->SetStringField(TEXT("input"), InputStr);
				EnqueueResponse(TEXT("tool_update"), JsonToString(UpdData), ChatId);

			}
		}

		if (Status == TEXT("completed") || Status == TEXT("failed"))
		{
			TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
			Data->SetStringField(TEXT("id"), ToolCallId);
			Data->SetStringField(TEXT("status"), Status);

			// See ExtractAcpToolOutputText for the shapes this must handle — notably
			// codex-acp's { result: { content: [...] } }, which the previous inline
			// version missed entirely.
			const FString Combined = ExtractAcpToolOutputText(Update->TryGetField(TEXT("rawOutput")));

			if (!Combined.IsEmpty())
			{
				Data->SetStringField(TEXT("output"), Combined);
			}

			EnqueueResponse(TEXT("tool_end"), JsonToString(Data), ChatId);
		}
		return;
	}

	// Session info update
	if (UpdateType == TEXT("session_info_update"))
	{
		return; // Could update title
	}

	// Current mode update
	if (UpdateType == TEXT("current_mode_update"))
	{
		FString ModeId = Update->HasField(TEXT("currentModeId")) ? Update->GetStringField(TEXT("currentModeId")) : TEXT("");
		// adapter-reliability-w6: a replayed mode update reflects the loaded
		// session's historical mode, not the user's live selection — don't let
		// it clobber CurrentMode during a session/load replay.
		// Official Codex mode ids (agent/read-only) are transport policy, not
		// Nwiro UI modes (default/plan). Legacy Zed retains the generic update
		// behavior it used before the Windows successor migration.
		if (!NwiroCodex::UsesOfficialRuntime(AdapterId)
			&& !(Session && Session->bInReplay))
		{
			CurrentMode = ModeId;
		}
		return;
	}

	// Defensive: an unrecognized sessionUpdate type previously fell through
	// silently with no log and no PushEvent — the same shape of gap that hid
	// compact_boundary before this file added explicit handling for it above.
	// One log line turns any future unknown type into a fast confirm/correct
	// loop instead of a silent miss.
	if (!UpdateType.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("Nwiro IK: unrecognized sessionUpdate type '%s' (chat=%s)"), *UpdateType, *ChatId);
	}
}

// ============================================================
// Permission response
// ============================================================

void UNwiroIKBridge::RespondToPermission(int32 RequestId, const FString& OptionId)
{
	// ─── codex-acp-string-id ─────────────────────────────────────────────────
	// ACP permission card: bridge minted RequestId as a LocalPermId, the real
	// ACP id (which may be string/null) lives in the pending record. Look up and
	// echo with the original wire type so the agent can match it.
	// ─────────────────────────────────────────────────────────────────────────
	if (PermissionCoordinator.HasPending(RequestId))
	{
		// The coordinator derives allow/deny from the vocabulary it captured, validates
		// that the clicked id was actually offered, removes first, then revalidates the
		// origin context. The bridge deliberately computes nothing here: deciding the
		// semantics at this call site is how an unknown option id defaulted to allow.
		PermissionCoordinator.SettleByClick(RequestId, OptionId);
		return;
	}

	// MCP tool permission (in-process server). FNwiroIKMCPServer::NextPermissionId
	// starts at 1000, so its id range is naturally disjoint from LocalPermId.
	if (RequestId >= 1000)
	{
		bool bAllowed = OptionId.Contains(TEXT("allow")) || OptionId.Contains(TEXT("approved"));
		if (OptionId == TEXT("allow_session"))
		{
			FNwiroIKMCPServer::bSessionAllowed = true;
			bAllowed = true;
		}
		FNwiroIKMCPServer::RespondToToolPermission(RequestId, bAllowed);
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("Nwiro: RespondToPermission ignored unknown id %d (no ACP map entry, below MCP range)"),
		RequestId);
}

// ============================================================
// Adapter binary discovery
// ============================================================

FString UNwiroIKBridge::FindAdapterBinary() const
{
	if (NwiroCodex::UsesOfficialRuntime(CurrentAdapter))
	{
		const NwiroCodex::FLaunchSpec Spec = NwiroCodex::ResolveLaunchSpec();
		return Spec.IsValid() ? Spec.Executable : TEXT("");
	}

	const FAdapterInfo* Info = FindAdapter(CurrentAdapter);
	if (!Info) return TEXT("");

#if PLATFORM_WINDOWS
	FString ExeName = Info->BinaryName + TEXT(".exe");
#else
	FString ExeName = Info->BinaryName;
#endif

	// Search paths in priority order
	TArray<FString> SearchDirs = {
		FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("NwiroIntegrationKit"))->GetBaseDir(), TEXT("Binaries")),
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries")),
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroIntegrationKit")),
	};

	for (const FString& Dir : SearchDirs)
	{
		FString FullPath = FPaths::Combine(Dir, ExeName);
		if (NwiroPathExists(FullPath)) return FullPath;
	}

	return ExeName; // Fallback to PATH
}

bool UNwiroIKBridge::IsAdapterAvailable(const FString& Adapter)
{
	if (NwiroCodex::UsesOfficialRuntime(Adapter))
		return NwiroCodex::ResolveLaunchSpec().IsValid();

	FString OldAdapter = CurrentAdapter;
	CurrentAdapter = Adapter;
	FString Path = FindAdapterBinary();
	CurrentAdapter = OldAdapter;
	return NwiroPathExists(Path);
}

bool UNwiroIKBridge::IsCliAvailable(const FString& Adapter)
{
	FString CliName = Adapter; // claude, codex etc.

	// First check known paths
	const FAdapterInfo* Info = FindAdapter(Adapter);
	if (Info)
	{
		for (const FString& Candidate : Info->ExeCandidates)
		{
			if (NwiroPathExists(Candidate)) return true;
		}
	}

	// Fallback: use 'where' (Windows) to search PATH
#if PLATFORM_WINDOWS
	int32 RetCode = 0;
	FString StdOut, StdErr;
	FPlatformProcess::ExecProcess(TEXT("where"), *CliName, &RetCode, &StdOut, &StdErr);
	return RetCode == 0 && !StdOut.TrimStartAndEnd().IsEmpty();
#else
	int32 RetCode = 0;
	FString StdOut, StdErr;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/which"), *CliName, &RetCode, &StdOut, &StdErr);
	return RetCode == 0;
#endif
}

FString UNwiroIKBridge::GetAdapterRuntimeStatus(const FString& Adapter)
{
	TSharedPtr<FJsonObject> Status = MakeShareable(new FJsonObject);
	Status->SetStringField(TEXT("adapter"), Adapter);
	if (Adapter != TEXT("codex"))
	{
		Status->SetStringField(TEXT("state"), TEXT("error"));
		Status->SetStringField(TEXT("source"), TEXT("none"));
		Status->SetStringField(TEXT("errorCode"), TEXT("unsupported_adapter"));
		return JsonToString(Status);
	}

#if !PLATFORM_WINDOWS
	// The official successor runtime is intentionally Windows-only for now.
	// Report that boundary explicitly so renderer capability gates cannot
	// mistake an unrelated relative directory for a managed runtime when
	// RuntimeBaseDir() is unavailable on this platform.
	Status->SetStringField(TEXT("state"), TEXT("not_installed"));
	Status->SetStringField(TEXT("source"), TEXT("none"));
	Status->SetStringField(TEXT("errorCode"), TEXT("unsupported_platform"));
	Status->SetBoolField(TEXT("canInstall"), false);
	TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject);
	Node->SetBoolField(TEXT("available"), false);
	Node->SetBoolField(TEXT("supported"), false);
	TSharedPtr<FJsonObject> Npm = MakeShareable(new FJsonObject);
	Npm->SetBoolField(TEXT("available"), false);
	TSharedPtr<FJsonObject> Prerequisites = MakeShareable(new FJsonObject);
	Prerequisites->SetObjectField(TEXT("node"), Node);
	Prerequisites->SetObjectField(TEXT("npm"), Npm);
	Status->SetObjectField(TEXT("prerequisites"), Prerequisites);
	return JsonToString(Status);
#else
	const NwiroCodex::FNodeToolchain Toolchain = NwiroCodex::ResolveNodeToolchain();
	const NwiroCodex::FLaunchSpec Launch = NwiroCodex::ResolveLaunchSpec();
	const bool bManagedRuntimeReady = NwiroCodex::IsRuntimeRootValid(
		NwiroCodex::ManagedRuntimeDir(), NwiroCodex::ERuntimeValidation::ManagedComplete);
	const bool bGlobalRuntimeReady = Launch.IsValid() && Launch.Source == TEXT("global");
	const NwiroCodex::EManagedInstallDecision InstallDecision =
		NwiroCodex::DecideManagedInstall(
			bManagedRuntimeReady, bGlobalRuntimeReady, Launch.Version);
	Status->SetBoolField(
		TEXT("canInstall"),
		InstallDecision == NwiroCodex::EManagedInstallDecision::InstallManaged
			&& !bManagedCodexInstallInProgress
			&& Toolchain.bNodeSupported
			&& !Toolchain.NpmCliPath.IsEmpty());
	// Runtime ownership is independent from current launch readiness. Keep a
	// verified Nwiro-owned tree removable even when Node.js is temporarily
	// missing or unsupported.
	const FString RuntimeSource = bManagedRuntimeReady
		? TEXT("managed")
		: (Launch.IsValid() ? Launch.Source : TEXT("none"));
	const FString RuntimeVersion = bManagedRuntimeReady
		? FString(NwiroCodex::PackageVersion)
		: (Launch.IsValid() ? Launch.Version : TEXT(""));
	Status->SetStringField(TEXT("source"), RuntimeSource);
	if (!RuntimeVersion.IsEmpty()) Status->SetStringField(TEXT("version"), RuntimeVersion);

	TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject);
	Node->SetBoolField(TEXT("available"), !Toolchain.NodePath.IsEmpty());
	Node->SetBoolField(TEXT("supported"), Toolchain.bNodeSupported);
	if (!Toolchain.NodeVersion.IsEmpty()) Node->SetStringField(TEXT("version"), Toolchain.NodeVersion);
	TSharedPtr<FJsonObject> Npm = MakeShareable(new FJsonObject);
	Npm->SetBoolField(TEXT("available"), !Toolchain.NpmCliPath.IsEmpty());
	if (!Toolchain.NpmVersion.IsEmpty()) Npm->SetStringField(TEXT("version"), Toolchain.NpmVersion);
	TSharedPtr<FJsonObject> Prerequisites = MakeShareable(new FJsonObject);
	Prerequisites->SetObjectField(TEXT("node"), Node);
	Prerequisites->SetObjectField(TEXT("npm"), Npm);
	Status->SetObjectField(TEXT("prerequisites"), Prerequisites);

	if (bManagedCodexInstallInProgress)
	{
		Status->SetStringField(TEXT("state"), TEXT("installing"));
	}
	else if (Launch.IsValid())
	{
		Status->SetStringField(TEXT("state"), TEXT("ready"));
	}
	else if (bManagedRuntimeReady)
	{
		Status->SetStringField(TEXT("state"), TEXT("error"));
		if (Toolchain.NodePath.IsEmpty()) Status->SetStringField(TEXT("errorCode"), TEXT("node_missing"));
		else if (!Toolchain.bNodeSupported) Status->SetStringField(TEXT("errorCode"), TEXT("node_unsupported"));
		else Status->SetStringField(TEXT("errorCode"), TEXT("runtime_unavailable"));
	}
	else
	{
		Status->SetStringField(TEXT("state"), TEXT("not_installed"));
		if (Toolchain.NodePath.IsEmpty()) Status->SetStringField(TEXT("errorCode"), TEXT("node_missing"));
		else if (!Toolchain.bNodeSupported) Status->SetStringField(TEXT("errorCode"), TEXT("node_unsupported"));
		else if (Toolchain.NpmCliPath.IsEmpty()) Status->SetStringField(TEXT("errorCode"), TEXT("npm_missing"));
	}
	return JsonToString(Status);
#endif
}

void UNwiroIKBridge::InstallManagedAdapter(const FString& Adapter)
{
	BeginManagedAdapterInstall(Adapter);
}

void UNwiroIKBridge::BeginManagedAdapterInstall(const FString& Adapter)
{
	if (Adapter != TEXT("codex"))
	{
		EnqueueResponse(TEXT("error"), TEXT("Managed installation is only available for the official Codex ACP runtime."));
		return;
	}
#if !PLATFORM_WINDOWS
	PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"unsupported_platform\",\"errorDetail\":\"Managed Codex ACP installation is currently available on Windows only.\"}"));
	return;
#else
	if (bManagedCodexInstallInProgress) return;
	const bool bManagedRuntimeReady = NwiroCodex::IsRuntimeRootValid(
		NwiroCodex::ManagedRuntimeDir(), NwiroCodex::ERuntimeValidation::ManagedComplete);
	const NwiroCodex::FLaunchSpec ExistingLaunch = NwiroCodex::ResolveLaunchSpec();
	const bool bGlobalRuntimeReady =
		ExistingLaunch.IsValid() && ExistingLaunch.Source == TEXT("global");
	const NwiroCodex::EManagedInstallDecision InstallDecision =
		NwiroCodex::DecideManagedInstall(
			bManagedRuntimeReady, bGlobalRuntimeReady, ExistingLaunch.Version);
	if (InstallDecision != NwiroCodex::EManagedInstallDecision::InstallManaged)
	{
		// A managed runtime installed by another editor may already be valid
		// while this editor still owns an older global process. Converge the
		// process source without touching the verified managed files.
		const bool bSourceReady =
			InstallDecision != NwiroCodex::EManagedInstallDecision::UseManaged
			|| QuiesceCodexProcessForRuntimeChange(TEXT("global"));
		if (bSourceReady)
		{
			PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"done\"}"));
		}
		else
		{
			PushEvent(TEXT("download_progress"),
				TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"runtime_switch_failed\",\"errorDetail\":\"The Nwiro-managed Codex runtime is installed, but the existing global Codex process could not be stopped safely. Restart Unreal Editor to use the managed runtime.\"}"));
		}
		return;
	}

	const NwiroCodex::FNodeToolchain Toolchain = NwiroCodex::ResolveNodeToolchain();
	FString ErrorCode;
	FString ErrorDetail;
	if (Toolchain.NodePath.IsEmpty())
	{
		ErrorCode = TEXT("node_missing");
		ErrorDetail = TEXT("Node.js 20 or newer is required for the official Codex ACP runtime.");
	}
	else if (!Toolchain.bNodeSupported)
	{
		ErrorCode = TEXT("node_unsupported");
		ErrorDetail = TEXT("The detected Node.js version is unsupported; Node.js 20 or newer is required.");
	}
	else if (Toolchain.NpmCliPath.IsEmpty())
	{
		ErrorCode = TEXT("npm_missing");
		ErrorDetail = TEXT("npm was not found beside the detected Node.js installation.");
	}
	if (!ErrorCode.IsEmpty())
	{
		PushEvent(TEXT("download_progress"), FString::Printf(
			TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"%s\",\"errorDetail\":\"%s\"}"),
			*ErrorCode, *NwiroJsonEscape(ErrorDetail)));
		return;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NwiroIntegrationKit"));
	const FString ResourceDir = Plugin.IsValid()
		? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("CodexAcpRuntime"))
		: TEXT("");
	const FString SourcePackage = FPaths::Combine(ResourceDir, TEXT("package.json"));
	const FString SourceLock = FPaths::Combine(ResourceDir, TEXT("package-lock.json"));
	if (!NwiroPathExists(SourcePackage) || !NwiroPathExists(SourceLock))
	{
		PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"installer_resources_missing\",\"errorDetail\":\"The Codex runtime installer resources are missing from this plugin build.\"}"));
		return;
	}

	bManagedCodexInstallInProgress = true;
	PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"starting\",\"phase\":\"installing\"}"));
	const FString RuntimeBase = NwiroCodex::RuntimeBaseDir();
	const FString TargetDir = NwiroCodex::ManagedRuntimeDir();
	const FString OperationId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString StagingDir = FPaths::Combine(RuntimeBase, TEXT(".staging-") + OperationId);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), Toolchain, SourcePackage, SourceLock,
			TargetDir, StagingDir]()
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		bool bSuccess = false;
		FString FailureCode = TEXT("install_failed");
		FString FailureDetail = TEXT("The official Codex ACP runtime could not be installed.");

		PlatformFile.CreateDirectoryTree(*StagingDir);
		const bool bCopiedManifest = IFileManager::Get().Copy(
			*FPaths::Combine(StagingDir, TEXT("package.json")), *SourcePackage, true, true) == COPY_OK;
		const bool bCopiedLock = IFileManager::Get().Copy(
			*FPaths::Combine(StagingDir, TEXT("package-lock.json")), *SourceLock, true, true) == COPY_OK;
		if (!bCopiedManifest || !bCopiedLock)
		{
			FailureCode = TEXT("installer_resources_copy_failed");
			FailureDetail = TEXT("The Codex runtime installer could not prepare its managed directory.");
		}
		else
		{
			const FString InstallArgs = NwiroCodex::QuoteArg(Toolchain.NpmCliPath)
				+ TEXT(" ci --prefix ") + NwiroCodex::QuoteArg(StagingDir)
				+ TEXT(" --omit=dev --include=optional --ignore-scripts --no-audit --no-fund --no-update-notifier")
				+ TEXT(" --fetch-retries=2 --fetch-retry-mintimeout=1000 --fetch-retry-maxtimeout=10000 --fetch-timeout=60000")
				+ TEXT(" --registry=https://registry.npmjs.org/");
			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			FPlatformProcess::ExecProcess(*Toolchain.NodePath, *InstallArgs, &ReturnCode, &StdOut, &StdErr);
			if (ReturnCode != 0)
			{
				// npm output may contain user-specific absolute paths. The
				// renderer receives a stable error code; keep logs path-free.
				UE_LOG(LogTemp, Error, TEXT("Nwiro IK: managed Codex npm install failed code=%d"),
					ReturnCode);
			}
			else if (!NwiroCodex::IsRuntimeRootValid(
				StagingDir, NwiroCodex::ERuntimeValidation::ManagedStaging))
			{
				FailureCode = TEXT("integrity_failed");
				FailureDetail = TEXT("The installed Codex runtime did not match the pinned official package.");
			}
			else
			{
				const FString EntryPath = FPaths::Combine(NwiroCodex::PackageDir(StagingDir), TEXT("dist"), TEXT("index.js"));
				const FString VerifyArgs = NwiroCodex::QuoteArg(EntryPath) + TEXT(" --version");
				ReturnCode = -1;
				StdOut.Empty();
				StdErr.Empty();
				FPlatformProcess::ExecProcess(*Toolchain.NodePath, *VerifyArgs, &ReturnCode, &StdOut, &StdErr);
				const FString ExpectedIdentity = FString::Printf(TEXT("%s %s"), NwiroCodex::PackageName, NwiroCodex::PackageVersion);
				if (ReturnCode != 0 || NwiroCodex::FirstNonEmptyLine(StdOut) != ExpectedIdentity)
				{
					FailureCode = TEXT("runtime_verify_failed");
					FailureDetail = TEXT("The installed Codex runtime failed its identity check.");
				}
				else
				{
					const FString Marker = FString::Printf(
						TEXT("{\"package\":\"%s\",\"version\":\"%s\"}"),
						NwiroCodex::PackageName, NwiroCodex::PackageVersion);
					const bool bMarkerWritten = FFileHelper::SaveStringToFile(
						Marker, *FPaths::Combine(StagingDir, TEXT(".install-complete.json")));
					if (!bMarkerWritten || !NwiroCodex::IsRuntimeRootValid(
						StagingDir, NwiroCodex::ERuntimeValidation::ManagedComplete))
					{
						FailureCode = TEXT("integrity_failed");
						FailureDetail = TEXT("The verified Codex runtime could not be committed in staging.");
					}
					else
					{
						// Process ownership lives on the game thread. Stop this
						// editor's managed tree first so its shared runtime lease
						// is released, then atomically contend for the exclusive
						// cross-editor mutation lease on the worker.
						const TSharedRef<NwiroCodex::FGameThreadBarrier, ESPMode::ThreadSafe> Barrier =
							MakeShared<NwiroCodex::FGameThreadBarrier, ESPMode::ThreadSafe>();
						AsyncTask(ENamedThreads::GameThread, [WeakThis, Barrier]()
						{
							if (Barrier->TryBegin())
							{
								if (UNwiroIKBridge* This = WeakThis.Get())
								{
									Barrier->bCompleted =
										This->QuiesceCodexProcessForRuntimeChange(TEXT("managed"));
								}
							}
							Barrier->Event->Trigger();
						});

						bool bBarrierSignaled = Barrier->Event->Wait(30000);
						if (!bBarrierSignaled && !Barrier->CancelIfPending())
						{
							// The game-thread callback already began. Wait for
							// its positive tree-drained acknowledgement rather
							// than racing it.
							Barrier->Event->Wait();
							bBarrierSignaled = true;
						}
						const bool bQuiesced = bBarrierSignaled && Barrier->bCompleted;
						if (!bQuiesced)
						{
							FailureCode = TEXT("runtime_quiesce_failed");
							FailureDetail = TEXT("The managed Codex process tree could not be stopped safely for replacement.");
						}
						else
						{
							void* MutationLockHandle = nullptr;
#if PLATFORM_WINDOWS
							const bool bMutationLockAcquired =
								NwiroCodex::AcquireRuntimeFileLock(true, MutationLockHandle);
#else
							const bool bMutationLockAcquired = true;
#endif
							if (!bMutationLockAcquired)
							{
								// A concurrent editor may have won publication
								// and immediately taken a shared runtime lease.
								// Validate only while holding our own shared
								// lease so an exclusive delete cannot race the
								// idempotent no-op decision.
								bool bConcurrentTargetReady = false;
#if PLATFORM_WINDOWS
								void* ValidationLockHandle = nullptr;
								if (NwiroCodex::AcquireRuntimeFileLock(false, ValidationLockHandle))
								{
									bConcurrentTargetReady = NwiroCodex::IsRuntimeRootValid(
										TargetDir, NwiroCodex::ERuntimeValidation::ManagedComplete);
								}
								NwiroCodex::ReleaseRuntimeFileLock(ValidationLockHandle);
#endif
								if (bConcurrentTargetReady)
								{
									bSuccess = true;
								}
								else
								{
									FailureCode = TEXT("managed_runtime_in_use");
									FailureDetail = TEXT("Another Unreal Editor is using or updating the managed Codex runtime.");
								}
							}
							else
							{
								// Re-evaluate only after taking the exclusive
								// lease: another editor may have completed an
								// install while this editor was staging npm.
								const bool bTargetExists = PlatformFile.DirectoryExists(*TargetDir);
								const bool bExistingTargetReady = bTargetExists
									&& NwiroCodex::IsRuntimeRootValid(
										TargetDir, NwiroCodex::ERuntimeValidation::ManagedComplete);

								// Never replace a healthy managed runtime. This
								// under-lock recheck makes concurrent installs
								// converge on the first verified publisher.
								if (bExistingTargetReady)
								{
									bSuccess = true;
								}
								else
								{
									// Only an absent or already-invalid
									// Nwiro-owned target reaches this branch.
									// Publish marker-last so readers never
									// accept a partial copy as runnable.
									const FString StagingMarker = FPaths::Combine(
										StagingDir, TEXT(".install-complete.json"));
									const bool bStagingMarkerRemoved =
										PlatformFile.DeleteFile(*StagingMarker);
									const bool bTargetCleared = bStagingMarkerRemoved
										&& (!bTargetExists
											|| PlatformFile.DeleteDirectoryRecursively(*TargetDir));
									const bool bPromoted = bTargetCleared
										&& PlatformFile.CopyDirectoryTree(*TargetDir, *StagingDir, false);
									const bool bPromotedTreeValid = bPromoted
										&& NwiroCodex::IsRuntimeRootValid(
											TargetDir, NwiroCodex::ERuntimeValidation::ManagedStaging);
									const bool bTargetMarkerWritten = bPromotedTreeValid
										&& FFileHelper::SaveStringToFile(
											Marker, *FPaths::Combine(TargetDir, TEXT(".install-complete.json")));
									bSuccess = bTargetMarkerWritten
										&& NwiroCodex::IsRuntimeRootValid(
											TargetDir, NwiroCodex::ERuntimeValidation::ManagedComplete);
									if (!bSuccess)
									{
										// No valid managed runtime was removed;
										// leave the owned target clean for an
										// idempotent retry.
										if (PlatformFile.DirectoryExists(*TargetDir))
											PlatformFile.DeleteDirectoryRecursively(*TargetDir);
										FailureCode = TEXT("runtime_promote_failed");
										FailureDetail =
											TEXT("The verified Codex runtime could not be activated.");
									}
								}
							}
#if PLATFORM_WINDOWS
							NwiroCodex::ReleaseRuntimeFileLock(MutationLockHandle);
#endif
						}
					}
				}
			}
		}

		// Promotion copies from the unique staging directory, so every outcome
		// (including a concurrent-install no-op) must remove that directory.
		if (PlatformFile.DirectoryExists(*StagingDir))
			PlatformFile.DeleteDirectoryRecursively(*StagingDir);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, bSuccess, FailureCode, FailureDetail]()
		{
			UNwiroIKBridge* This = WeakThis.Get();
			if (!This) return;
			This->bManagedCodexInstallInProgress = false;
			if (bSuccess)
			{
				// Installing managed over an older global package changes the
				// selected source. Stop only that global Codex process so the
				// next use resolves the newly verified managed runtime.
				if (This->QuiesceCodexProcessForRuntimeChange(TEXT("global")))
				{
					This->PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"done\"}"));
				}
				else
				{
					// Publication is already complete and remains valid. Do not
					// roll it back; report that this editor must restart before
					// it can safely switch away from the old global process.
					This->PushEvent(TEXT("download_progress"),
						TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"runtime_switch_failed\",\"errorDetail\":\"The Nwiro-managed Codex runtime is installed, but the existing global Codex process could not be stopped safely. Restart Unreal Editor to use the managed runtime.\"}"));
				}
			}
			else
			{
				This->PushEvent(TEXT("download_progress"), FString::Printf(
					TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"%s\",\"errorDetail\":\"%s\"}"),
					*FailureCode, *NwiroJsonEscape(FailureDetail)));
			}
		});
	});
#endif
}

void UNwiroIKBridge::DeleteAdapter(const FString& Adapter)
{
	if (NwiroCodex::UsesOfficialRuntime(Adapter))
	{
#if !PLATFORM_WINDOWS
		PushEvent(TEXT("download_progress"),
			TEXT("{\"adapter\":\"codex\",\"status\":\"delete_failed\",\"errorCode\":\"unsupported_platform\",\"errorDetail\":\"Managed Codex ACP removal is currently available on Windows only.\"}"));
		return;
#else
		if (bManagedCodexInstallInProgress)
		{
			PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"delete_failed\",\"errorDetail\":\"The managed Codex integration cannot be removed while installation is in progress.\"}"));
			return;
		}

		// A global Codex process does not map Nwiro-owned files and is left
		// untouched. A managed process must positively drain its full Job tree
		// before this editor releases its shared runtime lease.
		if (!QuiesceCodexProcessForRuntimeChange(TEXT("managed")))
		{
			PushEvent(TEXT("download_progress"),
				TEXT("{\"adapter\":\"codex\",\"status\":\"delete_failed\",\"errorCode\":\"runtime_quiesce_failed\",\"errorDetail\":\"The managed Codex process tree could not be stopped safely for removal.\"}"));
			return;
		}

		// Reuse the mutation-in-progress gate so a chat in this editor cannot
		// start a fresh managed process between local quiesce and exclusive
		// cross-editor lock acquisition.
		bManagedCodexInstallInProgress = true;
		const FString ManagedDir = NwiroCodex::ManagedRuntimeDir();
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
			[WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), ManagedDir, Adapter]()
		{
			void* MutationLockHandle = nullptr;
			const bool bMutationLockAcquired =
				NwiroCodex::AcquireRuntimeFileLock(true, MutationLockHandle);
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			const bool bExisted = bMutationLockAcquired
				&& PlatformFile.DirectoryExists(*ManagedDir);
			const bool bDeleted = bMutationLockAcquired
				&& (!bExisted || PlatformFile.DeleteDirectoryRecursively(*ManagedDir));
			NwiroCodex::ReleaseRuntimeFileLock(MutationLockHandle);

			const FString FailureCode = bMutationLockAcquired
				? TEXT("delete_failed")
				: TEXT("managed_runtime_in_use");
			const FString FailureDetail = bMutationLockAcquired
				? TEXT("The managed Codex runtime could not be removed.")
				: TEXT("Another Unreal Editor is using or updating the managed Codex runtime.");
			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, bDeleted, Adapter, FailureCode, FailureDetail]()
			{
				if (UNwiroIKBridge* This = WeakThis.Get())
				{
					This->bManagedCodexInstallInProgress = false;
					if (bDeleted)
					{
						This->PushEvent(TEXT("download_progress"), FString::Printf(
							TEXT("{\"adapter\":\"%s\",\"status\":\"deleted\"}"),
							*Adapter));
					}
					else
					{
						This->PushEvent(TEXT("download_progress"), FString::Printf(
							TEXT("{\"adapter\":\"%s\",\"status\":\"delete_failed\",\"errorCode\":\"%s\",\"errorDetail\":\"%s\"}"),
							*Adapter, *FailureCode, *NwiroJsonEscape(FailureDetail)));
					}
				}
			});
		});
		return;
#endif
	}

	const FAdapterInfo* Info = FindAdapter(Adapter);
	if (!Info) return;

#if PLATFORM_WINDOWS
	FString ExeName = Info->BinaryName + TEXT(".exe");
#else
	FString ExeName = Info->BinaryName;
#endif

	FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroIntegrationKit"));
	FString ExePath = FPaths::Combine(SaveDir, ExeName);

	// Kill adapter process on game thread (fast)
	KillProcess();

	// Background: force-kill + wait + delete. Capture a TWeakObjectPtr instead of
	// raw `this`: the 1s sleep below widens the window in which the bridge UObject
	// can be GC'd (e.g. editor shutdown right after a delete), and the GameThread
	// continuation must not deref a freed bridge. Mirrors DownloadAdapter/EnsureProcess.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), ExeName, ExePath, Adapter]()
	{
#if PLATFORM_WINDOWS
		FString KillCmd = FString::Printf(TEXT("/C taskkill /F /IM \"%s\" >nul 2>&1"), *ExeName);
		FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *KillCmd, nullptr, nullptr, nullptr);
#endif
		FPlatformProcess::Sleep(1.0f);

		bool bDeleted = false;
		if (FPaths::FileExists(ExePath))
			bDeleted = IFileManager::Get().Delete(*ExePath, false, true);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, bDeleted, Adapter]()
		{
			UNwiroIKBridge* This = WeakThis.Get();
			if (!This) return;
			if (bDeleted)
				This->PushEvent(TEXT("download_progress"), FString::Printf(TEXT("{\"adapter\":\"%s\",\"status\":\"deleted\"}"), *Adapter));
			else
				This->PushEvent(TEXT("download_progress"), FString::Printf(TEXT("{\"adapter\":\"%s\",\"status\":\"delete_failed\"}"), *Adapter));
		});
	});
}

void UNwiroIKBridge::DownloadAdapter(const FString& Adapter, const FString& Url)
{
	if (NwiroCodex::UsesOfficialRuntime(Adapter))
	{
		// Fail closed for older renderers that still carry the archived Zed URL.
		// The new fixed-input method is the only supported Codex acquisition path.
		PushEvent(TEXT("download_progress"), TEXT("{\"adapter\":\"codex\",\"status\":\"error\",\"errorCode\":\"managed_install_required\",\"errorDetail\":\"This plugin accepts only the managed official Codex ACP runtime.\"}"));
		return;
	}

	const FAdapterInfo* Info = FindAdapter(Adapter);
	if (!Info)
	{
		EnqueueResponse(TEXT("error"), FString::Printf(TEXT("Unknown adapter: %s"), *Adapter));
		return;
	}

#if PLATFORM_WINDOWS
	FString BinaryName = Info->BinaryName + TEXT(".exe");
#else
	FString BinaryName = Info->BinaryName;
#endif

	// Some adapters ship .tar.gz on Unix (codex), others ship .zip (claude on
	// every platform, codex on Windows). Pick the suffix from the URL so we
	// save it with the right extension and pick the right extractor below.
	const bool bIsTarGz = Url.EndsWith(TEXT(".tar.gz")) || Url.EndsWith(TEXT(".tgz"));
	const FString ArchiveExt = bIsTarGz ? TEXT("-download.tar.gz") : TEXT("-download.zip");
	FString ZipName = Info->BinaryName + ArchiveExt;

	FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroIntegrationKit"));
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*SaveDir);

	FString ZipPath = FPaths::Combine(SaveDir, ZipName);
	FString ExePath = FPaths::Combine(SaveDir, BinaryName);
	// URL must be supplied by the JS layer — no hardcoded fallback so a stale
	// release URL can never sneak in via a recompiled plugin.
	FString DownloadUrl = Url.IsEmpty() ? Info->DownloadUrl : Url;
	if (DownloadUrl.IsEmpty())
	{
		EnqueueResponse(TEXT("error"), FString::Printf(
			TEXT("No download URL provided for %s. The frontend should pass it via bridge.downloadadapter(adapter, url)."),
			*Adapter));
		return;
	}

	// Already downloaded?
	if (FPaths::FileExists(ExePath))
	{
		PushEvent(TEXT("download_progress"), FString::Printf(TEXT("{\"adapter\":\"%s\",\"status\":\"done\"}"), *Adapter));
		EnqueueResponse(TEXT("system"), FString::Printf(TEXT("%s is already available."), *Adapter));
		return;
	}

	PushEvent(TEXT("download_progress"), FString::Printf(TEXT("{\"adapter\":\"%s\",\"progress\":0,\"total\":0,\"status\":\"starting\"}"), *Adapter));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(DownloadUrl);
	HttpRequest->SetVerb(TEXT("GET"));

	HttpRequest->OnRequestProgress64().BindLambda([WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), Adapter](FHttpRequestPtr, uint64 BytesSent, uint64 BytesReceived)
	{
		float ProgressMB = static_cast<float>(BytesReceived) / (1024.0f * 1024.0f);
		// HTTP progress fires on a worker thread and the bridge UObject may be
		// GC'd by then — marshal PushEvent to the game thread and guard lifetime.
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Payload = FString::Printf(TEXT("{\"adapter\":\"%s\",\"progress\":%.1f,\"status\":\"downloading\"}"), *Adapter, ProgressMB)]()
		{
			if (UNwiroIKBridge* T = WeakThis.Get()) T->PushEvent(TEXT("download_progress"), Payload);
		});
	});

	HttpRequest->OnProcessRequestComplete().BindLambda([WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), ZipPath, ExePath, SaveDir, Adapter, bIsTarGz](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
	{
		if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
			const FString Detail = FString::Printf(
				TEXT("Download HTTP failed (success=%s, code=%d) — check network, proxy, or GitHub availability"),
				bSuccess ? TEXT("true") : TEXT("false"), Code);
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: adapter '%s' %s"), *Adapter, *Detail);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Payload = FString::Printf(
				TEXT("{\"adapter\":\"%s\",\"status\":\"error\",\"errorDetail\":\"%s\"}"),
				*Adapter, *NwiroJsonEscape(Detail))]()
			{
				if (UNwiroIKBridge* T = WeakThis.Get()) T->PushEvent(TEXT("download_progress"), Payload);
			});
			return;
		}

		// Save archive. SaveArrayToFile returns false on disk-full, AV mid-
		// write quarantine, or read-only Saved/ — previously discarded, which
		// left the failure to surface as a phantom "extraction error" later.
		// Absolute path goes to UE_LOG (audit trail) but not to errorDetail —
		// the latter would leak Windows usernames via toasts (CWE-209).
		const bool bSaved = FFileHelper::SaveArrayToFile(Response->GetContent(), *ZipPath);
		if (!bSaved)
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: adapter '%s' could not write archive to %s"), *Adapter, *ZipPath);
			const FString Detail = TEXT("Could not write archive to Saved/NwiroIntegrationKit — disk full, antivirus quarantine, or read-only Saved/ folder");
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Payload = FString::Printf(
				TEXT("{\"adapter\":\"%s\",\"status\":\"error\",\"errorDetail\":\"%s\"}"),
				*Adapter, *NwiroJsonEscape(Detail))]()
			{
				if (UNwiroIKBridge* T = WeakThis.Get()) T->PushEvent(TEXT("download_progress"), Payload);
			});
			return;
		}

		float TotalMB = static_cast<float>(Response->GetContent().Num()) / (1024.0f * 1024.0f);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Payload = FString::Printf(TEXT("{\"adapter\":\"%s\",\"progress\":%.1f,\"total\":%.1f,\"status\":\"extracting\"}"), *Adapter, TotalMB, TotalMB)]()
		{
			if (UNwiroIKBridge* T = WeakThis.Get()) T->PushEvent(TEXT("download_progress"), Payload);
		});

		// Extract in background — miniz for .zip, system `tar` for .tar.gz.
		// codex-acp ships only .tar.gz on Unix; without this branch the zip
		// extractor silently failed and the binary never appeared, so the UI
		// kept bouncing the user back to "Download".
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, ZipPath, ExePath, SaveDir, Adapter, bIsTarGz]()
		{
			// ExtractError is hoisted to outer scope so the post-extract
			// FileExists check can include it in the final error payload.
			// Previously it was declared inside the else-branch and the
			// `if (!bExtracted) {}` block below was an empty stub — the
			// detail was captured and immediately discarded.
			bool bExtracted = false;
			FString ExtractError;
			if (bIsTarGz)
			{
#if PLATFORM_MAC || PLATFORM_LINUX
				const FString TarArgs = FString::Printf(TEXT("-xzf \"%s\" -C \"%s\""), *ZipPath, *SaveDir);
				int32 RetCode = -1;
				FString TarOut, TarErr;
				FPlatformProcess::ExecProcess(TEXT("/usr/bin/tar"), *TarArgs, &RetCode, &TarOut, &TarErr);
				bExtracted = (RetCode == 0);
				if (!bExtracted)
				{
					ExtractError = FString::Printf(TEXT("tar exit code=%d stderr=%s"), RetCode, *TarErr.Left(300));
					UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: tar extract failed: %s"), *ExtractError);
				}
#else
				ExtractError = TEXT("tar.gz extraction is not implemented on Windows — please report this URL to support");
				UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: %s"), *ExtractError);
#endif
			}
			else
			{
				bExtracted = FNwiroIKZipExtractor::ExtractZip(ZipPath, SaveDir, true, ExtractError);
			}
			IFileManager::Get().Delete(*ZipPath);

			if (!bExtracted)
			{
				UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: adapter '%s' extraction failed: %s"), *Adapter, *ExtractError);
			}

			if (!FPaths::FileExists(ExePath))
			{
				TArray<FString> Found;
				IFileManager::Get().FindFilesRecursive(Found, *SaveDir, *FPaths::GetCleanFilename(ExePath), true, false);
				if (Found.Num() > 0) IFileManager::Get().Move(*ExePath, *Found[0]);
			}

#if PLATFORM_MAC || PLATFORM_LINUX
			// On Unix the extracted binary lands without the executable bit,
			// so posix_spawn fails with EACCES when the adapter is launched.
			if (FPaths::FileExists(ExePath))
			{
				const FTCHARToUTF8 ExePathUtf8(*ExePath);
				if (chmod(ExePathUtf8.Get(), 0755) != 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: chmod +x failed on %s (errno=%d)"), *ExePath, errno);
				}
			}
#endif

			// Compose a useful detail for the GameThread emit. Three cases:
			//   (a) extraction failed outright — use ExtractError.
			//   (b) extraction "succeeded" but the expected binary isn't at
			//       the expected path (mismatched zip layout, or AV quarantine
			//       of the .exe right after extraction).
			//   (c) everything worked — no detail needed.
			//
			// Absolute paths are logged via UE_LOG above (audit trail) but
			// kept out of the user-facing detail string — those go through
			// toasts and would otherwise leak the Windows username (CWE-209)
			// to anything that screenshots the UI or forwards errorDetail to
			// future telemetry. The folder is described relatively.
			FString FailureDetail;
			if (!FPaths::FileExists(ExePath))
			{
				if (!ExtractError.IsEmpty())
				{
					FailureDetail = ExtractError;
				}
				else
				{
					FailureDetail = FString::Printf(
						TEXT("Extraction reported success but %s is not present in Saved/NwiroIntegrationKit — the binary may have been quarantined by antivirus immediately after extraction, or the archive layout was unexpected"),
						*FPaths::GetCleanFilename(ExePath));
				}
				UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: adapter '%s' final binary check failed at %s"), *Adapter, *ExePath);
			}

			AsyncTask(ENamedThreads::GameThread, [WeakThis, ExePath, Adapter, FailureDetail]()
			{
				UNwiroIKBridge* This = WeakThis.Get();
				if (!This) return;
				if (FPaths::FileExists(ExePath))
				{
					This->PushEvent(TEXT("download_progress"), FString::Printf(TEXT("{\"adapter\":\"%s\",\"status\":\"done\"}"), *Adapter));
				}
				else
				{
					This->PushEvent(TEXT("download_progress"), FString::Printf(
						TEXT("{\"adapter\":\"%s\",\"status\":\"error\",\"errorDetail\":\"%s\"}"),
						*Adapter, *NwiroJsonEscape(FailureDetail)));
				}
			});
		});
	});

	HttpRequest->ProcessRequest();
}

// ============================================================
// Process management
// ============================================================

void UNwiroIKBridge::EnsureProcess()
{
	// adapter-reliability-w1: start the timeout ticker the first time any
	// adapter activity happens. Idempotent — subsequent calls are no-ops.
	// Placed before the early-return so the ticker exists even when the
	// adapter is already running (covers the SetActiveChat → DoCreateSession
	// path that can fire RPCs without re-entering the spawn block).
	EnsureTimeoutTickerStarted();

	FAdapterProcess& AP = AdapterProcesses.FindOrAdd(CurrentAdapter);
	const bool bUsesOfficialCodexRuntime =
		NwiroCodex::UsesOfficialRuntime(CurrentAdapter);
	if (bUsesOfficialCodexRuntime
		&& (AP.bCodexShutdownUnproven
			|| AP.LastErrorCode == TEXT("runtime_shutdown_incomplete")))
	{
		// StopCodexProcessTreeAndWait deliberately retains the Job and any
		// managed-runtime lease after a failed drain. Surface that fail-closed
		// state even if the root still reports as running.
		return;
	}
	if (AP.Process.IsValid() && AP.Process->IsRunning()) return;
	if (bUsesOfficialCodexRuntime
		&& (AP.CodexProcessJobHandle || AP.ManagedRuntimeUseLockHandle))
	{
		// A prior tree could not be positively drained. Retaining its Job/lease
		// is intentional: fail closed instead of spawning another process or
		// allowing another editor to mutate files that may still be mapped.
		AP.LastStage = TEXT("launching_process");
		AP.LastErrorCode = TEXT("runtime_shutdown_incomplete");
		AP.LastError = TEXT("The previous Codex process tree has not terminated safely.");
		return;
	}
	if (bUsesOfficialCodexRuntime && bManagedCodexInstallInProgress)
	{
		// Promotion briefly moves the old managed directory aside before the
		// verified staging directory takes its place. Do not let a concurrent
		// chat fall through to a global runtime during that commit window.
		AP.LastStage = TEXT("launching_process");
		AP.LastErrorCode = TEXT("runtime_installing");
		AP.LastError = TEXT("The official Codex ACP runtime is being installed.");
		return;
	}

	FString AdapterBinary;
	FString AdapterArguments;
	FString AdapterRuntimeSource;
	AP.RuntimeSource.Empty();
	AP.ExpectedPackageVersion.Empty();
	if (bUsesOfficialCodexRuntime)
	{
		const NwiroCodex::FLaunchSpec CodexLaunch = NwiroCodex::ResolveLaunchSpec();
		AdapterBinary = CodexLaunch.Executable;
		AdapterArguments = CodexLaunch.Arguments;
		AdapterRuntimeSource = CodexLaunch.Source;
		AP.RuntimeSource = CodexLaunch.Source;
		AP.ExpectedPackageVersion = CodexLaunch.Version;
	}
	else
	{
		AdapterBinary = FindAdapterBinary();
	}
	if (AdapterBinary.IsEmpty())
	{
		AP.LastStage = TEXT("launching_process");
		AP.LastErrorCode = bUsesOfficialCodexRuntime
			? TEXT("managed_runtime_missing") : TEXT("adapter_exe_missing");
		AP.LastError = bUsesOfficialCodexRuntime
			? TEXT("The official Codex ACP runtime is not ready.") : TEXT("The adapter executable could not be located.");
		return;
	}
	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);

	FString CodexProcessConfig;
	if (bUsesOfficialCodexRuntime)
	{
		const FString ExistingCodexConfig =
			FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_CONFIG"));
		FString ConfigFailure;
		if (!FNwiroIKMCPServer::BuildCodexProcessConfig(
			ExistingCodexConfig,
			CodexProcessConfig,
			ConfigFailure))
		{
			AP.CodexMcpPort = 0;
			AP.LastStage = TEXT("launching_process");
			AP.LastErrorCode = TEXT("mcp_config_invalid");
			AP.LastError = ConfigFailure.IsEmpty()
				? TEXT("The process-local Codex MCP configuration could not be prepared.")
				: ConfigFailure;
			return;
		}
	}

	// Set environment variables from adapter registry (e.g. CLAUDE_CODE_EXECUTABLE)
	const FAdapterInfo* AdapterInfo = FindAdapter(CurrentAdapter);
	if (AdapterInfo && !AdapterInfo->EnvKey.IsEmpty())
	{
		// adapter-reliability-w6 env-override: honor an existing user-set env var
		// (e.g. user explicitly set CLAUDE_CODE_EXECUTABLE in their shell to point
		// at a custom build or fork). Without this check, the loop below would
		// overwrite it with whatever auto-discovery finds — silently defeating
		// the override. Only auto-discover if the existing value is empty or
		// points to a non-existent file (stale env var).
		const FString ExistingEnv = FPlatformMisc::GetEnvironmentVariable(*AdapterInfo->EnvKey);
		const bool bHonorExistingEnv = !ExistingEnv.IsEmpty() && NwiroPathExists(ExistingEnv);

		FString ResolvedCandidate;
		if (bHonorExistingEnv)
		{
			ResolvedCandidate = ExistingEnv;
		}
		else
		{
			for (const FString& Candidate : AdapterInfo->ExeCandidates)
			{
				if (NwiroPathExists(Candidate))
				{
					FPlatformMisc::SetEnvironmentVar(*AdapterInfo->EnvKey, *Candidate);
					ResolvedCandidate = Candidate;
					break;
				}
			}
		}

		if (!ResolvedCandidate.IsEmpty())
		{
			// adapter-reliability-w6: log the resolved CLI path so post-incident
			// triage can identify version-mismatch bugs (the kind that produce a
			// silent first_token_timeout because the underlying CLI crashed
			// internally — see FindClaudeInstallerPaths comment).
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: resolved CLI for adapter '%s' (env %s%s) → %s"),
				*CurrentAdapter, *AdapterInfo->EnvKey,
				bHonorExistingEnv ? TEXT(", user-set override") : TEXT(""),
				*ResolvedCandidate);

			// adapter-reliability-w7: log configured MCP servers (claude only).
			// Static-once flag inside ensures this only fires on first spawn per
			// editor session — see helper comment for full rationale.
			LogClaudeMcpServersConfig(CurrentAdapter);
#if PLATFORM_WINDOWS
			// Soft min-version check. Only fires when we resolved to the installer
			// path (where version is encoded in the path itself). For .local\bin
			// shims and user overrides pointing at non-installer paths we have no
			// way to know the version without spawning the binary, so we silently
			// proceed and rely on the bytes-counter diagnostic if the user later
			// hits a timeout. Warning-only, not a hard fail — version floors are
			// best-effort and rot quickly as Anthropic ships new releases.
			if (TOptional<TArray<int32>> Sem = ParseSemverFromInstallerPath(ResolvedCandidate); Sem.IsSet())
			{
				const TArray<int32>& V = Sem.GetValue();
				if (V[0] < 2 || (V[0] == 2 && V[1] < 1))
				{
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: resolved Claude Code is %d.%d.%d — older than 2.1.0. ")
						TEXT("This combination with claude-agent-acp 0.25.x is known to ")
						TEXT("hang on session/prompt (TypeError on effortLevel). ")
						TEXT("Update Claude Code via the Anthropic installer if you see first_token_timeout."),
						V[0], V[1], V[2]);
				}
				else
				{
					UE_LOG(LogTemp, Log,
						TEXT("Nwiro IK: resolved Claude Code version %d.%d.%d (>= 2.1.0, compatible)"),
						V[0], V[1], V[2]);
				}
			}
#endif
		}
	}

	// Suppress FInteractiveProcess stdout logging BEFORE creating the process.
	// The engine logs every line via LogInteractiveProcess — we already parse
	// stdout ourselves via OnOutput, so the engine's duplicate is pure spam.
	if (GEngine) GEngine->Exec(nullptr, TEXT("Log LogInteractiveProcess Error"));

	// FPaths::ProjectSavedDir() can hand back a deeply-relative path
	// (../../../../../<user>/.../Saved/...). FInteractiveProcess on Windows
	// resolves that fine, but macOS spawn paths choke and the relative segments
	// get interpreted from cwd, missing the actual binary entirely. Normalise
	// to absolute *and* collapse `..` segments before we hand it to spawn.
	FString AbsAdapterBinary = FPaths::ConvertRelativePathToFull(AdapterBinary);
	FPaths::CollapseRelativeDirectories(AbsAdapterBinary);

	if (bUsesOfficialCodexRuntime)
	{
		// Codex provenance is represented by verified source/version. Avoid
		// logging user-specific Node and project paths when they add no support
		// value.
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: launching verified Codex ACP runtime (source=%s, version=%s)"),
			*AdapterRuntimeSource, *AP.ExpectedPackageVersion);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("Nwiro IK: launching ACP adapter '%s' from %s (source=%s, cwd=%s)"),
			*CurrentAdapter, *AbsAdapterBinary,
			AdapterRuntimeSource.IsEmpty() ? TEXT("binary") : *AdapterRuntimeSource,
			*WorkingDir);
	}

	// adapter-reliability-w0: stage = launching_process.
	SetAdapterStage(CurrentAdapter, ActiveChatId, TEXT("launching_process"));
	if (bUsesOfficialCodexRuntime && AdapterRuntimeSource == TEXT("managed"))
	{
#if PLATFORM_WINDOWS
		if (!NwiroCodex::AcquireRuntimeFileLock(
			false, AP.ManagedRuntimeUseLockHandle))
		{
			AP.LastErrorCode = TEXT("managed_runtime_busy");
			AP.LastError = TEXT("The managed Codex runtime is being updated by another editor.");
			return;
		}
#endif
	}
	AP.ProcessGeneration = NextProcessGeneration++;
	AP.Process = MakeShareable(new FInteractiveProcess(AbsAdapterBinary, AdapterArguments, WorkingDir, true, true));

	FString AdapterId = CurrentAdapter;
	const uint64 ProcessGeneration = AP.ProcessGeneration;
	AP.Process->OnOutput().BindLambda([WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), AdapterId, ProcessGeneration](const FString& Output)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, AdapterId, ProcessGeneration, Output]()
		{
			UNwiroIKBridge* This = WeakThis.Get();
			if (!This) return;
			FAdapterProcess* RunningAdapter = This->AdapterProcesses.Find(AdapterId);
			if (!RunningAdapter || RunningAdapter->ProcessGeneration != ProcessGeneration) return;

			// adapter-reliability-w5: count every byte read from stdout BEFORE
			// the buffer append, so a parse-time crash later can't leave the
			// counter desynced from what we actually received.
			RunningAdapter->BytesFromStdout += Output.Len();
			RunningAdapter->StdoutBuffer += Output;
			int32 NewlineIdx = INDEX_NONE;
			while (RunningAdapter->StdoutBuffer.FindChar('\n', NewlineIdx))
			{
				FString Line = RunningAdapter->StdoutBuffer.Left(NewlineIdx);
				RunningAdapter->StdoutBuffer = RunningAdapter->StdoutBuffer.RightChop(NewlineIdx + 1);
				FString Trimmed = Line.TrimStartAndEnd();
				if (!Trimmed.IsEmpty()) This->ProcessLine(AdapterId, Trimmed);
			}

			// Bun-built adapters (claude-agent-acp v0.25.x) print stderr warnings
			// like "warn: CPU lacks AVX support…" WITHOUT a trailing newline before
			// the first JSON-RPC frame goes out on stdout. Anonymous pipes merge
			// stderr+stdout into the same buffer, so without this we keep adding
			// to the buffer forever and never find a newline.
			//
			// Drop any non-JSON prefix and then peel off complete top-level
			// `{…}` objects by tracking brace balance (string-aware so braces
			// inside JSON strings don't confuse us).
			FString& Buf = RunningAdapter->StdoutBuffer;
			while (true)
			{
				int32 ObjStart = INDEX_NONE;
				Buf.FindChar('{', ObjStart);
				if (ObjStart == INDEX_NONE) break;
				if (ObjStart > 0) Buf = Buf.RightChop(ObjStart);

				int32 Depth = 0;
				bool bInString = false;
				bool bEscape = false;
				int32 ObjEnd = INDEX_NONE;
				for (int32 i = 0; i < Buf.Len(); ++i)
				{
					const TCHAR Ch = Buf[i];
					if (bEscape) { bEscape = false; continue; }
					if (bInString)
					{
						if (Ch == TEXT('\\')) { bEscape = true; }
						else if (Ch == TEXT('"')) { bInString = false; }
						continue;
					}
					if (Ch == TEXT('"')) { bInString = true; continue; }
					if (Ch == TEXT('{')) { ++Depth; }
					else if (Ch == TEXT('}'))
					{
						--Depth;
						if (Depth == 0) { ObjEnd = i; break; }
					}
				}

				if (ObjEnd == INDEX_NONE) break; // incomplete — wait for more bytes
				FString Frame = Buf.Left(ObjEnd + 1);
				Buf = Buf.RightChop(ObjEnd + 1);
				This->ProcessLine(AdapterId, Frame);
			}
		});
	});

	// adapter-reliability-w1 codex-review-fix: capture TWeakPtr identity of the
	// process we're binding to. The OnCompleted callback can fire AFTER an
	// init-timeout-driven respawn replaces this entry; without an identity
	// check, the old callback would call AdapterProcesses.Remove(AdapterId)
	// and orphan the freshly-launched adapter (init/session ids still set,
	// but no Process to talk to). Pin + compare TSharedPtr — bail unless the
	// completion is for the still-current process.
	TWeakPtr<FInteractiveProcess> WeakProcess(AP.Process);
	AP.Process->OnCompleted().BindLambda([WeakThis = TWeakObjectPtr<UNwiroIKBridge>(this), AdapterId, WeakProcess, ProcessGeneration](int32 ReturnCode, bool bCanceled)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ReturnCode, bCanceled, AdapterId, WeakProcess, ProcessGeneration]()
		{
			UNwiroIKBridge* This = WeakThis.Get();
			if (!This) return;
			TSharedPtr<FInteractiveProcess> Pinned = WeakProcess.Pin();
			FAdapterProcess* CompletedAdapter = This->AdapterProcesses.Find(AdapterId);
			if (!CompletedAdapter || CompletedAdapter->Process != Pinned
				|| CompletedAdapter->ProcessGeneration != ProcessGeneration)
			{
				UE_LOG(LogTemp, Verbose,
					TEXT("Nwiro IK: stale OnCompleted ignored adapter=%s (process replaced after timeout)"),
					*AdapterId);
				return;
			}

			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: ACP adapter '%s' completed (code=%d, canceled=%s)"),
				*AdapterId, ReturnCode, bCanceled ? TEXT("true") : TEXT("false"));

			FString Tail = CompletedAdapter->StdoutBuffer.TrimStartAndEnd();
			if (!Tail.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: flushing unterminated ACP stdout tail: %s"), *Tail);
				This->ProcessLine(AdapterId, Tail);
			}

			// The process that minted this adapter's B1 claims is gone, and so is any
			// authorization they carried.
			This->RetireAcpPermissionState(AdapterId, FString());

			// adapter-reliability-w2 (codex carryover): classify process exit
			// via FailChatWithAdapterError so support telemetry shows
			// `adapter_exited` aggregate counts instead of bare-text "Agent
			// exited with code N" entries that aren't filterable. Snapshot
			// the chat ids first — FailChatWithAdapterError mutates
			// bProcessing on each session while we iterate.
			TArray<FString> ExitedChats;
			for (auto& Pair : This->ChatSessions)
			{
				if (!Pair.Value.bProcessing) continue;
				const FString SessionAdapterId = Pair.Value.AdapterId.IsEmpty()
					? AdapterId : Pair.Value.AdapterId;
				if (SessionAdapterId == AdapterId) ExitedChats.Add(Pair.Key);
			}
			for (const FString& ExitedChatId : ExitedChats)
			{
				FChatSession* Session = This->ChatSessions.Find(ExitedChatId);
				if (!Session) continue;
				const FString ExitedAdapterId = Session->AdapterId.IsEmpty() ? AdapterId : Session->AdapterId;
				const FString Detail = ReturnCode != 0
					? FString::Printf(TEXT("Adapter process exited unexpectedly (code=%d). Try sending again — Wave 1 preflight will respawn it."), ReturnCode)
					: TEXT("Adapter process exited cleanly. Try sending again — preflight will respawn it.");
				This->FailChatWithAdapterError(ExitedChatId, ExitedAdapterId,
					TEXT("launching_process"),
					TEXT("adapter_exited"),
					Detail);
			}

			// unknown-session-recovery: sweep per-chat ACP session ids minted
			// by the process that just died — including IDLE chats the
			// bProcessing loop above never touched. Retained ids would be
			// resent by SendMessage's session-exists fast path and the
			// respawned adapter can only answer "unknown session", wedging
			// every chat after a shim restart. Clear, do NOT blacklist — the
			// id space died with the process, and the respawn empties
			// RejectedAcpSessionIds anyway. LastModel is cleared alongside so
			// DoSendPrompt re-pushes the model config to the fresh session
			// (set_config_option is skipped when ModelToPush == LastModel).
			// A late OnCompleted from a replaced process generation cannot
			// reach here — the WeakProcess identity check above bails first.
			// Chats on OTHER adapters are left untouched.
			for (auto& Pair : This->ChatSessions)
			{
				FChatSession& Swept = Pair.Value;
				const FString SweptAdapterId = Swept.AdapterId.IsEmpty() ? AdapterId : Swept.AdapterId;
				if (SweptAdapterId != AdapterId) continue;
				if (!Swept.SessionId.IsEmpty()) This->SessionToChatId.Remove(Swept.SessionId);
				This->ClearChatRpcState(Swept);
				Swept.SessionId.Empty();
				Swept.PendingMessage.Empty();
				Swept.PendingRetryImageBlocks.Empty();
				Swept.bProcessing = false;
				Swept.LastModel.Empty();
				Swept.AvailableModelIds.Empty();
				Swept.AvailableModeIds.Empty();
				Swept.BoundProcessGeneration = 0;
				// unknown-session-recovery (image-aware): retained wire blocks
				// minted against the dead process belong to a dead turn — drop
				// them (the bLastSentHadImages flag deliberately survives so a
				// recovery that somehow still fires for that turn fails closed,
				// not reduced).
				Swept.LastSentImageBlocks.Empty();
				Swept.PendingRetryImageBlocks.Empty();
			}

			if (AdapterId == TEXT("codex")
				&& !This->StopCodexProcessTreeAndWait(
					*CompletedAdapter, TEXT("process_completed")))
			{
				CompletedAdapter->LastStage = TEXT("launching_process");
				CompletedAdapter->LastErrorCode = TEXT("runtime_shutdown_incomplete");
				CompletedAdapter->LastError =
					TEXT("The completed Codex root left a child process that could not be terminated safely.");
				return;
			}
			This->AdapterProcesses.Remove(AdapterId);
		});
	});

	// Local-LLM apiKey delivery -- hybrid (d)-hard channel. Set immediately
	// before child spawn so the FInteractiveProcess inherits it; cleared
	// immediately after to prevent leakage to subsequent unrelated child
	// processes. Per-adapter env-var name avoids races if claude/codex/localllm
	// spawn concurrently from a future multi-adapter feature.
	const FString LocalLlmEnvKey = FString::Printf(TEXT("NWIRO_LOCAL_LLM_API_KEY_%s"), *CurrentAdapter);
	const bool bSetLocalLlmKey = (CurrentAdapter == TEXT("localllm")) && !LocalLlmApiKey.IsEmpty();
	if (bSetLocalLlmKey)
	{
		FPlatformMisc::SetEnvironmentVar(*LocalLlmEnvKey, *LocalLlmApiKey);
	}

	// Warmup load-request timeout (non-secret) — pass the user-configured value to
	// the localllm shim via env, mirroring the apiKey channel above. The shim reads
	// NWIRO_LOCAL_LLM_WARMUP_TIMEOUT_SECS (default 300; 0 = unbounded), so a generous
	// value lets a slow cold-start model finish loading instead of failing with
	// errorKind=timeout. Not cleared after spawn (not sensitive) and harmless to
	// non-localllm children, which ignore the NWIRO_LOCAL_LLM_* keys.
	if ((CurrentAdapter == TEXT("localllm")) && !LocalLlmWarmupTimeoutSecs.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("NWIRO_LOCAL_LLM_WARMUP_TIMEOUT_SECS"), *LocalLlmWarmupTimeoutSecs);
	}

	// The official package owns a compatible @openai/codex dependency. Clear an
	// inherited CODEX_PATH for this child so a stale global CLI cannot replace
	// the pinned runtime. Start read-only; every prompt explicitly confirms
	// read-only or agent through the config-option ACK barrier before release. The
	// editor environment is restored immediately after CreateProcess returns.
	const bool bOfficialCodexChild = bUsesOfficialCodexRuntime;
	const FString PreviousCodexPath = bOfficialCodexChild
		? FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_PATH")) : TEXT("");
	const FString PreviousInitialAgentMode = bOfficialCodexChild
		? FPlatformMisc::GetEnvironmentVariable(TEXT("INITIAL_AGENT_MODE")) : TEXT("");
	const FString PreviousCodexConfig = bOfficialCodexChild
		? FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_CONFIG")) : TEXT("");
	if (bOfficialCodexChild)
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_PATH"), TEXT(""));
		FPlatformMisc::SetEnvironmentVar(TEXT("INITIAL_AGENT_MODE"), TEXT("read-only"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_CONFIG"), *CodexProcessConfig);
	}

	const bool bLaunched = AP.Process->Launch();
	if (bOfficialCodexChild)
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_PATH"), *PreviousCodexPath);
		FPlatformMisc::SetEnvironmentVar(TEXT("INITIAL_AGENT_MODE"), *PreviousInitialAgentMode);
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_CONFIG"), *PreviousCodexConfig);
	}

	if (!bLaunched)
	{
		if (bSetLocalLlmKey)
		{
			// Clear even on failed launch -- no child was created to inherit it.
			FPlatformMisc::SetEnvironmentVar(*LocalLlmEnvKey, TEXT(""));
		}
		if (bUsesOfficialCodexRuntime)
		{
			UE_LOG(LogTemp, Error,
				TEXT("Nwiro IK: failed to launch verified Codex ACP runtime (source=%s, version=%s)"),
				*AdapterRuntimeSource, *AP.ExpectedPackageVersion);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Nwiro IK: failed to launch ACP adapter '%s' from %s"),
				*CurrentAdapter, *AdapterBinary);
		}
		EnqueueResponse(TEXT("error"), bUsesOfficialCodexRuntime
			? TEXT("Failed to launch the managed official Codex ACP runtime.")
			: TEXT("Failed to launch ACP adapter. Make sure the adapter binary is available."));
		if (bOfficialCodexChild)
		{
#if PLATFORM_WINDOWS
			FProcHandle RootHandle = AP.Process->GetProcessHandle();
			if (RootHandle.IsValid())
			{
				// UE5.7 Launch() can return false after CreateProc succeeds if
				// its runnable thread cannot be created. Attach the already-live
				// root to a Job, recursively terminate any child created before
				// assignment, then require an empty Job before releasing the
				// managed-runtime lease.
				void* FailedLaunchJob = nullptr;
				if (NwiroCodex::AttachCodexProcessKillJob(
					RootHandle,
					FailedLaunchJob))
				{
					AP.CodexProcessJobHandle = FailedLaunchJob;
					FPlatformProcess::TerminateProc(RootHandle, true);
					StopCodexProcessTreeAndWait(
						AP,
						TEXT("launch_failed_after_createproc"));
					return;
				}

				// Job assignment itself failed. Kill and wait for the known
				// root/tree, but retain the runtime guard and latch the bridge
				// closed because descendant emptiness cannot be proven.
				AP.bCodexShutdownUnproven = true;
				FPlatformProcess::TerminateProc(RootHandle, true);
				FPlatformProcess::WaitForProc(RootHandle);
				AP.Process.Reset();
				AP.LastStage = TEXT("launching_process");
				AP.LastErrorCode = TEXT("runtime_shutdown_incomplete");
				AP.LastError =
					TEXT("The failed Codex launch could not be attached to a protected Windows process job.");
				return;
			}
#endif
			// CreateProc itself failed, so there is no process tree to drain.
			AP.Process.Reset();
			ReleaseCodexRuntimeGuards(AP);
			return;
		}
		AP.Process.Reset();
		return;
	}

#if PLATFORM_WINDOWS
	if (bOfficialCodexChild && !NwiroCodex::AttachCodexProcessKillJob(
		AP.Process->GetProcessHandle(), AP.CodexProcessJobHandle))
	{
		// Even before the first ACP request, a bootstrap process may create a
		// descendant. Root termination therefore cannot prove the tree empty.
		// Retain the managed lease and permanently latch this editor closed.
		FProcHandle RootHandle = AP.Process->GetProcessHandle();
		AP.bCodexShutdownUnproven = true;
		FPlatformProcess::TerminateProc(RootHandle, true);
		FPlatformProcess::WaitForProc(RootHandle);
		AP.Process->Cancel(true);
		AP.Process.Reset();
		ReleaseCodexRuntimeGuards(AP);
		AP.LastStage = TEXT("launching_process");
		AP.LastErrorCode = TEXT("runtime_shutdown_incomplete");
		AP.LastError =
			TEXT("Codex could not be attached to a protected Windows process job.");
		UE_LOG(LogTemp, Error,
			TEXT("Nwiro IK: rejected Codex launch because Windows Job assignment failed"));
		return;
	}
#endif

	if (bOfficialCodexChild)
	{
		AP.CodexMcpPort = FNwiroIKMCPServer::GetPort();
	}
	else
	{
		AP.CodexMcpPort = 0;
	}

	if (bSetLocalLlmKey)
	{
		// Child has already inherited the env at spawn -- clear it from the
		// editor's environment so it doesn't leak to other tools or appear
		// in any debug `printenv` from a subsequent shell.
		FPlatformMisc::SetEnvironmentVar(*LocalLlmEnvKey, TEXT(""));
	}

	// Start ACP handshake
	DoInitialize();
}

void UNwiroIKBridge::KillProcess()
{
	// Cards belonging to the adapter being killed die with it. Settled here, while the
	// process can still receive the rejection, instead of surviving until the TTL.
	RetireAcpPermissionState(CurrentAdapter, FString());

	FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
	const bool bHasCodexRuntimeGuards = AP
		&& CurrentAdapter == TEXT("codex")
		&& (AP->bCodexShutdownUnproven
			|| AP->CodexProcessJobHandle
			|| AP->ManagedRuntimeUseLockHandle);
	if (AP && (AP->Process.IsValid() || bHasCodexRuntimeGuards))
	{
		// Cancel all active sessions
		for (auto& Pair : ChatSessions)
		{
			const FString AdapterId = Pair.Value.AdapterId.IsEmpty() ? CurrentAdapter : Pair.Value.AdapterId;
			if (AdapterId == CurrentAdapter && Pair.Value.bProcessing && !Pair.Value.SessionId.IsEmpty())
			{
				TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
				Params->SetStringField(TEXT("sessionId"), Pair.Value.SessionId);
				SendRpcNotification(AdapterId, TEXT("session/cancel"), Params);
			}
		}
		if (CurrentAdapter == TEXT("codex"))
		{
			if (!StopCodexProcessTreeAndWait(*AP, TEXT("kill_process")))
			{
				UE_LOG(LogTemp, Error,
					TEXT("Nwiro IK: KillProcess left Codex state intact because the tree did not drain"));
				return;
			}
		}
		else
		{
			AP->Process->Cancel(true);
			AP->Process.Reset();
		}
		AdapterProcesses.Remove(CurrentAdapter);
	}
	ChatSessions.Empty();
	RpcToChatId.Empty();
	SessionToChatId.Empty();
}

// ============================================================
// Public API
// ============================================================

void UNwiroIKBridge::SendMessage(const FString& Message)
{
	if (Message.TrimStartAndEnd().IsEmpty() || ActiveChatId.IsEmpty()) return;

	FChatSession* Session = ChatSessions.Find(ActiveChatId);
	if (Session && Session->bProcessing) return; // Already processing this chat

	// adapter-reliability-w0: preflight before EnsureProcess. Catches missing
	// binary or missing CLI early so the user sees a classified error instantly
	// instead of waiting for the shim to spawn-then-hang on session/new
	// (Hypothesis #1 in tasks/todo.md). EnsureProcess itself is left as-is —
	// StartAdapter (the other caller) pre-warms without a chatId so a silent
	// no-op there is acceptable; SendMessage's preflight covers the user-facing
	// path.
	{
		const FString AdapterBinary = FindAdapterBinary();
		FString PreflightCode;
		FString PreflightMessage;
		if (!CheckAdapterBinaries(CurrentAdapter, AdapterBinary, PreflightCode, PreflightMessage))
		{
			FailChatWithAdapterError(ActiveChatId, CurrentAdapter, TEXT("launching_process"),
				PreflightCode, PreflightMessage);
			return;
		}
	}

	EnsureProcess();
	FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
	if (!AP || !AP->Process.IsValid() || !AP->Process->IsRunning())
	{
		// adapter-reliability-w0: classified replacement for the old bare
		// "Agent process not running." text. Preflight already ruled out the
		// known causes, so this code path now only fires on novel launch
		// failures (permissions, antivirus quarantine, etc.) — all of which
		// share the `adapter_launch_failed` bucket for now.
		const FString ErrorCode = AP && !AP->LastErrorCode.IsEmpty()
			? AP->LastErrorCode : TEXT("adapter_launch_failed");
		const FString ErrorDetail = AP && !AP->LastError.IsEmpty()
			? AP->LastError
			: TEXT("Agent process failed to start. Check the Unreal Output Log for details (often: permission denied, antivirus quarantine, or the binary is corrupt — try Reinstall Adapter).");
		FailChatWithAdapterError(
			ActiveChatId,
			CurrentAdapter,
			TEXT("launching_process"),
			ErrorCode,
			ErrorDetail);
		return;
	}

	// Get or create session for this chat
	FChatSession& S = ChatSessions.FindOrAdd(ActiveChatId);
	S.ChatId = ActiveChatId;
	if (S.AdapterId.IsEmpty())
	{
		S.AdapterId = CurrentAdapter;
	}
	else if (S.AdapterId != CurrentAdapter)
	{
		// See the twin site above: claims from the outgoing adapter are dead evidence.
		if (!S.SessionId.IsEmpty()) RetireAcpPermissionState(FString(), S.SessionId);
		if (!S.SessionId.IsEmpty()) SessionToChatId.Remove(S.SessionId);
		ClearCodexModelBarrier(S);
		S.AdapterId = CurrentAdapter;
		S.SessionId.Empty();
		S.PendingMessage.Empty();
		S.PendingRetryImageBlocks.Empty();
		S.SessionRpcId = 0;
		S.PromptRpcId = 0;
		S.LastModel.Empty();
		S.AvailableModelIds.Empty();
		S.AvailableModeIds.Empty();
		S.BoundProcessGeneration = 0;
		S.SeenToolCallIds.Empty();
		// adapter-reliability-w6: resume/replay state is per-adapter — wipe the
		// prior session id (meaningless to the new adapter) alongside the rest,
		// so it can't trigger a spurious session/load after an adapter switch.
		if (!S.PriorSessionId.IsEmpty()) SessionToChatId.Remove(S.PriorSessionId);
		S.PriorSessionId.Empty();
		S.AdapterVersionAtCreate.Empty();
		S.SessionLoadRpcId = 0;
		S.SessionLoadStartedAt = 0.0;
		S.bInReplay = false;
		S.bResumeAttempted = false;
		S.bResumeFailed = false;
	}

	// unknown-session-recovery: a genuine user send re-arms the one-shot
	// recreate+resend budget and drops any staged retry images left over from
	// a failed recovery — they belong to the previous turn, not this one.
	// Reset ONLY here — the recovery path itself must never reset the count,
	// or a persistent unknown-session answer would loop.
	S.UnknownSessionRetryCount = 0;
	S.PendingRetryImageBlocks.Empty();

	// ── The logical turn is admitted HERE, not in DoSendPrompt ───────────────────
	// This is the last point that is still synchronous with the user's send. When no
	// session exists yet the message is queued below and DoSendPrompt runs only after
	// session creation completes — an asynchronous gap in which another chat's send can
	// call setmode and overwrite the bridge-global CurrentMode. Capturing there meant a
	// chat could be judged under a mode it never selected.
	//
	// Captured once and reused by every downstream attempt (create, resume, retry,
	// unknown-session recovery), because they are all still the same logical turn.
	++S.TurnGeneration;
	S.ModeSnapshot = CurrentMode;

	if (!S.SessionId.IsEmpty())
	{
		// Session exists, send prompt directly
		DoSendPrompt(ActiveChatId, Message);
	}
	else
	{
		// No session yet — queue message and create session
		S.PendingMessage = Message;
		S.bProcessing = true;
		EnqueueResponse(TEXT("thinking"), TEXT(""), ActiveChatId);

		if (AP && AP->bInitialized)
			TryCreateOrLoadSession(ActiveChatId);
		// else: DoInitialize already called, session will be created after init
	}
}

// Compatibility entry point for renderers that predate CancelChatTurn. ActiveChatId is
// the LAST CHAT THAT SENT — SetActiveChat is called from sendViaBridge and nowhere else,
// so it does not follow a UI chat switch. Send in A, send in B, switch back to A, press
// Stop: this cancelled B. Worse, the renderer had already recorded its user-cancel token
// against A, so B's "Cancelled." echo arrived unpaired and was swallowed while A kept
// streaming with no way to stop it. New callers name the chat.
void UNwiroIKBridge::CancelMessage()
{
	CancelChatTurn(ActiveChatId);
}

int32 UNwiroIKBridge::GetChatTurnGeneration(const FString& ChatId) const
{
	const FChatSession* Session = ChatSessions.Find(ChatId);
	// int32 rather than uint64: UHT does not expose uint64 to the CEF bridge, and a turn
	// counter that only advances on send/cancel/close will not approach the limit.
	return Session ? static_cast<int32>(Session->TurnGeneration) : 0;
}

void UNwiroIKBridge::CancelChatTurnAt(const FString& ChatId, int32 ExpectedTurnGeneration)
{
	const FChatSession* Session = ChatSessions.Find(ChatId);
	const int32 Current = Session ? static_cast<int32>(Session->TurnGeneration) : 0;
	if (ExpectedTurnGeneration != 0 && Current != ExpectedTurnGeneration)
	{
		// The turn the caller decided to cancel is already over, and a newer one is running.
		// Killing that one would be cancelling a turn nobody asked to cancel.
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK CANCEL: ignored stale cancel chat=%s expected_turn=%d current_turn=%d"),
			*ChatId, ExpectedTurnGeneration, Current);
		// The echo is still sent. The renderer pairs cancel tokens to "Cancelled." echoes
		// FIFO, one per call, and records its token before calling — so a call that answered
		// with nothing would strand that token and shift the pairing for every later echo,
		// which is how a stale echo ends up idling a live turn. The echo answers the CALL,
		// not the turn; a watchdog-origin token suppresses its rendering either way.
		EnqueueResponse(TEXT("system"), TEXT("Cancelled."), ChatId);
		return;
	}
	CancelChatTurnInternal(ChatId, ExpectedTurnGeneration);
}

void UNwiroIKBridge::CancelChatTurn(const FString& ChatId)
{
	CancelChatTurnInternal(ChatId, 0);
}

void UNwiroIKBridge::CancelChatTurnInternal(const FString& ChatId, int32 ExpectedTurnGeneration)
{
	FChatSession* Session = ChatSessions.Find(ChatId);
	if (Session)
	{
		const FString AdapterId = Session->AdapterId.IsEmpty()
			? CurrentAdapter : Session->AdapterId;
		const FString CancelledSessionId = Session->SessionId;
		const bool bRetireSession =
			NwiroCodex::ShouldRetireCancelledSession(AdapterId);

		// Ask the peer to stop first, while the live id is still available.
		if (!CancelledSessionId.IsEmpty())
		{
			TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
			Params->SetStringField(TEXT("sessionId"), CancelledSessionId);
			SendRpcNotification(AdapterId, TEXT("session/cancel"), Params);

			if (bRetireSession)
			{
				// Codex session/update does not carry a prompt id, so a later
				// chunk cannot be distinguished from the user's next turn.
				// Reject and unmap the complete session as one transaction.
				if (FAdapterProcess* AP = AdapterProcesses.Find(AdapterId))
					AP->RejectedAcpSessionIds.Add(CancelledSessionId);
				SessionToChatId.Remove(CancelledSessionId);
			}
		}

		// THE turn ends here, whether or not the session is retired. Claude deliberately
		// KEEPS its session across a cancel (ShouldRetireCancelledSession is false for it),
		// so without this bump a permission card raised by the cancelled turn would still
		// validate against a live session during the next turn on that same session.
		++Session->TurnGeneration;
		// Settle, do not merely drop: cards from the cancelled turn are answered on the
		// wire while the process is still live, and the renderer is told they are dead.
		// Scoped to the turn when the caller named one: a deferred cancel must not retire a
		// card raised by a turn that started after its decision. `ExpectedTurnGeneration` is
		// the PRE-bump value the caller decided about, which is what the cards carry.
		PermissionCoordinator.RetireMatching(
			FString(), FString(), ChatId, EAcpPermissionOutcome::Retired,
			EAcpPermissionReason::ContextRetired,
			static_cast<uint64>(FMath::Max(0, ExpectedTurnGeneration)));

		ClearChatRpcState(*Session);
		if (Session->SessionLoadRpcId != 0) RpcToChatId.Remove(Session->SessionLoadRpcId);
		Session->SessionLoadRpcId = 0;
		Session->SessionLoadStartedAt = 0.0;
		Session->bInReplay = false;
		Session->PendingMessage.Empty();
		Session->PendingRetryImageBlocks.Empty();
		Session->LastSentImageBlocks.Empty();
		Session->bHasStreamed = false;
		Session->bProcessing = false;

		if (bRetireSession)
		{
			// Retiring the session retires its B1 claims with it.
			if (!Session->SessionId.IsEmpty()) RetireAcpPermissionState(FString(), Session->SessionId);
			Session->SessionId.Empty();
			if (!Session->PriorSessionId.IsEmpty()
				&& Session->PriorSessionId != CancelledSessionId)
			{
				SessionToChatId.Remove(Session->PriorSessionId);
			}
			Session->PriorSessionId.Empty();
			Session->bResumeAttempted = true;
			Session->bResumeFailed = true;
			Session->LastModel.Empty();
			Session->AvailableModelIds.Empty();
			Session->AvailableModeIds.Empty();
			Session->BoundProcessGeneration = 0;
			Session->SeenToolCallIds.Empty();
		}
	}
	EnqueueResponse(TEXT("system"), TEXT("Cancelled."), ChatId);
}

void UNwiroIKBridge::CloseChat(const FString& ChatId)
{
	// Staged attachments belong to the chat, so they go with it. Left behind they are both
	// a leak (full image files) and a hazard if the id is ever reused.
	PendingImagesByChat.Remove(ChatId);

	FChatSession* Session = ChatSessions.Find(ChatId);
	if (Session) ClearCodexModelBarrier(*Session);

	// Settle BEFORE the session mapping and the chat record go away: the wire response
	// needs the still-live process, and the retirement rule needs the session to look up.
	// This used to be missed entirely — closing a chat left its cards pending forever,
	// with the agent still waiting on a request nobody would answer.
	if (Session)
	{
		++Session->TurnGeneration;
		// Session-scoped only when there IS a session. Passing an empty id here made both
		// filters wildcards, so closing a chat that never got a session retired every
		// OTHER chat's permission state too.
		if (!Session->SessionId.IsEmpty()) RetireAcpPermissionState(FString(), Session->SessionId);
	}
	// Chat-scoped: covers the no-session case, and is the correct scope regardless.
	if (!ChatId.IsEmpty())
	{
		PermissionCoordinator.RetireMatching(
			FString(), FString(), ChatId, EAcpPermissionOutcome::Retired);
	}

	if (Session && !Session->SessionId.IsEmpty())
	{
		// Cancel if processing
		if (Session->bProcessing)
		{
			TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
			Params->SetStringField(TEXT("sessionId"), Session->SessionId);
			const FString AdapterId = Session->AdapterId.IsEmpty() ? CurrentAdapter : Session->AdapterId;
			SendRpcNotification(AdapterId, TEXT("session/cancel"), Params);
		}
		SessionToChatId.Remove(Session->SessionId);
	}
	ChatSessions.Remove(ChatId);
}

void UNwiroIKBridge::NewConversation()
{
	// Just clear active chat — don't kill the process (other chats may be active)
	ActiveChatId.Empty();
	FNwiroIKMCPServer::bSessionAllowed = false;
}

FString UNwiroIKBridge::SearchAssets(const FString& Query)
{
	// Support both raw query string and JSON {"query":"..."}
	FString SearchTerm = Query;
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Query);
	if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid() && JsonObj->HasField(TEXT("query")))
	{
		SearchTerm = JsonObj->GetStringField(TEXT("query"));
	}
	bool bShowAll = SearchTerm.IsEmpty() || SearchTerm == TEXT("*");

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// `false` second arg = include in-memory assets too. Without this, BPs
	// the agent just created via create_blueprint but didn't save yet would
	// be invisible — and the agent rarely saves between turns.
	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAllAssets(AllAssets, false);

	FString QueryLower = SearchTerm.ToLower();
	TArray<TSharedPtr<FJsonValue>> Results;
	const int32 MaxResults = 50;

	for (const FAssetData& Asset : AllAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.GetObjectPathString();

		// Only project content (/Game/)
		if (!Path.StartsWith(TEXT("/Game/"))) continue;

		if (bShowAll || Name.ToLower().Contains(QueryLower))
		{
			TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject);
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("path"), Path);
			Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
			Results.Add(MakeShareable(new FJsonValueObject(Item)));
		}
	}

	// Return the same {success, assets, count} shape callers expect from
	// find_assets / the inline fallback. Previously this only emitted a bare
	// JSON array, which made `.assets` undefined on the client and looked
	// identical to "no results".
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("assets"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString ResultStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResultStr);
	FJsonSerializer::Serialize(Root, Writer);
	return ResultStr;
}

FString UNwiroIKBridge::GetStaticMeshes()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Force rescan so deleted/moved assets are detected
	AssetRegistry.ScanPathsSynchronous({TEXT("/Game")}, /*bForceRescan=*/ true);

	// Filter for Static Meshes in /Game folder
	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	int32 RegistryHitCount = 0;
	int32 HardLoadCount = 0;

	TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> MeshArray;

	for (const FAssetData& Asset : AssetList)
	{
		TSharedRef<FJsonObject> MeshObj = MakeShareable(new FJsonObject());
		MeshObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		MeshObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		MeshObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		// Derive top-level folder category from package path
		FString PackagePath = Asset.PackagePath.ToString();
		PackagePath.RemoveFromStart(TEXT("/Game/"));
		int32 SlashIndex;
		if (PackagePath.FindChar('/', SlashIndex))
		{
			PackagePath = PackagePath.Left(SlashIndex);
		}
		MeshObj->SetStringField(TEXT("category"), PackagePath);

		float SizeX = 0, SizeY = 0, SizeZ = 0;
		bool bFoundInRegistry = false;

		// Fast path: read ApproxSize tag from asset registry
		FString ApproxSizeStr;
		if (Asset.GetTagValue(FName("ApproxSize"), ApproxSizeStr))
		{
			TArray<FString> Dims;
			if (ApproxSizeStr.ParseIntoArray(Dims, TEXT("x"), true) == 3)
			{
				SizeX = FCString::Atof(*Dims[0]);
				SizeY = FCString::Atof(*Dims[1]);
				SizeZ = FCString::Atof(*Dims[2]);
				bFoundInRegistry = true;
				RegistryHitCount++;
			}
		}

		// Slow path: load the mesh from disk to get bounds
		if (!bFoundInRegistry)
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
			if (!Mesh) Mesh = Cast<UStaticMesh>(Asset.ToSoftObjectPath().TryLoad());
			if (Mesh)
			{
				FBoxSphereBounds Bounds = Mesh->GetBounds();
				SizeX = Bounds.BoxExtent.X * 2.0f;
				SizeY = Bounds.BoxExtent.Y * 2.0f;
				SizeZ = Bounds.BoxExtent.Z * 2.0f;
				HardLoadCount++;
			}
		}

		MeshObj->SetNumberField(TEXT("sizeX"), SizeX);
		MeshObj->SetNumberField(TEXT("sizeY"), SizeY);
		MeshObj->SetNumberField(TEXT("sizeZ"), SizeZ);

		MeshArray.Add(MakeShareable(new FJsonValueObject(MeshObj)));
	}

	RootObject->SetArrayField(TEXT("meshes"), MeshArray);
	RootObject->SetNumberField(TEXT("count"), MeshArray.Num());

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject, Writer);

	return OutputString;
}

FString UNwiroIKBridge::GetStaticMeshesByPath(const FString& FolderPath)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FString SearchPath = FolderPath;
	if (!SearchPath.StartsWith(TEXT("/Game")))
	{
		SearchPath = TEXT("/Game/") + SearchPath;
	}

	AssetRegistry.ScanPathsSynchronous({SearchPath}, /*bForceRescan=*/ true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*SearchPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	int32 RegistryHitCount = 0;
	int32 HardLoadCount = 0;

	TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> MeshArray;

	for (const FAssetData& Asset : AssetList)
	{
		TSharedRef<FJsonObject> MeshObj = MakeShareable(new FJsonObject());
		MeshObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		MeshObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		MeshObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		float SizeX = 0, SizeY = 0, SizeZ = 0;
		bool bFoundInRegistry = false;

		FString ApproxSizeStr;
		if (Asset.GetTagValue(FName("ApproxSize"), ApproxSizeStr))
		{
			TArray<FString> Dims;
			if (ApproxSizeStr.ParseIntoArray(Dims, TEXT("x"), true) == 3)
			{
				SizeX = FCString::Atof(*Dims[0]);
				SizeY = FCString::Atof(*Dims[1]);
				SizeZ = FCString::Atof(*Dims[2]);
				bFoundInRegistry = true;
				RegistryHitCount++;
			}
		}

		if (!bFoundInRegistry)
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
			if (!Mesh) Mesh = Cast<UStaticMesh>(Asset.ToSoftObjectPath().TryLoad());
			if (Mesh)
			{
				FBoxSphereBounds Bounds = Mesh->GetBounds();
				SizeX = Bounds.BoxExtent.X * 2.0f;
				SizeY = Bounds.BoxExtent.Y * 2.0f;
				SizeZ = Bounds.BoxExtent.Z * 2.0f;
				HardLoadCount++;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Nwiro: Could not load mesh: %s"), *Asset.AssetName.ToString());
			}
		}

		MeshObj->SetNumberField(TEXT("sizeX"), SizeX);
		MeshObj->SetNumberField(TEXT("sizeY"), SizeY);
		MeshObj->SetNumberField(TEXT("sizeZ"), SizeZ);

		MeshArray.Add(MakeShareable(new FJsonValueObject(MeshObj)));
	}

	RootObject->SetArrayField(TEXT("meshes"), MeshArray);
	RootObject->SetNumberField(TEXT("count"), MeshArray.Num());
	RootObject->SetStringField(TEXT("searchPath"), SearchPath);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject, Writer);

	return OutputString;
}

bool UNwiroIKBridge::ExecutePython(const FString& Code)
{
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python Script Plugin is not available."));
		return false;
	}
	if (!PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python is not available in this UE installation."));
		return false;
	}

	bool bSuccess = PythonPlugin->ExecPythonCommand(*Code);
	if (bSuccess)
	{
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Nwiro: Python execution failed. Check Output Log."));
	}
	return bSuccess;
}

bool UNwiroIKBridge::SaveFile(const FString& Path, const FString& Content)
{
	FString FilePath = Path;
	FString FileContent = Content;
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [FilePath, FileContent]()
	{
		FFileHelper::SaveStringToFile(FileContent, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	});
	return true;
}

void UNwiroIKBridge::StartAdapter(const FString& Adapter)
{
	FString Prev = CurrentAdapter;
	CurrentAdapter = Adapter;
	EnsureProcess();
	CurrentAdapter = Prev;
}

// Declared in Nwiro.cpp
extern void SaveMCPConfig(int32 Port);

void UNwiroIKBridge::StartMCPServer(int32 Port)
{
	if (FNwiroIKMCPServer::IsRunning()) FNwiroIKMCPServer::Stop();
	FNwiroIKMCPServer::Start(Port);
	RestartCodexForMcpPortChangeIfNeeded();
	SaveMCPConfig(Port);
}

void UNwiroIKBridge::StopMCPServer()
{
	FNwiroIKMCPServer::Stop();
}

void UNwiroIKBridge::SetMCPPort(int32 Port)
{
	if (FNwiroIKMCPServer::IsRunning())
	{
		FNwiroIKMCPServer::Stop();
		FNwiroIKMCPServer::Start(Port);
		RestartCodexForMcpPortChangeIfNeeded();
	}
	SaveMCPConfig(Port);
}

FString UNwiroIKBridge::GetToolDefinitions()
{
	// Enabled tools only — this also feeds the local-LLM tools push, so it must
	// stay model-safe. The settings UI uses GetToolDefinitionsForSettings().
	return FNwiroIKMCPServer::GetEnabledToolDefinitionsJson();
}

FString UNwiroIKBridge::GetToolDefinitionsForSettings()
{
	return FNwiroIKMCPServer::GetToolDefinitionsForSettingsJson();
}

// Phase 2 — per-command shell approval (one-shot, 60s TTL). Bound to the exact
// normalized command hash. All accessors lock ShellApprovalCS (council #2: the
// approval-creation callback and terminal/create may run on different threads).
void UNwiroIKBridge::ApproveShellCommand(const FString& NormalizedHash)
{
	FScopeLock Lock(&ShellApprovalCS);
	ApprovedShellCommandExpiry.Add(NormalizedHash, FPlatformTime::Seconds() + 60.0);
}

// Peek: valid-and-unexpired without consuming. Used as the Phase-2 exec-site gate so
// a stub/denied path does not burn a real approval; Phase 4 calls Consume at spawn.
bool UNwiroIKBridge::HasValidShellApproval(const FString& NormalizedHash)
{
	FScopeLock Lock(&ShellApprovalCS);
	const double* Expiry = ApprovedShellCommandExpiry.Find(NormalizedHash);
	if (!Expiry) return false;
	if (*Expiry < FPlatformTime::Seconds()) { ApprovedShellCommandExpiry.Remove(NormalizedHash); return false; }
	return true;
}

bool UNwiroIKBridge::ConsumeShellApproval(const FString& NormalizedHash)
{
	FScopeLock Lock(&ShellApprovalCS);
	const double* Expiry = ApprovedShellCommandExpiry.Find(NormalizedHash);
	if (!Expiry) return false;
	const bool bValid = (*Expiry >= FPlatformTime::Seconds());
	ApprovedShellCommandExpiry.Remove(NormalizedHash); // one-shot: consume even if expired
	return bValid;
}

FString UNwiroIKBridge::GetDisabledToolNamespaces() const
{
	TArray<FString> Quoted;
	for (const FString& N : FNwiroIKToolRegistry::GetDisabledNamespaces())
	{
		Quoted.Add(FString::Printf(TEXT("\"%s\""), *N));
	}
	return TEXT("[") + FString::Join(Quoted, TEXT(",")) + TEXT("]");
}

void UNwiroIKBridge::SetDisabledToolNamespaces(const FString& JsonArray)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonArray);
	TArray<FString> Namespaces;
	if (FJsonSerializer::Deserialize(Reader, Items))
	{
		for (const TSharedPtr<FJsonValue>& V : Items)
		{
			if (V.IsValid()) { Namespaces.Add(V->AsString()); }
		}
	}
	FNwiroIKToolRegistry::SetDisabledNamespaces(Namespaces);
	FNwiroIKToolRegistry::Invalidate();
}

bool UNwiroIKBridge::GetToolbarButton() const
{
	return FNwiroIKPanel::IsToolbarButtonEnabled();
}

void UNwiroIKBridge::SetToolbarButton(bool bEnabled)
{
	FNwiroIKPanel::SetToolbarButtonEnabled(bEnabled);
}

// Declared in Nwiro.cpp
extern FString LoadNwiroSecret(const FString& Key);
extern void SaveNwiroSecret(const FString& Key, const FString& Value);

FString UNwiroIKBridge::GetSecret(const FString& Key)
{
	return LoadNwiroSecret(Key);
}

void UNwiroIKBridge::SetSecret(const FString& Key, const FString& Value)
{
	SaveNwiroSecret(Key, Value);
}

void UNwiroIKBridge::SetChatExtensions(const FString& ChatId, const FString& ExtensionsJson)
{
	ActiveChatExtensions.Empty();
	TSharedPtr<FJsonObject> Ext;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExtensionsJson);
	if (FJsonSerializer::Deserialize(R, Ext) && Ext.IsValid())
	{
		for (const auto& Pair : Ext->Values)
		{
			bool bEnabled = false;
			if (Pair.Value->TryGetBool(bEnabled))
			{
				ActiveChatExtensions.Add(FString(*Pair.Key), bEnabled);
			}
		}
	}
}

// SECURITY-CRITICAL -- never log Key, never serialize to JSON, never write
// to argv. Stored in private member; consumed only by the env-var injection
// branch in EnsureProcess. Cleared in BeginDestroy. See PLAN.md.
void UNwiroIKBridge::SetLocalLlmApiKey(const FString& Key)
{
	LocalLlmApiKey = Key;
	// Intentionally no UE_LOG. Even a "key set (length=N)" message is too
	// revealing for sufficiently short keys. Telemetry that the user
	// configured the adapter lives at the JS layer, not here.
}

// Pre-warm the local-LLM model. Sends `session/warmup` to the shim, which
// performs Ollama's empty-prompt + keep_alive trick to load weights into
// VRAM without generating tokens (or a /models reachability check for
// LM Studio / other backends). Async — the response arrives via
// HandleRpcResponse and is forwarded to JS as `EnqueueResponse("warmup",
// <jsonResult>, "")`. The JS settings store reads the result to update
// its WarmUpState (idle / loading / warm / failed) and surface a
// categorised error message when warmup fails.
//
// Why this is its own UFUNCTION instead of a generic `SendRpc` exposed to JS:
// keeping the surface narrow means we can validate that warmup is only sent
// to the localllm adapter (cloud adapters would 32601 it), and the C++ side
// can spawn the adapter process if it isn't running yet — saving the JS layer
// from racing process launch + RPC send.
void UNwiroIKBridge::WarmupLocalLlm()
{
	if (CurrentAdapter != TEXT("localllm"))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: WarmupLocalLlm called while CurrentAdapter='%s' — ignoring (warmup is localllm-only)"),
			*CurrentAdapter);
		return;
	}
	if (LocalLlmBaseUrl.IsEmpty() || LocalLlmModel.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Nwiro IK: WarmupLocalLlm called before SetAdapterContext populated baseUrl+model — ignoring"));
		// Surface a synthetic failure to JS so the UI doesn't get stuck on
		// "loading" forever. Frontend treats `errorKind=unknown` as a
		// generic actionable error.
		EnqueueResponse(TEXT("warmup"),
			TEXT("{\"status\":\"failed\",\"errorKind\":\"unknown\",\"message\":\"Local LLM endpoint not configured. Save the Endpoint URL and Model in Settings, then retry.\"}"),
			TEXT(""));
		return;
	}

	// Make sure the shim process is alive. EnsureProcess is zero-arg and
	// implicitly operates on `CurrentAdapter`, which the guard above
	// already pinned to "localllm". The function is idempotent: if the
	// process is running and initialised it no-ops; otherwise it spawns +
	// sends `initialize` and the warmup RPC queued below arrives once
	// init resolves. (Mirrors the SendMessage path's behaviour.)
	EnsureProcess();

	// Build params with the currently-stored config so the shim can warm
	// the right model even if its in-memory client is stale (the shim
	// reconfigures itself when these fields are present — see
	// `handle_warmup` in `acp/server.rs`).
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
	Params->SetStringField(TEXT("baseUrl"), LocalLlmBaseUrl);
	Params->SetStringField(TEXT("model"), LocalLlmModel);
	Params->SetStringField(TEXT("keepAlive"), TEXT("15m"));

	const int32 RpcId = SendRpc(TEXT("localllm"), TEXT("session/warmup"), Params);
	if (FAdapterProcess* AP = AdapterProcesses.Find(TEXT("localllm")))
	{
		AP->PendingWarmupRpcId = RpcId;
	}
	UE_LOG(LogTemp, Log, TEXT("Nwiro IK: WarmupLocalLlm dispatched rpc=%d model=%s"), RpcId, *LocalLlmModel);
}

void UNwiroIKBridge::SetAdapterContext(const FString& ContextJson)
{
	// SECURITY: NEVER log ContextJson at any UE_LOG level. Caller may pass
	// localLlm.{baseUrl, model} fields that are not secrets, but the JSON
	// envelope is also where future non-secret fields will be added. Rule of
	// thumb: if you need to debug this function, log specific extracted
	// fields by name, not the raw envelope. apiKey lives in LocalLlmApiKey
	// member (set via SetLocalLlmApiKey UFUNCTION), never here.
	TSharedPtr<FJsonObject> Ctx;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ContextJson);
	if (!FJsonSerializer::Deserialize(R, Ctx) || !Ctx.IsValid()) return;

	TSharedPtr<FJsonObject> Safety = Ctx->HasField(TEXT("safety"))
		? Ctx->GetObjectField(TEXT("safety")) : nullptr;
	if (Safety.IsValid())
	{
		if (Safety->HasField(TEXT("blockCommandExecution")))
		{
			const bool bRequestedBlockCommandExecution = Safety->GetBoolField(TEXT("blockCommandExecution"));
			const bool bPreviousBlockCommandExecution = bBlockCommandExecution;
			// User-controlled (council-gated). The app only sends false when the user
			// enabled shell AND completed onboarding. Execution is still gated by the
			// policy engine + per-command approval before anything runs (Phase 1-4);
			// terminal/create remains hard-blocked until those land.
			bBlockCommandExecution = bRequestedBlockCommandExecution;
			UE_LOG(LogTemp, Log, TEXT("Nwiro IK: SetAdapterContext blockCommandExecution=%s"),
				bBlockCommandExecution ? TEXT("true") : TEXT("false"));

			// ACP capabilities are fixed by initialize. If the shell gate changes
			// while the adapter is already running, relaunch it so the next
			// initialize advertises the correct terminal capability.
			if (bPreviousBlockCommandExecution != bBlockCommandExecution)
			{
				FAdapterProcess* AP = AdapterProcesses.Find(CurrentAdapter);
				if (AP && AP->Process.IsValid() && AP->Process->IsRunning())
				{
					UE_LOG(LogTemp, Log,
						TEXT("Nwiro IK: restarting adapter '%s' after shell execution setting changed"),
						*CurrentAdapter);

					// Every claim this adapter minted dies with the process being replaced.
					RetireAcpPermissionState(CurrentAdapter, FString());

					TArray<int32> RpcIdsToRemove;
					for (auto& Pair : ChatSessions)
					{
						FChatSession& Session = Pair.Value;
						const FString SessionAdapter = Session.AdapterId.IsEmpty() ? CurrentAdapter : Session.AdapterId;
						if (SessionAdapter != CurrentAdapter) continue;

						if (!Session.SessionId.IsEmpty()) SessionToChatId.Remove(Session.SessionId);
						if (Session.SessionRpcId != 0) RpcIdsToRemove.Add(Session.SessionRpcId);
						if (Session.PromptRpcId != 0) RpcIdsToRemove.Add(Session.PromptRpcId);
						if (Session.SessionLoadRpcId != 0) RpcIdsToRemove.Add(Session.SessionLoadRpcId);
						Session.SessionId.Empty();
						Session.SessionRpcId = 0;
						Session.PromptRpcId = 0;
						Session.PendingMessage.Empty();
						Session.PendingRetryImageBlocks.Empty();
						Session.bProcessing = false;
						Session.LastModel.Empty();
						Session.SeenToolCallIds.Empty();
						// adapter-reliability-w6: also reset resume/replay state so a stale
						// prior session id can't trigger a spurious session/load on restart.
						if (!Session.PriorSessionId.IsEmpty()) SessionToChatId.Remove(Session.PriorSessionId);
						Session.PriorSessionId.Empty();
						Session.AdapterVersionAtCreate.Empty();
						Session.SessionLoadRpcId = 0;
						Session.SessionLoadStartedAt = 0.0;
						Session.bInReplay = false;
						Session.bResumeAttempted = false;
						Session.bResumeFailed = false;
					}
					for (int32 RpcId : RpcIdsToRemove)
					{
						RpcToChatId.Remove(RpcId);
					}

					if (CurrentAdapter == TEXT("codex"))
					{
						if (!StopCodexProcessTreeAndWait(
							*AP, TEXT("adapter_context_changed")))
						{
							UE_LOG(LogTemp, Error,
								TEXT("Nwiro IK: Codex context restart refused because its process tree did not drain"));
							return;
						}
					}
					else
					{
						AP->Process->Cancel(true);
						AP->Process.Reset();
					}
					AdapterProcesses.Remove(CurrentAdapter);
				}
			}
		}
	}

	// Extract localllm endpoint config (non-secret). The no-log guard at the
	// top of this function applies to the FULL ContextJson string; logging
	// individually-extracted non-secret fields is permitted and useful for
	// diagnosing "why isn't my local LLM responding". apiKey is NOT here —
	// it lives in LocalLlmApiKey, set via the dedicated SetLocalLlmApiKey
	// UFUNCTION.
	TSharedPtr<FJsonObject> LocalLlmObj = Ctx->HasField(TEXT("localLlm"))
		? Ctx->GetObjectField(TEXT("localLlm")) : nullptr;
	if (LocalLlmObj.IsValid())
	{
		// Snapshot prior values BEFORE overwriting so we can detect a change.
		// The config-propagation fix below depends on this comparison: if the
		// already-running localllm shim has stale (baseUrl, model) baked into
		// its initialize handshake, no in-flight RPC can update it — the
		// only correct response is to terminate the process and let the
		// next chat send respawn it with fresh InitializeParams.
		const FString PrevBaseUrl = LocalLlmBaseUrl;
		const FString PrevModel = LocalLlmModel;
		LocalLlmObj->TryGetStringField(TEXT("baseUrl"), LocalLlmBaseUrl);
		LocalLlmObj->TryGetStringField(TEXT("model"), LocalLlmModel);
		// v0.2.x: optional user-set ACP-stage timeout overrides (number or string;
		// absent/0 = use env/default). first-token raises the slow-model first-token
		// ceiling (the wall that actually gates a cold local model); session-new +
		// initialize are exposed for parity with the NWIRO_*_TIMEOUT_SECONDS env
		// vars and rare edge cases. Consumed live by the Get*TimeoutSeconds()
		// resolvers in CheckAdapterTimeouts — no shim spawn/restart needed.
		FirstTokenTimeoutOverrideSecs = ReadOptionalTimeoutSecsField(LocalLlmObj, TEXT("firstTokenTimeoutSecs"));
		SessionNewTimeoutOverrideSecs = ReadOptionalTimeoutSecsField(LocalLlmObj, TEXT("sessionNewTimeoutSecs"));
		InitializeTimeoutOverrideSecs = ReadOptionalTimeoutSecsField(LocalLlmObj, TEXT("initializeTimeoutSecs"));
			// v0.2.x: optional user-set warmup timeout (accept number OR string).
			// Threaded to the shim via NWIRO_LOCAL_LLM_WARMUP_TIMEOUT_SECS on the
			// next spawn so a slow cold-start model finishes loading instead of
			// failing at the shim's 300s default.
			double WarmupTimeoutNum = 0.0;
			if (LocalLlmObj->TryGetNumberField(TEXT("warmupTimeoutSecs"), WarmupTimeoutNum))
			{
				LocalLlmWarmupTimeoutSecs = FString::FromInt(FMath::Max(0, (int32)WarmupTimeoutNum));
			}
			else
			{
				LocalLlmObj->TryGetStringField(TEXT("warmupTimeoutSecs"), LocalLlmWarmupTimeoutSecs);
			}
		UE_LOG(LogTemp, Log,
			TEXT("Nwiro IK: SetAdapterContext localLlm baseUrl=%s model=%s"),
			LocalLlmBaseUrl.IsEmpty() ? TEXT("(empty)") : *LocalLlmBaseUrl,
			LocalLlmModel.IsEmpty() ? TEXT("(empty)") : *LocalLlmModel);

		// Live-update propagation: if the localllm shim is already running
		// and the user just changed baseUrl or model, terminate the process
		// so EnsureProcess respawns it on the next send with the new config.
		// Without this, the shim is locked to the values it received in its
		// one-shot `initialize` and every subsequent prompt routes to the
		// stale endpoint. We only nuke the process if (a) the localllm
		// adapter is the active one (changes for an inactive adapter are
		// stored for the next activation), and (b) something actually
		// changed (avoid spurious restarts on duplicate Save clicks).
		const bool bConfigChanged = (PrevBaseUrl != LocalLlmBaseUrl) || (PrevModel != LocalLlmModel);
		if (bConfigChanged && CurrentAdapter == TEXT("localllm"))
		{
			if (FAdapterProcess* AP = AdapterProcesses.Find(TEXT("localllm")))
			{
				if (AP->Process.IsValid() && AP->Process->IsRunning())
				{
					UE_LOG(LogTemp, Log, TEXT("Nwiro IK: localllm config changed — terminating shim for restart"));
					AP->Process->Cancel(true);
					AP->Process.Reset();
					AP->bInitialized = false;
					AP->StdoutBuffer.Empty();
					AP->InitRpcId = 0;
					AP->InitRpcStartedAt = 0.0;
					// Strand any in-flight chats on this adapter so the next send
					// triggers a fresh EnsureProcess + initialize cycle. Skip
					// PendingMessage replay because the shim never accepted the
					// prompt under the old config anyway.
					for (auto& SessPair : ChatSessions)
					{
						if (SessPair.Value.AdapterId == TEXT("localllm"))
						{
							SessPair.Value.SessionId.Empty();
							SessPair.Value.LastModel.Empty();
							ClearChatRpcState(SessPair.Value);
							SessPair.Value.bProcessing = false;
						}
					}
				}
			}
		}
	}
}

void UNwiroIKBridge::SetPromptPreambles(const FString& ExecutePreamble, const FString& PlanPreamble)
{
	CurrentExecutePreamble = ExecutePreamble;
	CurrentPlanPreamble = PlanPreamble;
}

// adapter-reliability-w6: JS hands the bridge the prior session id to resume
// for a chat. Always-call contract — JS calls this on every chat activation,
// passing an empty SessionId to clear when there's nothing to resume. Uses
// FindOrAdd so the id can be seeded before the session struct otherwise
// materializes (the resume decision in TryCreateOrLoadSession reads it later).
void UNwiroIKBridge::SetResumeSessionId(const FString& ChatId, const FString& SessionId)
{
	if (ChatId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: SetResumeSessionId ignored — empty ChatId"));
		return;
	}
	FChatSession& Session = ChatSessions.FindOrAdd(ChatId);
	Session.ChatId = ChatId;
	Session.PriorSessionId = SessionId;
	UE_LOG(LogTemp, Verbose, TEXT("Nwiro IK: SetResumeSessionId chat=%s prior=%s"),
		*ChatId, SessionId.IsEmpty() ? TEXT("<cleared>") : *SessionId);
}

bool UNwiroIKBridge::IsChatExtensionEnabled(const FString& ExtName) const
{
	const bool* Found = ActiveChatExtensions.Find(ExtName);
	return Found && *Found;
}

// ============================================================
// MESHY → UE5 IMPORT
// ============================================================
// Downloads an FBX (or OBJ) from a remote URL into a temp file, then runs it
// through AssetTools' import pipeline with UE-specific options tuned for the
// kind of content meshy.ai produces: single static mesh, no LODs, embedded
// textures off (we'd need to handle them separately), auto-collision on.
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxFactory.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

void UNwiroIKBridge::ImportMesh(const FString& Url, const FString& FileName, const FString& DestFolder, bool bRevealInBrowser)
{
	if (Url.IsEmpty() || FileName.IsEmpty() || DestFolder.IsEmpty()) return;

	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("NwiroIntegrationKit"), TEXT("MeshyImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(Url);
	const FString CapturedDestFolder = DestFolder;
	const bool bCapturedReveal = bRevealInBrowser;
	Req->OnProcessRequestComplete().BindLambda([LocalPath, FileName, CapturedDestFolder, bCapturedReveal](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 400)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Nwiro] Meshy import: download failed for %s"), *FileName);
			if (UNwiroIKBridge::Instance)
			{
				UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
					FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Meshy import failed: download error\"}")));
			}
			return;
		}

		if (!FFileHelper::SaveArrayToFile(Resp->GetContent(), *LocalPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Nwiro] Meshy import: failed to write %s"), *LocalPath);
			return;
		}

		// Use FTSTicker instead of AsyncTask to run on the game thread's main
		// loop — outside the TaskGraph context. ImportAssetTasks internally
		// creates TaskGraph tasks via Interchange, and running inside an
		// AsyncTask causes a re-entrancy assertion crash.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[LocalPath, FileName, CapturedDestFolder, bCapturedReveal](float) -> bool
		{
			const FString BaseName = FPaths::GetBaseFilename(FileName);
			const FString Ext = FPaths::GetExtension(FileName).ToLower();
			FString DestPath = CapturedDestFolder;
			while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = LocalPath;
			Task->DestinationPath = DestPath;
			Task->bAutomated = true;
			Task->bSave = true;
			Task->bReplaceExisting = true;
			Task->DestinationName = BaseName;

			UFbxFactory* Factory = nullptr;
			if (Ext == TEXT("fbx"))
			{
				Factory = NewObject<UFbxFactory>(GetTransientPackage(), UFbxFactory::StaticClass(), NAME_None, RF_NoFlags);
				Factory->AddToRoot();
				UFbxImportUI* Options = NewObject<UFbxImportUI>(Factory);
				if (!Options->StaticMeshImportData)
				{
					Options->StaticMeshImportData = NewObject<UFbxStaticMeshImportData>(Options);
				}
				Options->MeshTypeToImport = FBXIT_StaticMesh;
				Options->OriginalImportType = FBXIT_StaticMesh;
				Options->bImportMesh = true;
				Options->bImportMaterials = true;
				Options->bImportTextures = true;
				Options->bImportAnimations = false;
				Options->bImportAsSkeletal = false;
				Options->bCreatePhysicsAsset = false;
				Options->bAutomatedImportShouldDetectType = false;
				Options->StaticMeshImportData->NormalImportMethod = FBXNIM_ImportNormalsAndTangents;
				Options->StaticMeshImportData->bAutoGenerateCollision = true;
				Options->StaticMeshImportData->bCombineMeshes = true;
				Options->StaticMeshImportData->bGenerateLightmapUVs = true;
				Factory->SetDetectImportTypeOnImport(false);
				Factory->ImportUI = Options;
				Task->Factory = Factory;
				Task->Options = Options;
			}
			Task->AddToRoot();

			FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
			TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
			AT.Get().ImportAssetTasks(Tasks);
			Task->RemoveFromRoot();
			if (Factory) Factory->RemoveFromRoot();

			const bool bAny = Task->ImportedObjectPaths.Num() > 0;
			if (bAny && bCapturedReveal)
			{
				FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
				TArray<FAssetData> Assets;
				FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				for (const FString& ObjPath : Task->ImportedObjectPaths)
				{
					FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
					if (AD.IsValid()) Assets.Add(AD);
				}
				if (Assets.Num() > 0)
				{
					CB.Get().SyncBrowserToAssets(Assets);
				}
				else
				{
					CB.Get().SyncBrowserToFolders({ DestPath });
				}
			}
			if (UNwiroIKBridge::Instance)
			{
				const FString Msg = bAny
					? FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"Meshy imported: %s\"}"), *BaseName)
					: FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"Meshy import failed for %s\"}"), *BaseName);
				UNwiroIKBridge::Instance->PushEvent(TEXT("toast"), Msg);
			}
			return false; // one-shot ticker — do not repeat
		}));
	});
	Req->ProcessRequest();
}

void UNwiroIKBridge::SaveAudioToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext)
{
	if (Base64Bytes.IsEmpty()) return;
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0) return;

	const FString SafeExt = Ext.IsEmpty() ? TEXT("mp3") : Ext.Replace(TEXT("."), TEXT(""));
	const FString Filter = FString::Printf(TEXT("Audio (*.%s)|*.%s"), *SafeExt, *SafeExt);

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
	TArray<FString> OutFiles;
	const bool bSaved = Desktop->SaveFileDialog(
		ParentWindow,
		TEXT("Save Audio"),
		FPaths::ProjectDir(),
		FString::Printf(TEXT("%s.%s"), *SuggestedName, *SafeExt),
		Filter,
		EFileDialogFlags::None,
		OutFiles
	);
	if (!bSaved || OutFiles.Num() == 0) return;

	FString Target = OutFiles[0];
	if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
	{
		Target += TEXT(".");
		Target += SafeExt;
	}
	if (FFileHelper::SaveArrayToFile(Bytes, *Target))
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"Audio saved to %s\"}"),
					*Target.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))));
		}
	}
}

void UNwiroIKBridge::ImportTextureFromUrl(const FString& Url, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser)
{
	if (Url.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty()) return;
	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;

	auto Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->OnProcessRequestComplete().BindLambda(
		[CapturedFolder, CapturedName, bCapturedReveal, SafeExt](FHttpRequestPtr, FHttpResponsePtr R, bool ok)
		{
			if (!ok || !R.IsValid() || R->GetResponseCode() < 200 || R->GetResponseCode() >= 300)
			{
				if (UNwiroIKBridge::Instance)
				{
					UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
						TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: download failed\"}"));
				}
				return;
			}
			const TArray<uint8>& Bytes = R->GetContent();
			if (UNwiroIKBridge::Instance && Bytes.Num() > 0)
			{
				const FString B64 = FBase64::Encode(Bytes);
				UNwiroIKBridge::Instance->ImportTextureBytes(B64, SafeExt, CapturedFolder, CapturedName, bCapturedReveal);
			}
		});
	Req->ProcessRequest();
}

void UNwiroIKBridge::SaveTextureFromUrl(const FString& Url, const FString& SuggestedName, const FString& Ext)
{
	if (Url.IsEmpty()) return;
	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString CapturedName = SuggestedName;

	auto Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->OnProcessRequestComplete().BindLambda(
		[CapturedName, SafeExt](FHttpRequestPtr, FHttpResponsePtr R, bool ok)
		{
			if (!ok || !R.IsValid() || R->GetResponseCode() < 200 || R->GetResponseCode() >= 300) return;
			const TArray<uint8>& Bytes = R->GetContent();
			const FString Filter = FString::Printf(TEXT("Image (*.%s)|*.%s"), *SafeExt, *SafeExt);
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
			if (!Desktop) return;
			const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
				? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
				: nullptr;
			TArray<FString> OutFiles;
			const bool bSaved = Desktop->SaveFileDialog(
				ParentWindow, TEXT("Save Texture"), FPaths::ProjectDir(),
				FString::Printf(TEXT("%s.%s"), *CapturedName, *SafeExt),
				Filter, EFileDialogFlags::None, OutFiles);
			if (!bSaved || OutFiles.Num() == 0) return;
			FString Target = OutFiles[0];
			if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
			{
				Target += TEXT(".");
				Target += SafeExt;
			}
			if (FFileHelper::SaveArrayToFile(Bytes, *Target) && UNwiroIKBridge::Instance)
			{
				UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
					FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"Texture saved to %s\"}"),
						*Target.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))));
			}
		});
	Req->ProcessRequest();
}

void UNwiroIKBridge::ImportTextureBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser)
{
	if (Base64Bytes.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty()) return;

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0)
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: invalid base64 payload\"}"));
		}
		return;
	}

	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("NwiroIntegrationKit"), TEXT("FalImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString FileName = FString::Printf(TEXT("%s_%lld.%s"), *DestName, FDateTime::UtcNow().ToUnixTimestamp(), *SafeExt);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);
	if (!FFileHelper::SaveArrayToFile(Bytes, *LocalPath))
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				TEXT("{\"level\":\"error\",\"text\":\"fal.ai import: failed to write temp file\"}"));
		}
		return;
	}

	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;
	const FString CapturedPath = LocalPath;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[CapturedPath, CapturedFolder, CapturedName, bCapturedReveal](float) -> bool
	{
		FString DestPath = CapturedFolder;
		while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = CapturedPath;
		Task->DestinationPath = DestPath;
		Task->DestinationName = CapturedName;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->AddToRoot();

		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
		AT.Get().ImportAssetTasks(Tasks);

		const bool bAny = Task->ImportedObjectPaths.Num() > 0;
		FString AssetObjectPath;
		if (bAny) AssetObjectPath = Task->ImportedObjectPaths[0];

		if (bAny && bCapturedReveal)
		{
			FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<FAssetData> Assets;
			FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			for (const FString& ObjPath : Task->ImportedObjectPaths)
			{
				FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
				if (AD.IsValid()) Assets.Add(AD);
			}
			if (Assets.Num() > 0) CB.Get().SyncBrowserToAssets(Assets);
			else                  CB.Get().SyncBrowserToFolders({ DestPath });
		}
		Task->RemoveFromRoot();

		if (UNwiroIKBridge::Instance)
		{
			const FString EscName  = CapturedName.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString EscAsset = AssetObjectPath.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString Data = FString::Printf(
				TEXT("{\"name\":\"%s\",\"assetPath\":\"%s\",\"imported\":%s}"),
				*EscName, *EscAsset, bAny ? TEXT("true") : TEXT("false"));
			UNwiroIKBridge::Instance->PushEvent(TEXT("fal_import"), Data);
		}
		return false;
	}));
}

void UNwiroIKBridge::SaveTextureToDisk(const FString& Base64Bytes, const FString& SuggestedName, const FString& Ext)
{
	if (Base64Bytes.IsEmpty()) return;
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0) return;

	const FString SafeExt = Ext.IsEmpty() ? TEXT("png") : Ext.Replace(TEXT("."), TEXT(""));
	const FString Filter = FString::Printf(TEXT("Image (*.%s)|*.%s"), *SafeExt, *SafeExt);

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return;
	const void* ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
	TArray<FString> OutFiles;
	const bool bSaved = Desktop->SaveFileDialog(
		ParentWindow,
		TEXT("Save Texture"),
		FPaths::ProjectDir(),
		FString::Printf(TEXT("%s.%s"), *SuggestedName, *SafeExt),
		Filter,
		EFileDialogFlags::None,
		OutFiles
	);
	if (!bSaved || OutFiles.Num() == 0) return;

	FString Target = OutFiles[0];
	if (!Target.EndsWith(FString::Printf(TEXT(".%s"), *SafeExt), ESearchCase::IgnoreCase))
	{
		Target += TEXT(".");
		Target += SafeExt;
	}
	if (FFileHelper::SaveArrayToFile(Bytes, *Target))
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"success\",\"text\":\"Texture saved to %s\"}"),
					*Target.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))));
		}
	}
}

void UNwiroIKBridge::ImportAudioBytes(const FString& Base64Bytes, const FString& Ext, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser)
{
	if (Base64Bytes.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty()) return;

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64Bytes, Bytes) || Bytes.Num() == 0)
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"ElevenLabs import: invalid base64 payload\"}")));
		}
		return;
	}

	const FString SafeExt = Ext.IsEmpty() ? TEXT("mp3") : Ext.Replace(TEXT("."), TEXT(""));
	const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("NwiroIntegrationKit"), TEXT("ElevenLabsImports"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString FileName = FString::Printf(TEXT("%s_%lld.%s"), *DestName, FDateTime::UtcNow().ToUnixTimestamp(), *SafeExt);
	const FString LocalPath = FPaths::Combine(TempDir, FileName);

	if (!FFileHelper::SaveArrayToFile(Bytes, *LocalPath))
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"ElevenLabs import: failed to write temp file\"}")));
		}
		return;
	}

	ImportAudio(LocalPath, DestFolder, DestName, bRevealInBrowser);
}

void UNwiroIKBridge::ImportAudio(const FString& LocalPath, const FString& DestFolder, const FString& DestName, bool bRevealInBrowser)
{
	if (LocalPath.IsEmpty() || DestFolder.IsEmpty() || DestName.IsEmpty()) return;
	if (!FPaths::FileExists(LocalPath))
	{
		if (UNwiroIKBridge::Instance)
		{
			UNwiroIKBridge::Instance->PushEvent(TEXT("toast"),
				FString::Printf(TEXT("{\"level\":\"error\",\"text\":\"ElevenLabs import: source file missing\"}")));
		}
		return;
	}

	const FString CapturedPath = LocalPath;
	const FString CapturedFolder = DestFolder;
	const FString CapturedName = DestName;
	const bool bCapturedReveal = bRevealInBrowser;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[CapturedPath, CapturedFolder, CapturedName, bCapturedReveal](float) -> bool
	{
		FString DestPath = CapturedFolder;
		while (DestPath.EndsWith(TEXT("/"))) DestPath.RemoveAt(DestPath.Len() - 1);

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = CapturedPath;
		Task->DestinationPath = DestPath;
		Task->DestinationName = CapturedName;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->AddToRoot();

		FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<UAssetImportTask*> Tasks; Tasks.Add(Task);
		AT.Get().ImportAssetTasks(Tasks);

		const bool bAny = Task->ImportedObjectPaths.Num() > 0;
		FString AssetObjectPath;
		if (bAny) AssetObjectPath = Task->ImportedObjectPaths[0];

		if (bAny && bCapturedReveal)
		{
			FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<FAssetData> Assets;
			FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			for (const FString& ObjPath : Task->ImportedObjectPaths)
			{
				FAssetData AD = AR.Get().GetAssetByObjectPath(FSoftObjectPath(ObjPath));
				if (AD.IsValid()) Assets.Add(AD);
			}
			if (Assets.Num() > 0) CB.Get().SyncBrowserToAssets(Assets);
			else                  CB.Get().SyncBrowserToFolders({ DestPath });
		}
		Task->RemoveFromRoot();

		if (UNwiroIKBridge::Instance)
		{
			const FString EscName  = CapturedName.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString EscAsset = AssetObjectPath.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			const FString Data = FString::Printf(
				TEXT("{\"name\":\"%s\",\"assetPath\":\"%s\",\"imported\":%s}"),
				*EscName, *EscAsset, bAny ? TEXT("true") : TEXT("false"));
			UNwiroIKBridge::Instance->PushEvent(TEXT("elevenlabs_import"), Data);
		}
		return false;
	}));
}

// ============================================================
// CONTENT PIPELINE BINDINGS
// ============================================================
#include "ContentPipeline/NwiroIKContentPipelineService.h"
#include "ContentPipeline/NwiroIKContentPipelineTypes.h"

FString UNwiroIKBridge::StartContentDownload(const FString& RequestJson)
{
	FNwiroIKContentRequest Req;
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}

	Json->TryGetStringField(TEXT("contentId"), Req.ContentId);
	Json->TryGetStringField(TEXT("hash"), Req.Hash);
	Json->TryGetStringField(TEXT("url"), Req.Url);
	Json->TryGetStringField(TEXT("sha256"), Req.ExpectedSHA256);
	Json->TryGetStringField(TEXT("md5"), Req.ExpectedMD5);
	Json->TryGetStringField(TEXT("importPath"), Req.RelativeImportPath);
	double SizeBytes = 0;
	if (Json->TryGetNumberField(TEXT("zipSize"), SizeBytes)) Req.ExpectedZipSizeBytes = (int64)SizeBytes;
	if (Json->TryGetNumberField(TEXT("unzipSize"), SizeBytes)) Req.EstimatedUnzippedSizeBytes = (int64)SizeBytes;

	FString Error;
	const bool bOk = FNwiroIKContentPipelineService::Get().StartDownloadAndImport(Req, Error);
	TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), bOk);
	Out->SetStringField(TEXT("hash"), Req.Hash);
	if (!bOk) Out->SetStringField(TEXT("error"), Error);
	return JsonToString(Out);
}

FString UNwiroIKBridge::GetContentStatus(const FString& Hash)
{
	return FNwiroIKContentPipelineService::Get().GetOperationStatusJson(TEXT(""), Hash);
}

bool UNwiroIKBridge::CancelContentDownload(const FString& Hash)
{
	FString Error;
	return FNwiroIKContentPipelineService::Get().CancelOperation(TEXT(""), Hash, Error);
}

FString UNwiroIKBridge::ListCachedContents()
{
	return FNwiroIKContentPipelineService::Get().GetCachedContentsJson();
}

// ============================================================
// adapter-reliability-w6: C++-owned session persistence
// Saved/NwiroIntegrationKit/sessions.json (schema v2)
// ============================================================
//
// Per-(chatId, adapter) JSON blobs the frontend uses to resume an ACP session
// after an editor restart. Modelled on LoadConfig/SaveConfig in
// NwiroIntegrationKit.cpp for the path + read/deserialize idioms, but the write
// path is hardened beyond that helper per the Wave 6 plan:
//   - atomic temp+rename (SaferReplaceWriteString pattern from NwiroIKMCPServer.cpp)
//   - corruption quarantine: a non-empty file that fails to parse is renamed to
//     sessions.corrupt.<unixts>.json so the next load starts clean without
//     destroying forensics
//   - debounced flush: the store is cached in memory and only written through
//     when the debounce window elapses, so a chat that saves on every turn
//     doesn't hammer the disk. A one-time OnEnginePreExit flush guarantees the
//     trailing write survives shutdown.
//
// Schema v2 shape:
//   { "schema": 2, "sessions": { "<chatId>": { "<adapterId>": "<blob>" } } }

namespace
{
	const int32 NwiroSessionsSchemaVersion = 2;

	// Debounce window for disk flushes. Env-tunable for tests; defaults to 1s.
	double GetSessionsFlushDebounceSeconds()
	{
		const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("NWIRO_SESSIONS_FLUSH_DEBOUNCE_SECONDS"));
		if (!Env.IsEmpty())
		{
			const double Parsed = FCString::Atod(*Env);
			if (Parsed >= 0.0) return Parsed;
		}
		return 1.0;
	}

	FString GetSessionsPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroIntegrationKit"), TEXT("sessions.json"));
	}

	// In-memory source of truth + debounce bookkeeping. Same-thread access only
	// (GetChatSession/SaveChatSession are UFUNCTIONs invoked from the game-thread
	// JS bridge), so no lock is needed — mirrors the existing ChatSessions map.
	TSharedPtr<FJsonObject> GSessionsStore;
	bool GSessionsLoaded = false;
	bool GSessionsDirty = false;
	double GSessionsLastFlush = 0.0;

	// Quarantine a corrupt store so the next load starts clean. Best-effort —
	// uses a timestamped name (no bReplace) so two corruptions in the same run
	// don't clobber each other. Matches the FDateTime::UtcNow().ToUnixTimestamp()
	// filename idiom used elsewhere in this file (ImportAudioBytes etc.).
	void QuarantineCorruptSessions(const FString& Path)
	{
		const FString CorruptPath = FPaths::Combine(
			FPaths::GetPath(Path),
			FString::Printf(TEXT("sessions.corrupt.%lld.json"), FDateTime::UtcNow().ToUnixTimestamp()));
		if (IFileManager::Get().Move(*CorruptPath, *Path, /*bReplace=*/false))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: sessions.json was corrupt — quarantined to %s"), *CorruptPath);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Nwiro IK: sessions.json was corrupt but could not be quarantined (move failed): %s"), *Path);
			// Last resort: delete so we don't keep re-parsing garbage every load.
			IFileManager::Get().Delete(*Path);
		}
	}

	// Read sessions.json into GSessionsStore. On a non-empty file that fails to
	// parse (or carries an unexpected schema) we quarantine + start fresh. A
	// missing file is the normal first-run case → fresh empty store.
	void EnsureSessionsLoaded()
	{
		if (GSessionsLoaded) return;
		GSessionsLoaded = true;

		const FString Path = GetSessionsPath();
		FString Content;
		if (FFileHelper::LoadFileToString(Content, *Path))
		{
			if (!Content.IsEmpty())
			{
				TSharedPtr<FJsonObject> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
				if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
				{
					double SchemaNum = 0.0;
					Parsed->TryGetNumberField(TEXT("schema"), SchemaNum);
					if ((int32)SchemaNum == NwiroSessionsSchemaVersion
						&& Parsed->HasTypedField<EJson::Object>(TEXT("sessions")))
					{
						GSessionsStore = Parsed;
						return;
					}
					// Wrong/missing schema — not corrupt, just unmigratable in v2.
					// Quarantine so forensics survive, then start clean.
					UE_LOG(LogTemp, Warning,
						TEXT("Nwiro IK: sessions.json schema=%d unsupported (expected %d) — quarantining + reinitializing"),
						(int32)SchemaNum, NwiroSessionsSchemaVersion);
					QuarantineCorruptSessions(Path);
				}
				else
				{
					// Non-empty but unparseable → corrupt.
					QuarantineCorruptSessions(Path);
				}
			}
		}

		// Fresh store (first run, empty file, corruption, or schema mismatch).
		GSessionsStore = MakeShareable(new FJsonObject);
		GSessionsStore->SetNumberField(TEXT("schema"), NwiroSessionsSchemaVersion);
		GSessionsStore->SetObjectField(TEXT("sessions"), MakeShareable(new FJsonObject));
	}

	// Atomic temp+rename write of GSessionsStore. Mirrors NwiroIKMCPServer.cpp's
	// SaferReplaceWriteString: write a sibling .tmp then IFileManager::Move with
	// bReplace=true (MoveFileEx/rename — closes the delete-then-move window).
	bool WriteSessionsAtomic()
	{
		if (!GSessionsStore.IsValid()) return false;

		const FString Path = GetSessionsPath();
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Path));

		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(GSessionsStore.ToSharedRef(), Writer);

		const FString TmpPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Output, *TmpPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: failed to write sessions temp file %s"), *TmpPath);
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *TmpPath, /*bReplace=*/true))
		{
			IFileManager::Get().Delete(*TmpPath);
			UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: failed to move sessions temp into place %s"), *Path);
			return false;
		}
		return true;
	}

	// Flush GSessionsStore to disk if dirty and the debounce window has elapsed
	// (or bForce). Returns whether a write happened. The trailing dirty write is
	// covered by a one-time OnEnginePreExit force-flush registered on first save.
	void FlushSessionsIfDue(bool bForce)
	{
		if (!GSessionsDirty) return;
		const double Now = FPlatformTime::Seconds();
		if (!bForce && (Now - GSessionsLastFlush) < GetSessionsFlushDebounceSeconds()) return;

		if (WriteSessionsAtomic())
		{
			GSessionsDirty = false;
			GSessionsLastFlush = Now;
		}
	}

	// Register a single shutdown flush so a debounced trailing write isn't lost
	// when the editor closes inside the debounce window. Function-local static
	// guard makes this idempotent without handle bookkeeping (process is exiting,
	// so we never need to unregister).
	void EnsureSessionsShutdownFlush()
	{
		static bool bRegistered = false;
		if (bRegistered) return;
		bRegistered = true;
		FCoreDelegates::OnEnginePreExit.AddLambda([]()
		{
			FlushSessionsIfDue(/*bForce=*/true);
		});
	}
}

FString UNwiroIKBridge::GetChatSession(const FString& ChatId, const FString& AdapterId)
{
	if (ChatId.IsEmpty() || AdapterId.IsEmpty()) return TEXT("");

	EnsureSessionsLoaded();
	const TSharedPtr<FJsonObject>* SessionsPtr = nullptr;
	if (!GSessionsStore->TryGetObjectField(TEXT("sessions"), SessionsPtr) || !SessionsPtr || !SessionsPtr->IsValid())
		return TEXT("");
	const TSharedPtr<FJsonObject> Sessions = *SessionsPtr;

	const TSharedPtr<FJsonObject>* ChatObjPtr = nullptr;
	if (!Sessions->TryGetObjectField(ChatId, ChatObjPtr) || !ChatObjPtr || !ChatObjPtr->IsValid())
		return TEXT("");

	FString Blob;
	(*ChatObjPtr)->TryGetStringField(AdapterId, Blob);
	return Blob;
}

bool UNwiroIKBridge::SaveChatSession(const FString& ChatId, const FString& AdapterId, const FString& Json)
{
	if (ChatId.IsEmpty() || AdapterId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Nwiro IK: SaveChatSession ignored — empty chatId/adapter"));
		return false;
	}

	EnsureSessionsLoaded();
	EnsureSessionsShutdownFlush();

	TSharedPtr<FJsonObject> Sessions = GSessionsStore->GetObjectField(TEXT("sessions"));
	if (!Sessions.IsValid())
	{
		Sessions = MakeShareable(new FJsonObject);
		GSessionsStore->SetObjectField(TEXT("sessions"), Sessions);
	}

	const TSharedPtr<FJsonObject>* ExistingChatPtr = nullptr;
	TSharedPtr<FJsonObject> ChatObj;
	if (Sessions->TryGetObjectField(ChatId, ExistingChatPtr) && ExistingChatPtr && ExistingChatPtr->IsValid())
	{
		ChatObj = *ExistingChatPtr;
	}
	else
	{
		ChatObj = MakeShareable(new FJsonObject);
		Sessions->SetObjectField(ChatId, ChatObj);
	}

	if (Json.IsEmpty())
	{
		// Empty blob clears the (chat,adapter) entry — mirrors the always-call
		// clear contract used by SetResumeSessionId.
		ChatObj->RemoveField(AdapterId);
	}
	else
	{
		ChatObj->SetStringField(AdapterId, Json);
	}

	GSessionsDirty = true;
	FlushSessionsIfDue(/*bForce=*/false);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
// Included HERE (bottom of this TU) rather than from a test-only file so the spec
// can reach the internal-linkage shell classifier directly and therefore test the
// shipping code path itself.
#include "Tests/NwiroIKShellClassifierSpec.inl"
#include "Tests/NwiroIKPermissionCoordinatorSpec.inl"
#include "Tests/NwiroIKToolOutputSpec.inl"
#include "Tests/NwiroIKCancelTargetSpec.inl"
#endif

