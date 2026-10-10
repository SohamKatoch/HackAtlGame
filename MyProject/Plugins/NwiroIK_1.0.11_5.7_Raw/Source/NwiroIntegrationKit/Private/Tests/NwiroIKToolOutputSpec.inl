// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroIKToolOutputSpec.inl — pins every `rawOutput` shape the tool badge must render.
//
// Origin: a live "run all tools" session (2026-08-06) in which every nwiro MCP tool badge
// opened to show Arguments and a BLANK Result, despite the tools having succeeded.
//
// codex-acp builds MCP results with createMcpRawOutput(result, error) -> { result, error },
// so the payload is:
//
//     "rawOutput": { "result": { "content": [{type,text}] }, "error": null }
//
// The extractor read `rawOutput.content` — one level too shallow — so the text was never
// found and the `output` field was never emitted. This is the SECOND bug of this exact
// shape in this one field (the changelog carries an earlier "ACP rawOutput parsing fixed"),
// which is why the shapes are enumerated as assertions rather than trusted to a comment.
//
// Included at FILE SCOPE from NwiroIKBridge.cpp under WITH_DEV_AUTOMATION_TESTS.

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKToolOutputSpec,
	"Nwiro.IntegrationKit.Tools.RawOutputExtraction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKToolOutputSpec::RunTest(const FString& Parameters)
{
	// Parses a full tool_call_update and returns what the badge would display.
	auto Extract = [](const FString& UpdateJson) -> FString
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UpdateJson);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) return TEXT("<parse-error>");
		return ExtractAcpToolOutputText(Obj->TryGetField(TEXT("rawOutput")));
	};

	// --- Shape 1: codex-acp 1.1.7 MCP tools — THE REGRESSION ---------------------
	// createMcpRawOutput(result, error). Nothing else in this file matters if this
	// one is wrong: it is the shape every nwiro tool actually sends.
	TestEqual(TEXT("result.content is extracted (codex-acp MCP tools)"),
		Extract(TEXT(
			"{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Created BP_Foo\"}]},"
			"\"error\":null}}")),
		FString(TEXT("Created BP_Foo")));

	TestEqual(TEXT("multiple content items are newline-joined"),
		Extract(TEXT(
			"{\"rawOutput\":{\"result\":{\"content\":["
			"{\"type\":\"text\",\"text\":\"line one\"},"
			"{\"type\":\"text\",\"text\":\"line two\"}]},\"error\":null}}")),
		FString(TEXT("line one\nline two")));

	// --- Shape 2: content directly on rawOutput (codex 0.13.0) -------------------
	TestEqual(TEXT("direct content still works"),
		Extract(TEXT("{\"rawOutput\":{\"content\":[{\"type\":\"text\",\"text\":\"direct\"}]}}")),
		FString(TEXT("direct")));

	// --- Shape 3: bare array (older adapters) ------------------------------------
	TestEqual(TEXT("bare content array still works"),
		Extract(TEXT("{\"rawOutput\":[{\"type\":\"text\",\"text\":\"bare\"}]}")),
		FString(TEXT("bare")));

	// --- Shape 4: plain string ---------------------------------------------------
	TestEqual(TEXT("plain string passes through"),
		Extract(TEXT("{\"rawOutput\":\"just text\"}")),
		FString(TEXT("just text")));

	// result may itself be a string or an array rather than {content:[...]}. Note BOTH
	// carry `error` — that key is the MCP envelope's signature (createMcpRawOutput emits
	// `result` and `error` together or nothing at all) and is what licenses reading
	// `result` as text. See the image-generation block below for why that matters.
	TestEqual(TEXT("result as a bare string, inside the MCP envelope"),
		Extract(TEXT("{\"rawOutput\":{\"result\":\"stringy\",\"error\":null}}")),
		FString(TEXT("stringy")));
	TestEqual(TEXT("result as a bare array, inside the MCP envelope"),
		Extract(TEXT("{\"rawOutput\":{\"result\":[{\"type\":\"text\",\"text\":\"arr\"}],\"error\":null}}")),
		FString(TEXT("arr")));

	// --- Shapes 5 and 6: codex's OWN tools ---------------------------------------
	// `wait`, `shell` and `apply_patch` are function calls, not MCP calls: codex-acp
	// routes them through createFunctionCallOutputUpdate, which never touches
	// createMcpRawOutput. Same blank-Result symptom, different key — fixing only the
	// MCP shape left `wait` still showing nothing.
	TestEqual(TEXT("output as a plain string (codex function call)"),
		Extract(TEXT("{\"rawOutput\":{\"output\":\"waited 5s\"}}")),
		FString(TEXT("waited 5s")));

	TestEqual(TEXT("output as an array of text items"),
		Extract(TEXT("{\"rawOutput\":{\"output\":[{\"text\":\"first\"},{\"text\":\"second\"}]}}")),
		FString(TEXT("first\nsecond")));

	// codex wraps a shell result as {output, metadata:{exit_code}} — one level in.
	TestEqual(TEXT("output as {output, metadata}"),
		Extract(TEXT(
			"{\"rawOutput\":{\"output\":{\"output\":\"total 0\",\"metadata\":{\"exit_code\":0}}}}")),
		FString(TEXT("total 0")));

	TestEqual(TEXT("formatted_output (terminal path)"),
		Extract(TEXT("{\"rawOutput\":{\"formatted_output\":\"build ok\",\"exit_code\":0}}")),
		FString(TEXT("build ok")));

	// A command that genuinely printed nothing must stay empty. Synthesising "exit 0"
	// here would put text in the Result pane that the tool never produced.
	TestEqual(TEXT("empty formatted_output stays empty"),
		Extract(TEXT("{\"rawOutput\":{\"formatted_output\":\"\",\"exit_code\":0}}")), FString());

	// Guard against the reverse regression: adding these keys must not shadow `result`.
	// (`error` present ⇒ genuine MCP envelope; without it this would not be one.)
	TestEqual(TEXT("result still wins when both are present"),
		Extract(TEXT(
			"{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"mcp\"}]},"
			"\"error\":null,\"output\":\"fn\"}}")),
		FString(TEXT("mcp")));

	// The localllm shim sends flat {content, isError} and NO result/output/error key.
	// It has no automated end-to-end coverage, so its exact frame is pinned here.
	TestEqual(TEXT("localllm shim frame (flat content + isError)"),
		Extract(TEXT(
			"{\"rawOutput\":{\"content\":[{\"type\":\"text\",\"text\":\"shim says hi\"}],"
			"\"isError\":false}}")),
		FString(TEXT("shim says hi")));

	// --- Shape 7: image generation — `result` is a base64 PNG, NOT text -----------
	// codex reuses the same `result` key for the complete image. Rendering it as text put
	// a multi-megabyte string into an uncapped bridge event and tool block, which is enough
	// to freeze or reload the CEF panel. Only bounded metadata is shown.
	{
		// Long enough that any leak is unmistakable in a diff, and shaped like real base64.
		FString Base64;
		for (int32 i = 0; i < 400; ++i) Base64 += TEXT("iVBORw0KGgoAAAANSUhEUg");

		const FString Frame = FString::Printf(TEXT(
			"{\"rawOutput\":{\"status\":\"completed\",\"revisedPrompt\":\"a red cube\","
			"\"result\":\"%s\",\"savedPath\":\"/Game/Gen/cube.png\"}}"), *Base64);
		const FString Rendered = Extract(Frame);

		TestEqual(TEXT("image generation shows only bounded metadata"), Rendered,
			FString(TEXT("Revised prompt: a red cube\nSaved to: /Game/Gen/cube.png")));
		// The assertion that actually matters: not one byte of the payload gets through.
		TestFalse(TEXT("no base64 payload reaches the Result pane"),
			Rendered.Contains(TEXT("iVBORw0KGgo")));
	}

	// Without savedPath the frame is still image generation — revisedPrompt alone identifies it.
	TestEqual(TEXT("image generation without a saved path"),
		Extract(TEXT("{\"rawOutput\":{\"status\":\"completed\",\"revisedPrompt\":\"a blue sphere\","
			"\"result\":\"QUJDREVG\"}}")),
		FString(TEXT("Revised prompt: a blue sphere")));

	// An UNRECOGNISED frame carrying a bare `result` string is not rendered at all. This is
	// the fail-safe direction: a future producer that reuses `result` for binary costs a
	// blank Result pane, not a frozen panel.
	TestEqual(TEXT("a bare result string outside the MCP envelope is NOT rendered"),
		Extract(TEXT("{\"rawOutput\":{\"status\":\"ok\",\"result\":\"c29tZSBiaW5hcnk=\"}}")),
		FString());

	// --- Output ceiling -----------------------------------------------------------
	// The renderer copies this straight into a bridge event and a tool block, neither of
	// which caps it, so the bound has to live at the point that decides the content.
	{
		FString Huge;
		while (Huge.Len() < 200 * 1024) Huge += TEXT("0123456789abcdef");
		const FString Rendered = Extract(FString::Printf(
			TEXT("{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]},"
				"\"error\":null}}"), *Huge));
		TestTrue(TEXT("an oversized result is clamped"), Rendered.Len() < 70 * 1024);
		TestTrue(TEXT("the clamp says what it dropped"),
			Rendered.Contains(TEXT("characters not shown")));
	}

	// TRUNCATION MUST NOT EAT THE FAILURE. Two ways head-only truncation loses exactly the
	// part that matters: the error is appended AFTER the result, and shell/build output
	// reports its failure in the LAST lines.
	{
		FString Huge;
		while (Huge.Len() < 200 * 1024) Huge += TEXT("filler-filler-filler ");
		const FString Rendered = Extract(FString::Printf(
			TEXT("{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]},"
				"\"error\":{\"message\":\"UNIQUE_ERROR_SENTINEL\"}}}"), *Huge));
		TestTrue(TEXT("a huge result cannot squeeze out a short error"),
			Rendered.Contains(TEXT("UNIQUE_ERROR_SENTINEL")));
		TestTrue(TEXT("and the whole thing still respects the ceiling"),
			Rendered.Len() <= 64 * 1024);
	}
	{
		// A build log: the interesting line is the last one.
		FString Log;
		while (Log.Len() < 200 * 1024) Log += TEXT("[  ok  ] compiling something\n");
		Log += TEXT("FATAL_AT_THE_VERY_END");
		const FString Rendered = Extract(FString::Printf(
			TEXT("{\"rawOutput\":{\"formatted_output\":\"%s\",\"exit_code\":1}}"), *Log));
		TestTrue(TEXT("the TAIL of oversized terminal output survives"),
			Rendered.Contains(TEXT("FATAL_AT_THE_VERY_END")));
		TestTrue(TEXT("the head survives too, so context is not lost either"),
			Rendered.Contains(TEXT("[  ok  ] compiling something")));
	}

	// --- The clamp's own contract -------------------------------------------------
	// Two defects found by review, both provable by arithmetic rather than observation.
	{
		// 1. The truncation marker is ~35 chars. A ceiling smaller than that used to return
		//    the marker ALONE — longer than the ceiling it was asked to enforce.
		FString Long;
		while (Long.Len() < 200) Long += TEXT("abcdefghij");
		const FString Tiny = ClampToolOutputText(Long, 20);
		TestTrue(TEXT("a ceiling smaller than the marker is still a ceiling"), Tiny.Len() <= 20);

		// 2. FString is UTF-16 and Left/Right cut CODE UNITS, so a cut can land between the
		//    halves of a surrogate pair and emit a lone surrogate. Build a string made
		//    entirely of a non-BMP character (U+1F600, encoded D83D DE00) so almost every
		//    cut position is mid-pair.
		FString Emoji;
		const TCHAR Pair[3] = { (TCHAR)0xD83D, (TCHAR)0xDE00, 0 };
		while (Emoji.Len() < 4000) Emoji += Pair;
		for (int32 Ceiling = 200; Ceiling < 240; ++Ceiling)
		{
			const FString Cut = ClampToolOutputText(Emoji, Ceiling);
			bool bLoneSurrogate = false;
			for (int32 i = 0; i < Cut.Len(); ++i)
			{
				const TCHAR C = Cut[i];
				const bool bHigh = C >= 0xD800 && C <= 0xDBFF;
				const bool bLow  = C >= 0xDC00 && C <= 0xDFFF;
				if (bHigh && (i + 1 >= Cut.Len() || !(Cut[i + 1] >= 0xDC00 && Cut[i + 1] <= 0xDFFF))) bLoneSurrogate = true;
				if (bLow  && (i == 0 || !(Cut[i - 1] >= 0xD800 && Cut[i - 1] <= 0xDBFF)))             bLoneSurrogate = true;
				if (bLoneSurrogate) break;
			}
			if (bLoneSurrogate)
			{
				AddError(FString::Printf(
					TEXT("clamping non-BMP text at ceiling %d produced a lone surrogate"), Ceiling));
				break;
			}
		}
	}

	// --- Errors are results too --------------------------------------------------
	// A failed MCP call carries its message in `error`. Dropping it rendered a failure
	// as an empty Result — visually identical to a success with no output.
	TestEqual(TEXT("error.message is surfaced"),
		Extract(TEXT("{\"rawOutput\":{\"result\":null,\"error\":{\"message\":\"asset not found\"}}}")),
		FString(TEXT("asset not found")));

	TestEqual(TEXT("error as a bare string is surfaced"),
		Extract(TEXT("{\"rawOutput\":{\"result\":null,\"error\":\"boom\"}}")),
		FString(TEXT("boom")));

	TestEqual(TEXT("a result AND an error are both shown"),
		Extract(TEXT(
			"{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"partial\"}]},"
			"\"error\":{\"message\":\"then failed\"}}}")),
		FString(TEXT("partial\nthen failed")));

	// --- Absent / malformed: empty, never a crash and never "[object Object]" -----
	TestEqual(TEXT("missing rawOutput yields empty"), Extract(TEXT("{}")), FString());
	TestEqual(TEXT("null rawOutput yields empty"), Extract(TEXT("{\"rawOutput\":null}")), FString());
	TestEqual(TEXT("empty content array yields empty"),
		Extract(TEXT("{\"rawOutput\":{\"result\":{\"content\":[]},\"error\":null}}")), FString());
	TestEqual(TEXT("content items without text are skipped"),
		Extract(TEXT("{\"rawOutput\":{\"result\":{\"content\":[{\"type\":\"image\",\"data\":\"…\"}]},"
			"\"error\":null}}")),
		FString());
	TestEqual(TEXT("a number rawOutput yields empty rather than garbage"),
		Extract(TEXT("{\"rawOutput\":42}")), FString());

	return true;
}
