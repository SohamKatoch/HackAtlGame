// Included by NwiroIKBridge.cpp after namespace NwiroCodex so these tests can
// exercise the internal protocol helpers without exporting production APIs.

#include "Misc/AutomationTest.h"

namespace NwiroCodexSuccessorSpec
{
	static TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Result;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Result);
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexCatalogTest,
	"Nwiro.IntegrationKit.Codex.SuccessorCatalog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexCatalogTest::RunTest(const FString& Parameters)
{
	using namespace NwiroCodexSuccessorSpec;
	const TSharedPtr<FJsonObject> Result = ParseObject(TEXT(R"json(
	{
	  "models":{"availableModels":[{"modelId":"gpt-5.6-luna[high]"}]},
	  "configOptions":[{
	    "id":"model","type":"select","currentValue":"gpt-5.5",
	    "options":[
	      {"value":"gpt-5.5","name":"GPT-5.5"},
	      {"value":"gpt-5.6-sol","name":"GPT-5.6 Sol"},
	      {"value":"gpt-5.6-terra","name":"GPT-5.6 Terra"},
	      {"value":"gpt-5.6-luna","name":"GPT-5.6 Luna"}
	    ]
	  }]
	})json"));

	const NwiroCodex::FModelCatalog Catalog = NwiroCodex::ParseModelCatalog(Result);
	TestTrue(TEXT("model config option is present"), Catalog.bHasModelOption);
	TestEqual(TEXT("current model comes from configOptions"), Catalog.CurrentModel, FString(TEXT("gpt-5.5")));
	TestTrue(TEXT("Sol base id is advertised"), Catalog.ModelIds.Contains(TEXT("gpt-5.6-sol")));
	TestTrue(TEXT("Terra base id is advertised"), Catalog.ModelIds.Contains(TEXT("gpt-5.6-terra")));
	TestTrue(TEXT("Luna base id is advertised"), Catalog.ModelIds.Contains(TEXT("gpt-5.6-luna")));
	TestFalse(TEXT("legacy composite model list is ignored"), Catalog.ModelIds.Contains(TEXT("gpt-5.6-luna[high]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexExactAckTest,
	"Nwiro.IntegrationKit.Codex.ExactModelAck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexExactAckTest::RunTest(const FString& Parameters)
{
	using namespace NwiroCodexSuccessorSpec;
	NwiroCodex::FModelCatalog Catalog;
	const TSharedPtr<FJsonObject> Exact = ParseObject(TEXT(R"json(
	{"configOptions":[{"id":"model","currentValue":"gpt-5.6-luna","options":[{"value":"gpt-5.6-luna"}]}]}
	)json"));
	TestTrue(TEXT("exact currentValue plus membership releases"),
		NwiroCodex::IsExactModelAck(Exact, TEXT("gpt-5.6-luna"), Catalog));

	const TSharedPtr<FJsonObject> WrongCurrent = ParseObject(TEXT(R"json(
	{"configOptions":[{"id":"model","currentValue":"gpt-5.5","options":[{"value":"gpt-5.6-luna"}]}]}
	)json"));
	TestFalse(TEXT("catalog membership alone does not release"),
		NwiroCodex::IsExactModelAck(WrongCurrent, TEXT("gpt-5.6-luna"), Catalog));

	const TSharedPtr<FJsonObject> MissingMembership = ParseObject(TEXT(R"json(
	{"configOptions":[{"id":"model","currentValue":"gpt-5.6-luna","options":[{"value":"gpt-5.5"}]}]}
	)json"));
	TestFalse(TEXT("currentValue without membership does not release"),
		NwiroCodex::IsExactModelAck(MissingMembership, TEXT("gpt-5.6-luna"), Catalog));

	const TSharedPtr<FJsonObject> LegacyCatalog = ParseObject(TEXT(R"json(
	{"configOptions":[{"id":"model","currentValue":"gpt-5.5","options":[{"value":"gpt-5.4"},{"value":"gpt-5.5"}]}]}
	)json"));
	TestTrue(TEXT("legacy Zed exact GPT-5.5 acknowledgement still releases"),
		NwiroCodex::IsExactModelAck(LegacyCatalog, TEXT("gpt-5.5"), Catalog));
	TestFalse(TEXT("legacy Zed catalog cannot acknowledge absent GPT-5.6"),
		NwiroCodex::IsExactModelAck(LegacyCatalog, TEXT("gpt-5.6-sol"), Catalog));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexSessionContractTest,
	"Nwiro.IntegrationKit.Codex.SessionContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexSessionContractTest::RunTest(const FString& Parameters)
{
	using namespace NwiroCodexSuccessorSpec;
	const TSharedPtr<FJsonObject> Complete = ParseObject(TEXT(R"json(
	{
	  "configOptions":[
	    {
	      "id":"model","currentValue":"gpt-5.6-sol",
	      "options":[{"value":"gpt-5.5"},{"value":"gpt-5.6-sol"}]
	    },
	    {
	      "id":"mode","currentValue":"read-only",
	      "options":[{"value":"read-only"},{"value":"agent"},{"value":"agent-full-access"}]
	    }
	  ]
	}
	)json"));

	NwiroCodex::FModelCatalog ModelCatalog;
	NwiroCodex::FSelectConfigCatalog ModeCatalog;
	TestTrue(TEXT("new/load session contract accepts model and required safe modes"),
		NwiroCodex::TryParseRequiredSessionContract(
			Complete, 42, ModelCatalog, ModeCatalog));
	TestTrue(TEXT("validated model catalog is returned"),
		ModelCatalog.ModelIds.Contains(TEXT("gpt-5.6-sol")));
	TestTrue(TEXT("validated mode catalog is returned"),
		ModeCatalog.Values.Contains(TEXT("read-only"))
			&& ModeCatalog.Values.Contains(TEXT("agent")));
	TestFalse(TEXT("session contract is never bound without a live process generation"),
		NwiroCodex::TryParseRequiredSessionContract(
			Complete, 0, ModelCatalog, ModeCatalog));

	const TSharedPtr<FJsonObject> MissingAgent = ParseObject(TEXT(R"json(
	{
	  "configOptions":[
	    {"id":"model","currentValue":"gpt-5.6-sol","options":[{"value":"gpt-5.6-sol"}]},
	    {"id":"mode","currentValue":"read-only","options":[{"value":"read-only"}]}
	  ]
	}
	)json"));
	TestFalse(TEXT("session contract requires both read-only and agent modes"),
		NwiroCodex::TryParseRequiredSessionContract(
			MissingAgent, 42, ModelCatalog, ModeCatalog));

	const TSharedPtr<FJsonObject> LegacyModelOnly = ParseObject(TEXT(R"json(
	{
	  "configOptions":[{
	    "id":"model","currentValue":"gpt-5.5",
	    "options":[{"value":"gpt-5.4"},{"value":"gpt-5.5"}]
	  }]
	}
	)json"));
	TestTrue(TEXT("legacy Zed contract requires the live model catalog but not successor modes"),
		NwiroCodex::TryParseRequiredModelContract(
			LegacyModelOnly, 42, ModelCatalog));
	TestFalse(TEXT("legacy model contract still requires a live process generation"),
		NwiroCodex::TryParseRequiredModelContract(
			LegacyModelOnly, 0, ModelCatalog));
	TestFalse(TEXT("legacy catalog does not invent GPT-5.6 support"),
		ModelCatalog.ModelIds.Contains(TEXT("gpt-5.6-sol")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexPlatformRoutingTest,
	"Nwiro.IntegrationKit.Codex.PlatformRouting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexPlatformRoutingTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Windows Codex selects the official managed runtime path"),
		NwiroCodex::UsesOfficialRuntime(TEXT("codex"), true));
	TestFalse(TEXT("non-Windows Codex selects the generic legacy binary path"),
		NwiroCodex::UsesOfficialRuntime(TEXT("codex"), false));
	TestFalse(TEXT("other Windows adapters remain on generic discovery"),
		NwiroCodex::UsesOfficialRuntime(TEXT("claude"), true));
	TestFalse(TEXT("legacy Zed rejects GPT-5.6 before consulting its catalog"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("codex"), TEXT("gpt-5.6-sol"), false));
	TestFalse(TEXT("legacy Zed rejects the bare GPT-5.6 family id case-insensitively"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("codex"), TEXT("GPT-5.6"), false));
	TestTrue(TEXT("legacy Zed does not misclassify GPT-5.60 as GPT-5.6"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("codex"), TEXT("gpt-5.60-sol"), false));
	TestTrue(TEXT("legacy Zed continues to allow GPT-5.5"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("codex"), TEXT("gpt-5.5"), false));
	TestTrue(TEXT("Windows official runtime allows GPT-5.6"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("codex"), TEXT("gpt-5.6-luna"), true));
	TestTrue(TEXT("Codex-specific policy does not constrain other adapters"),
		NwiroCodex::IsModelAllowedForRuntime(
			TEXT("claude"), TEXT("gpt-5.6-sol"), false));

	// Badge-fallback matrix. Only LEGACY Zed codex-acp emits no ACP tool_call of its
	// own, so it is the only runtime whose badge the frontend must synthesise. The
	// naive predicate `!UsesOfficialRuntime(...)` passes the two codex rows below and
	// fails every other one — those non-codex rows are the whole point of this block.
	TestFalse(TEXT("Windows Codex uses the official runtime, needs no badge fallback"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("codex"), true));
	TestTrue(TEXT("non-Windows Codex runs legacy Zed and DOES need the badge fallback"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("codex"), false));
	TestFalse(TEXT("claude badges via ACP tool_call — no fallback on Windows"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("claude"), true));
	TestFalse(TEXT("claude badges via ACP tool_call — no fallback off Windows"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("claude"), false));
	TestFalse(TEXT("localllm badges via ACP tool_call — no fallback off Windows"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("localllm"), false));
	TestFalse(TEXT("nwiro adapter never needs the badge fallback"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("nwiro"), false));
	TestFalse(TEXT("empty adapter never needs the badge fallback"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT(""), false));
	TestFalse(TEXT("unknown adapter never needs the badge fallback"),
		NwiroCodex::NeedsMcpBadgeFallback(TEXT("someFutureAdapter"), false));

	// THE ADAPTER-TRANSITION REGRESSION. Send setup calls setadapter() several awaits
	// before setactivechat(), so the global adapter can already name an incoming chat
	// while events still route to the previous one. Policy must follow the RECEIVING
	// chat, never the global — otherwise a legacy-Codex chat loses its only badge the
	// moment a Claude chat is opened.
	TestTrue(TEXT("legacy-Codex chat keeps its fallback while the global says claude"),
		NwiroCodex::NeedsMcpBadgeFallbackForChat(
			/*ChatAdapterId*/ TEXT("codex"), /*GlobalAdapterId*/ TEXT("claude"), /*bIsWindows*/ false));
	TestFalse(TEXT("claude chat gets no fallback while the global still says codex"),
		NwiroCodex::NeedsMcpBadgeFallbackForChat(
			TEXT("claude"), TEXT("codex"), false));
	TestFalse(TEXT("Windows Codex chat gets no fallback regardless of the global"),
		NwiroCodex::NeedsMcpBadgeFallbackForChat(
			TEXT("codex"), TEXT("claude"), true));

	// A chat with no adapter recorded yet is the ONLY case that may consult the global.
	TestTrue(TEXT("chat without an adapter falls back to the global (legacy codex)"),
		NwiroCodex::NeedsMcpBadgeFallbackForChat(TEXT(""), TEXT("codex"), false));
	TestFalse(TEXT("chat without an adapter falls back to the global (claude)"),
		NwiroCodex::NeedsMcpBadgeFallbackForChat(TEXT(""), TEXT("claude"), false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexIdentityAndModeTest,
	"Nwiro.IntegrationKit.Codex.IdentityAndMode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexIdentityAndModeTest::RunTest(const FString& Parameters)
{
	using namespace NwiroCodexSuccessorSpec;
	TestTrue(TEXT("official package identity, exact live version, and protocol are accepted"),
		NwiroCodex::IsOfficialIdentity(
			TEXT("@agentclientprotocol/codex-acp"), TEXT("1.1.7"), TEXT("1.1.7"), 1));
	TestFalse(TEXT("archived Zed identity is rejected"),
		NwiroCodex::IsOfficialIdentity(TEXT("codex-acp"), TEXT("1.1.7"), TEXT("1.1.7"), 1));
	TestFalse(TEXT("wrong protocol is rejected"),
		NwiroCodex::IsOfficialIdentity(
			TEXT("@agentclientprotocol/codex-acp"), TEXT("1.1.7"), TEXT("1.1.7"), 0));
	TestFalse(TEXT("fractional protocol cannot truncate to protocol 1"),
		NwiroCodex::IsOfficialIdentity(
			TEXT("@agentclientprotocol/codex-acp"), TEXT("1.1.7"), TEXT("1.1.7"), 1.5));
	TestFalse(TEXT("live agent version must match package metadata exactly"),
		NwiroCodex::IsOfficialIdentity(
			TEXT("@agentclientprotocol/codex-acp"), TEXT("1.1.6"), TEXT("1.1.7"), 1));
	TestFalse(TEXT("missing expected package version fails closed"),
		NwiroCodex::IsOfficialIdentity(
			TEXT("@agentclientprotocol/codex-acp"), TEXT("1.1.7"), TEXT(""), 1));

	const TSharedPtr<FJsonObject> Result = ParseObject(TEXT(R"json(
	{
	  "modes":{"currentModeId":"agent","availableModes":[
	    {"id":"read-only"},{"id":"agent"},{"id":"agent-full-access"}
	  ]},
	  "configOptions":[{
	    "id":"mode","type":"select","currentValue":"agent",
	    "options":[{"value":"read-only"},{"value":"agent"},{"value":"agent-full-access"}]
	  }]
	}
	)json"));
	TestTrue(TEXT("agent mode is available"), NwiroCodex::HasMode(Result, TEXT("agent")));
	TestTrue(TEXT("read-only mode is available"), NwiroCodex::HasMode(Result, TEXT("read-only")));
	TestFalse(TEXT("legacy auto mode is never selected"), NwiroCodex::HasMode(Result, TEXT("auto")));

	NwiroCodex::FSelectConfigCatalog ModeCatalog;
	TestTrue(TEXT("exact agent config acknowledgement is accepted"),
		NwiroCodex::IsExactSelectConfigAck(Result, TEXT("mode"), TEXT("agent"), ModeCatalog));
	TestTrue(TEXT("read-only is selectable through config options"),
		ModeCatalog.Values.Contains(TEXT("read-only")));
	TestFalse(TEXT("auto is absent from successor config options"),
		ModeCatalog.Values.Contains(TEXT("auto")));
	TestFalse(TEXT("wrong requested mode cannot release a prompt"),
		NwiroCodex::IsExactSelectConfigAck(Result, TEXT("mode"), TEXT("read-only"), ModeCatalog));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexVersionCompatibilityTest,
	"Nwiro.IntegrationKit.Codex.VersionCompatibility",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexVersionCompatibilityTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("minimum supported successor version is accepted"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("1.1.5")));
	TestTrue(TEXT("pinned successor version is accepted"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("1.1.7")));
	TestTrue(TEXT("newer compatible minor remains accepted"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("1.42.0")));
	TestFalse(TEXT("version below the supported floor is rejected"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("1.1.4")));
	TestFalse(TEXT("future major is rejected until explicitly supported"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("2.0.0")));
	TestFalse(TEXT("legacy major is rejected"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("0.99.0")));
	TestFalse(TEXT("non-semver package version is rejected"),
		NwiroCodex::IsSupportedPackageVersion(TEXT("1.1.7-beta.1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexManagedInstallDecisionTest,
	"Nwiro.IntegrationKit.Codex.ManagedInstallDecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexManagedInstallDecisionTest::RunTest(const FString& Parameters)
{
	using NwiroCodex::EManagedInstallDecision;
	TestTrue(TEXT("a valid managed runtime is an idempotent no-op"),
		NwiroCodex::DecideManagedInstall(true, false, TEXT(""))
			== EManagedInstallDecision::UseManaged);
	TestTrue(TEXT("an equal official global runtime remains unshadowed"),
		NwiroCodex::DecideManagedInstall(false, true, TEXT("1.1.7"))
			== EManagedInstallDecision::UseGlobal);
	TestTrue(TEXT("a newer official global runtime remains unshadowed"),
		NwiroCodex::DecideManagedInstall(false, true, TEXT("1.1.8"))
			== EManagedInstallDecision::UseGlobal);
	TestTrue(TEXT("an older official global runtime permits managed install"),
		NwiroCodex::DecideManagedInstall(false, true, TEXT("1.1.6"))
			== EManagedInstallDecision::InstallManaged);
	TestTrue(TEXT("an absent official runtime permits managed install"),
		NwiroCodex::DecideManagedInstall(false, false, TEXT(""))
			== EManagedInstallDecision::InstallManaged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexCancelIsolationTest,
	"Nwiro.IntegrationKit.Codex.CancelIsolation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexCancelIsolationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("official Windows Codex cancellation retires the ambiguous ACP session"),
		NwiroCodex::ShouldRetireCancelledSession(TEXT("codex"), true));
	TestFalse(TEXT("legacy non-Windows Codex cancellation stays turn-scoped"),
		NwiroCodex::ShouldRetireCancelledSession(TEXT("codex"), false));
	TestFalse(TEXT("Claude cancellation preserves model conversation context"),
		NwiroCodex::ShouldRetireCancelledSession(TEXT("claude"), true));
	TestFalse(TEXT("local-LLM cancellation remains turn-scoped"),
		NwiroCodex::ShouldRetireCancelledSession(TEXT("localllm"), true));
	return true;
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexRuntimeLeaseTest,
	"Nwiro.IntegrationKit.Codex.RuntimeLease",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexRuntimeLeaseTest::RunTest(const FString& Parameters)
{
	const FString TestDir = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("NwiroCodexRuntimeLeaseTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString LockPath = FPaths::Combine(TestDir, TEXT("runtime.lock"));

	void* ReaderOne = nullptr;
	void* ReaderTwo = nullptr;
	void* Writer = nullptr;
	void* ReaderDuringWrite = nullptr;

	TestTrue(TEXT("first managed process can take a shared lease"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(LockPath, false, ReaderOne));
	TestTrue(TEXT("a second editor can concurrently use the same immutable runtime"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(LockPath, false, ReaderTwo));
	TestFalse(TEXT("runtime mutation is refused while either editor is using it"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(LockPath, true, Writer));

	NwiroCodex::ReleaseRuntimeFileLock(ReaderOne);
	TestFalse(TEXT("one remaining reader still blocks mutation"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(LockPath, true, Writer));
	NwiroCodex::ReleaseRuntimeFileLock(ReaderTwo);
	TestTrue(TEXT("mutation can begin only after all readers release"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(LockPath, true, Writer));
	TestFalse(TEXT("a new process cannot launch during mutation"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(
			LockPath, false, ReaderDuringWrite));

	NwiroCodex::ReleaseRuntimeFileLock(Writer);
	TestTrue(TEXT("managed launches resume after mutation completes"),
		NwiroCodex::AcquireRuntimeFileLockAtPath(
			LockPath, false, ReaderDuringWrite));
	NwiroCodex::ReleaseRuntimeFileLock(ReaderDuringWrite);

	IFileManager::Get().DeleteDirectory(*TestDir, false, true);
	return true;
}
#endif
