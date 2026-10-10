// Copyright 2026 Nwiro. All Rights Reserved.

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKCodexProcessMcpConfigSpec,
	"Nwiro.IntegrationKit.Codex.ProcessLocalMcpConfig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKCodexProcessMcpConfigSpec::RunTest(const FString& Parameters)
{
	const FString Existing = TEXT(
		"{"
		"\"model\":\"gpt-5.6-sol\","
		"\"approval_policy\":\"on-request\","
		"\"mcp_servers\":{"
		"\"other\":{\"url\":\"http://127.0.0.1:9999/mcp\"},"
		"\"nwiro\":{\"url\":\"http://127.0.0.1:5353/mcp\",\"tool_timeout_sec\":5}"
		"}"
		"}");

	FString Merged;
	FString Failure;
	TestTrue(
		TEXT("process-local config can be built"),
		NwiroCodexMcpConfig::BuildProcessConfig(5357, Existing, Merged, Failure));
	TestTrue(TEXT("process-local config failure stays empty"), Failure.IsEmpty());

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Merged);
	TestTrue(
		TEXT("merged CODEX_CONFIG remains valid JSON"),
		FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());
	if (!Root.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("unrelated top-level settings are preserved"),
		Root->GetStringField(TEXT("model")),
		FString(TEXT("gpt-5.6-sol")));
	TestEqual(
		TEXT("another top-level setting is preserved"),
		Root->GetStringField(TEXT("approval_policy")),
		FString(TEXT("on-request")));

	const TSharedPtr<FJsonObject>* McpServers = nullptr;
	TestTrue(
		TEXT("merged config contains an MCP object"),
		Root->TryGetObjectField(TEXT("mcp_servers"), McpServers)
			&& McpServers != nullptr
			&& McpServers->IsValid());
	if (McpServers == nullptr || !McpServers->IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Other = nullptr;
	TestTrue(
		TEXT("foreign MCP sibling is preserved"),
		(*McpServers)->TryGetObjectField(TEXT("other"), Other)
			&& Other != nullptr
			&& Other->IsValid());
	if (Other != nullptr && Other->IsValid())
	{
		TestEqual(
			TEXT("foreign MCP URL is unchanged"),
			(*Other)->GetStringField(TEXT("url")),
			FString(TEXT("http://127.0.0.1:9999/mcp")));
	}

	const TSharedPtr<FJsonObject>* Nwiro = nullptr;
	TestTrue(
		TEXT("Nwiro MCP entry exists"),
		(*McpServers)->TryGetObjectField(TEXT("nwiro"), Nwiro)
			&& Nwiro != nullptr
			&& Nwiro->IsValid());
	if (Nwiro != nullptr && Nwiro->IsValid())
	{
		TestEqual(
			TEXT("stale Nwiro URL is replaced with this editor's port"),
			(*Nwiro)->GetStringField(TEXT("url")),
			FString(TEXT("http://127.0.0.1:5357/mcp")));
		TestEqual(
			TEXT("Nwiro tool timeout is canonical"),
			static_cast<int32>((*Nwiro)->GetNumberField(TEXT("tool_timeout_sec"))),
			120);
	}

	FString SecondEditorConfig;
	Failure.Empty();
	TestTrue(
		TEXT("a second editor can build an independent process config"),
		NwiroCodexMcpConfig::BuildProcessConfig(
			5358,
			Existing,
			SecondEditorConfig,
			Failure));
	TSharedPtr<FJsonObject> SecondRoot;
	TSharedRef<TJsonReader<>> SecondReader =
		TJsonReaderFactory<>::Create(SecondEditorConfig);
	TestTrue(
		TEXT("second editor config remains valid JSON"),
		FJsonSerializer::Deserialize(SecondReader, SecondRoot)
			&& SecondRoot.IsValid());
	const TSharedPtr<FJsonObject>* SecondMcpServers = nullptr;
	const TSharedPtr<FJsonObject>* SecondNwiro = nullptr;
	if (SecondRoot.IsValid()
		&& SecondRoot->TryGetObjectField(TEXT("mcp_servers"), SecondMcpServers)
		&& SecondMcpServers != nullptr
		&& SecondMcpServers->IsValid()
		&& (*SecondMcpServers)->TryGetObjectField(TEXT("nwiro"), SecondNwiro)
		&& SecondNwiro != nullptr
		&& SecondNwiro->IsValid())
	{
		TestEqual(
			TEXT("second editor receives only its own bound port"),
			(*SecondNwiro)->GetStringField(TEXT("url")),
			FString(TEXT("http://127.0.0.1:5358/mcp")));
	}
	else
	{
		AddError(TEXT("Second editor config is missing mcp_servers.nwiro."));
	}

	Merged.Empty();
	TestFalse(
		TEXT("malformed inherited CODEX_CONFIG fails closed"),
		NwiroCodexMcpConfig::BuildProcessConfig(
			5357,
			TEXT("{not-json"),
			Merged,
			Failure));
	TestTrue(TEXT("malformed config is not partially replaced"), Merged.IsEmpty());

	Merged.Empty();
	TestFalse(
		TEXT("non-object mcp_servers fails closed"),
		NwiroCodexMcpConfig::BuildProcessConfig(
			5357,
			TEXT("{\"mcp_servers\":[]}"),
			Merged,
			Failure));
	TestTrue(TEXT("non-object MCP failure is actionable"),
		Failure.Contains(TEXT("must be an object")));

	return true;
}
