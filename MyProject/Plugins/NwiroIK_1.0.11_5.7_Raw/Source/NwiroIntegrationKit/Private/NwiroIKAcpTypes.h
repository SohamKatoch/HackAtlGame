// Copyright 2026 Nwiro. All Rights Reserved.
//
// Shared ACP wire types. Extracted from NwiroIKBridge.h so the permission coordinator can
// hold an Acp::FRequestId without including the bridge — the bridge owns the coordinator,
// so the include would be circular.
//
// ─── PORTING BANNER (codex-acp-string-id) ────────────────────────────────────
// Staging diff applied in nwiro-agents repo's plugin/ copy. Source-of-truth
// repo is leartes/nwiro/plugin — port this block + matching .cpp changes there
// before next `bun extract`, otherwise the fix is lost on round-trip.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace Acp
{
	// JSON-RPC 2.0 permits `id` to be Number | String | Null (§4 Request object).
	// ACP's RequestId enum mirrors this: { Null, Number(i64), Str(String) }.
	// Bridge-originated ids stay int32 (NextRpcId++ controls them); this type
	// is only used on the inbound-request → outbound-response round-trip path
	// where the agent picks the wire shape and we must echo it verbatim.
	struct FRequestId
	{
		TSharedPtr<FJsonValue> Value;

		FRequestId() = default;
		explicit FRequestId(TSharedPtr<FJsonValue> InValue) : Value(MoveTemp(InValue)) {}

		bool IsValid() const { return Value.IsValid(); }
		bool IsNull() const { return !Value.IsValid() || Value->Type == EJson::Null; }

		FString ToLogString() const
		{
			if (!Value.IsValid()) return TEXT("<missing>");
			switch (Value->Type)
			{
				case EJson::Number: return FString::Printf(TEXT("%g"), Value->AsNumber());
				case EJson::String: return FString::Printf(TEXT("\"%s\""), *Value->AsString());
				case EJson::Null:   return TEXT("null");
				default:            return TEXT("<unknown>");
			}
		}
	};

	// Read the raw `id` field (Number | String | Null) from an inbound message.
	// Returns nullptr if the field is missing — callers treat that as a notification.
	inline TSharedPtr<FJsonValue> ExtractRpcId(const TSharedPtr<FJsonObject>& Msg)
	{
		if (!Msg.IsValid()) return nullptr;
		const TSharedPtr<FJsonValue>* Field = Msg->Values.Find(TEXT("id"));
		return Field ? *Field : nullptr;
	}
}
