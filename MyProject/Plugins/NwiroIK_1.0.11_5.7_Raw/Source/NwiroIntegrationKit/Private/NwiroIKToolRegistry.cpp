// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKToolRegistry.h"
#include "NwiroToolProvider.h"
#include "Features/IModularFeatures.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeCounter64.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroRegistry, Log, All);
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "JsonObjectConverter.h"
#include "Misc/ConfigCacheIni.h"
#include "Runtime/Launch/Resources/Version.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

// NWIRO_HAS_TOOLSET_REGISTRY is defined by the .Build.cs (1 on UE 5.8+ where the
// experimental ToolsetRegistry module exists and is linked, 0 otherwise). We key
// the native-tool import off it rather than the engine version alone, so the
// include/link only happens when the module is actually available.
#ifndef NWIRO_HAS_TOOLSET_REGISTRY
#define NWIRO_HAS_TOOLSET_REGISTRY 0
#endif

#if NWIRO_HAS_TOOLSET_REGISTRY
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/Toolset.h"
#endif

// ════════════════════════════════════════════════════════════════════════════
//  Shared helpers
// ════════════════════════════════════════════════════════════════════════════
namespace
{
	FString JsonEscape(const FString& In)
	{
		FString S = In;
		S.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		S.ReplaceInline(TEXT("\""), TEXT("\\\""));
		S.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		S.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		S.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return S;
	}

	const TCHAR* RiskToStr(ENwiroToolRisk Risk)
	{
		switch (Risk)
		{
			case ENwiroToolRisk::ReadOnly:    return TEXT("read_only");
			case ENwiroToolRisk::Destructive: return TEXT("destructive");
			default:                          return TEXT("mutating");
		}
	}

	ENwiroToolRisk RiskFromStr(const FString& S)
	{
		if (S.Equals(TEXT("read_only"), ESearchCase::IgnoreCase) || S.Equals(TEXT("readonly"), ESearchCase::IgnoreCase))
			return ENwiroToolRisk::ReadOnly;
		if (S.Equals(TEXT("destructive"), ESearchCase::IgnoreCase))
			return ENwiroToolRisk::Destructive;
		return ENwiroToolRisk::Mutating;
	}

	/** Keep only lowercase [a-z0-9_.] — the allowed namespace/leaf charset. */
	FString Sanitize(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR C : In)
		{
			const TCHAR L = FChar::ToLower(C);
			if ((L >= 'a' && L <= 'z') || (L >= '0' && L <= '9') || L == TEXT('_') || L == TEXT('.'))
			{
				Out.AppendChar(L);
			}
		}
		return Out;
	}

	bool IsReservedNamespace(const FString& Id)
	{
		static const TCHAR* const Reserved[] = {
			TEXT("nwiro"), TEXT("ue"), TEXT("system"), TEXT("internal"), TEXT("debug"), TEXT("mcp")
		};
		FString Head = Id;
		int32 Dot = INDEX_NONE;
		if (Id.FindChar(TEXT('.'), Dot)) { Head = Id.Left(Dot); }
		for (const TCHAR* R : Reserved)
		{
			if (Head.Equals(R, ESearchCase::IgnoreCase)) { return true; }
		}
		return false;
	}

	TArray<INwiroToolProvider*> GetProviders()
	{
		return IModularFeatures::Get().GetModularFeatureImplementations<INwiroToolProvider>(
			INwiroToolProvider::GetFeatureName());
	}

	/** A single resolved external tool, ready to serialize + dispatch. */
	struct FResolvedTool
	{
		FString FullName;        // namespaced, e.g. plugin.acme.build_road
		FString Description;
		FString InputSchemaJson; // a JSON object (string)
		ENwiroToolRisk Risk = ENwiroToolRisk::Mutating;
		FString Source;          // third_party | epic_import
		FString ProviderDisplay;
		bool bGameThread = true;

		// Dispatch binding (exactly one is set depending on Source):
		INwiroToolProvider* Provider = nullptr;   // Phase 1
		FString ProviderLeaf;                      // leaf name for Provider->Execute
		UFunction* Function = nullptr;             // Phase 2 (reflection)
		bool bRawStringForm = false;               // reflection: single FString passthrough
		// Phase 3 (Epic 5.8 native): route through the engine's ToolsetRegistry,
		// exactly like UE's own MCP server (ExecuteTool by toolset+tool name).
		bool bEpic = false;
		FString EpicToolsetName;                   // fully-qualified, e.g. editor_toolset.toolsets.blueprint.BlueprintTools
		FString EpicToolName;                      // leaf, e.g. create
	};

	/** `DisabledSuffix` is either empty (agent-facing defs) or a
	 *  `,"disabled":true|false` fragment (settings-UI defs). */
	FString SerializeTool(const FResolvedTool& T, const TCHAR* DisabledSuffix = TEXT(""))
	{
		FString Schema = T.InputSchemaJson.TrimStartAndEnd();
		if (Schema.IsEmpty()) { Schema = TEXT("{\"type\":\"object\",\"properties\":{}}"); }
		return FString::Printf(
			TEXT("{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":%s,\"risk\":\"%s\",\"source\":\"%s\",\"provider\":\"%s\"%s}"),
			*T.FullName, *JsonEscape(T.Description), *Schema, RiskToStr(T.Risk),
			*T.Source, *JsonEscape(T.ProviderDisplay), DisabledSuffix);
	}

	// ── Reflection (Phase 2) — JSON-schema type for a UFUNCTION parameter ──
	const TCHAR* SchemaTypeOf(const FProperty* P)
	{
		if (P->IsA<FIntProperty>() || P->IsA<FInt64Property>() || P->IsA<FByteProperty>()) return TEXT("integer");
		if (P->IsA<FFloatProperty>() || P->IsA<FDoubleProperty>()) return TEXT("number");
		if (P->IsA<FBoolProperty>()) return TEXT("boolean");
		if (P->IsA<FArrayProperty>()) return TEXT("array");
		if (P->IsA<FStructProperty>() || P->IsA<FMapProperty>()) return TEXT("object");
		return TEXT("string"); // FStrProperty/FNameProperty/FTextProperty/FEnumProperty/objects
	}

	bool ParamHasDefault(const UFunction* Fn, const FProperty* P)
	{
#if WITH_EDITOR
		return Fn->HasMetaData(*FString::Printf(TEXT("CPP_Default_%s"), *P->GetName()));
#else
		return false;
#endif
	}

	/** Resolve a UClass from a path or bare name (mirrors the built-in tools'
	 *  StaticLoadClass("/Script/Engine.<Name>") convention). */
	UClass* ResolveClassFromString(const FString& InStr)
	{
		FString S = InStr;
		S.TrimStartAndEndInline();
		if (S.IsEmpty()) return nullptr;
		// Strip decorations like Class'/Script/Engine.PointLight'.
		int32 Q1, Q2;
		if (S.FindChar(TEXT('\''), Q1) && S.FindLastChar(TEXT('\''), Q2) && Q2 > Q1)
		{
			S = S.Mid(Q1 + 1, Q2 - Q1 - 1);
		}
		if (UClass* C = FindObject<UClass>(nullptr, *S)) return C;
		if (UClass* C = StaticLoadClass(UObject::StaticClass(), nullptr, *S)) return C;
		if (!S.Contains(TEXT(".")) && !S.Contains(TEXT("/")))
		{
			// Bare name (e.g. "PointLight"): retry as an Engine-qualified path through
			// the same resolvers that work for full paths above (StaticLoadClass loads
			// native classes that FindObject/FindFirstObject miss). No infinite
			// recursion — the qualified string contains '.'/'/'.
			if (UClass* C = ResolveClassFromString(FString::Printf(TEXT("/Script/Engine.%s"), *S))) return C;
			if (UClass* C = FindFirstObject<UClass>(*S, EFindFirstObjectOptions::NativeFirst)) return C;
			if (UClass* C = UClass::TryFindTypeSlow<UClass>(S)) return C;
		}
		return nullptr;
	}

#if WITH_EDITOR
	/** Resolve a live actor in the editor world by name/label (4-tier fuzzy match,
	 *  mirrors FindActorByName in NwiroIKEnvironmentTools). Editor-only. */
	AActor* ResolveActorFromString(const FString& Name)
	{
		if (Name.IsEmpty() || !GEditor) return nullptr;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return nullptr;
		AActor *Exact = nullptr, *CI = nullptr, *SW = nullptr, *Ct = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			const FString N = A->GetName();
			const FString L = A->GetActorLabel();
			if (N == Name || L == Name) { Exact = A; break; }
			if (!CI && (N.Equals(Name, ESearchCase::IgnoreCase) || L.Equals(Name, ESearchCase::IgnoreCase))) CI = A;
			if (!SW && (N.StartsWith(Name, ESearchCase::IgnoreCase) || L.StartsWith(Name, ESearchCase::IgnoreCase))) SW = A;
			if (!Ct && (N.Contains(Name, ESearchCase::IgnoreCase) || L.Contains(Name, ESearchCase::IgnoreCase))) Ct = A;
		}
		return Exact ? Exact : (CI ? CI : (SW ? SW : Ct));
	}
#endif

	/** Resolve a UObject for an object-typed param: actor-typed → editor world;
	 *  otherwise load the asset by path. */
	UObject* ResolveObjectFromString(const UClass* PropClass, const FString& S)
	{
		if (S.IsEmpty()) return nullptr;
#if WITH_EDITOR
		if (PropClass && PropClass->IsChildOf(AActor::StaticClass()))
		{
			return ResolveActorFromString(S);
		}
#endif
		if (UObject* O = FindObject<UObject>(nullptr, *S)) return O;
		return StaticLoadObject(PropClass ? const_cast<UClass*>(PropClass) : UObject::StaticClass(), nullptr, *S);
	}

	/** JSON-schema description hint for reference params, so clients know the format. */
	FString SchemaHintOf(const FProperty* P)
	{
		if (P->IsA<FClassProperty>() || P->IsA<FSoftClassProperty>())
			return TEXT("Class path (e.g. /Script/Engine.PointLight) or class name");
		if (const FObjectPropertyBase* OPB = CastField<FObjectPropertyBase>(P))
		{
			if (OPB->PropertyClass && OPB->PropertyClass->IsChildOf(AActor::StaticClass()))
				return TEXT("Actor name or label in the current level");
			return TEXT("Asset object path (e.g. /Game/Folder/Asset.Asset)");
		}
		return FString();
	}

	enum class EParamApply { Applied, UnresolvedRef, Unhandled };

	/** Set one UFUNCTION parameter from a JSON value. Scalars, object/class/actor
	 *  references (hard + soft), and structs. Returns UnresolvedRef when a non-empty
	 *  reference string fails to resolve, so the caller can fail honestly instead of
	 *  running the function with null inputs. */
	EParamApply SetParamFromJson(FProperty* P, void* ValuePtr, const TSharedPtr<FJsonValue>& V, FString& OutError)
	{
		if (!V.IsValid()) return EParamApply::Unhandled;
		if (FBoolProperty* BP = CastField<FBoolProperty>(P)) { BP->SetPropertyValue(ValuePtr, V->AsBool()); return EParamApply::Applied; }
		if (FIntProperty* IP = CastField<FIntProperty>(P)) { IP->SetPropertyValue(ValuePtr, (int32)V->AsNumber()); return EParamApply::Applied; }
		if (FInt64Property* I64 = CastField<FInt64Property>(P)) { I64->SetPropertyValue(ValuePtr, (int64)V->AsNumber()); return EParamApply::Applied; }
		if (FByteProperty* BYP = CastField<FByteProperty>(P)) { BYP->SetPropertyValue(ValuePtr, (uint8)V->AsNumber()); return EParamApply::Applied; }
		if (FFloatProperty* FP = CastField<FFloatProperty>(P)) { FP->SetPropertyValue(ValuePtr, (float)V->AsNumber()); return EParamApply::Applied; }
		if (FDoubleProperty* DP = CastField<FDoubleProperty>(P)) { DP->SetPropertyValue(ValuePtr, V->AsNumber()); return EParamApply::Applied; }
		if (FStrProperty* SP = CastField<FStrProperty>(P)) { SP->SetPropertyValue(ValuePtr, V->AsString()); return EParamApply::Applied; }
		if (FNameProperty* NP = CastField<FNameProperty>(P)) { NP->SetPropertyValue(ValuePtr, FName(*V->AsString())); return EParamApply::Applied; }
		if (FTextProperty* TP = CastField<FTextProperty>(P)) { TP->SetPropertyValue(ValuePtr, FText::FromString(V->AsString())); return EParamApply::Applied; }
		// Object / class / actor references — unified through FObjectPropertyBase
		// (soft variants override SetObjectPropertyValue to wrap the FSoftObjectPtr).
		if (FObjectPropertyBase* OPB = CastField<FObjectPropertyBase>(P))
		{
			const FString Ref = V->AsString();
			if (Ref.IsEmpty()) return EParamApply::Applied; // explicit "none" — leave null
			// A param wanting a UClass is either a TSubclassOf (FClassProperty) OR a
			// plain UClass* (FObjectProperty whose PropertyClass IS UClass — e.g. Epic's
			// `actor_type: unreal.Class`). Both must resolve class names, not assets.
			const bool bWantsClass = P->IsA<FClassProperty>() || P->IsA<FSoftClassProperty>()
				|| (OPB->PropertyClass && OPB->PropertyClass->IsChildOf(UClass::StaticClass()));
			UObject* Resolved = bWantsClass
				? (UObject*)ResolveClassFromString(Ref)
				: ResolveObjectFromString(OPB->PropertyClass, Ref);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve '%s'"), *Ref);
				return EParamApply::UnresolvedRef;
			}
			OPB->SetObjectPropertyValue(ValuePtr, Resolved);
			return EParamApply::Applied;
		}
		// Structs (e.g. FTransform) — best-effort via the JSON converter.
		if (FStructProperty* STP = CastField<FStructProperty>(P))
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				FJsonObjectConverter::JsonObjectToUStruct(Obj->ToSharedRef(), STP->Struct, ValuePtr, 0, 0);
			}
			return EParamApply::Applied;
		}
		// Unsupported types are left at their default-initialized value.
		return EParamApply::Unhandled;
	}

}

// ════════════════════════════════════════════════════════════════════════════
//  Frozen session state + disabled-namespace controls
// ════════════════════════════════════════════════════════════════════════════
namespace
{
	bool GFrozen = false;
	TArray<FResolvedTool> GTools;        // resolved external tools (session-frozen)
	TSet<FString> GDisabled;             // user-disabled namespaces (lowercased)
	bool GDisabledLoaded = false;

	const TCHAR* DisabledIniSection = TEXT("NwiroIK.ToolSources");
	const TCHAR* DisabledIniKey = TEXT("DisabledNamespaces");
	const TCHAR* DisabledIniInitKey = TEXT("Initialized");

	void LoadDisabledOnce()
	{
		if (GDisabledLoaded) return;
		GDisabledLoaded = true;
		if (!GConfig) return;

		bool bInitialized = false;
		GConfig->GetBool(DisabledIniSection, DisabledIniInitKey, bInitialized, GEditorPerProjectIni);
		if (!bInitialized)
		{
			// First run: Epic native MCP (ue.*) tools are OFF by default. The
			// user can enable them in Settings -> Tool Sources, which writes the
			// Initialized flag and makes their explicit choice authoritative.
			GDisabled.Add(TEXT("ue"));
			return;
		}
		TArray<FString> Arr;
		GConfig->GetArray(DisabledIniSection, DisabledIniKey, Arr, GEditorPerProjectIni);
		for (const FString& N : Arr) { GDisabled.Add(N.ToLower()); }
	}

	/** True if `FullName`'s namespace is switched off by the user. */
	bool IsDisabled(const FString& FullName)
	{
		LoadDisabledOnce();
		if (GDisabled.Num() == 0) return false;
		const FString Lower = FullName.ToLower();
		for (const FString& Ns : GDisabled)
		{
			if (Lower == Ns || Lower.StartsWith(Ns + TEXT("."))) { return true; }
		}
		return false;
	}

	// ── Source resolvers (each appends FResolvedTool into Out) ──────────────
	void ResolveModularProviders(TArray<FResolvedTool>& Out, TSet<FString>& Seen)
	{
		for (INwiroToolProvider* Provider : GetProviders())
		{
			if (!Provider) continue;
			if (Provider->GetNwiroToolApiVersion() > INwiroToolProvider::ApiVersion) continue;
			const FString Id = Sanitize(Provider->GetProviderId());
			if (Id.IsEmpty() || IsReservedNamespace(Id)) continue;

			TArray<FNwiroToolDef> Tools;
			Provider->GetToolDefinitions(Tools);
			const FString Display = Provider->GetDisplayName();
			for (const FNwiroToolDef& Tool : Tools)
			{
				const FString Leaf = Sanitize(Tool.Name);
				if (Leaf.IsEmpty()) continue;
				const FString FullName = FString::Printf(TEXT("plugin.%s.%s"), *Id, *Leaf);
				if (Seen.Contains(FullName)) continue;
				Seen.Add(FullName);

				FResolvedTool R;
				R.FullName = FullName;
				R.Description = Tool.Description;
				R.InputSchemaJson = Tool.InputSchemaJson;
				R.Risk = Tool.Risk;
				R.Source = TEXT("third_party");
				R.ProviderDisplay = Display;
				R.bGameThread = Tool.bGameThread;
				R.Provider = Provider;
				R.ProviderLeaf = Leaf;
				Out.Add(MoveTemp(R));
			}
		}
	}

	/** Build a resolved tool from a tagged UFUNCTION — schema from the raw-string
	 *  convention, an explicit NwiroSchema meta, or the typed parameters. */
	FResolvedTool BuildReflectionTool(UFunction* Fn, const FString& FullName,
		const TCHAR* Source, ENwiroToolRisk Risk, const FString& Description, bool bAllowRawForm)
	{
		// Inputs are params WITHOUT CPF_OutParm. CPF_OutParm also flags the return
		// param and any by-ref outputs — those are results, not advertised inputs.
		TArray<FProperty*> Params;
		for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
		{
			FProperty* P = *PIt;
			if (P->HasAnyPropertyFlags(CPF_OutParm)) continue;
			Params.Add(P);
		}
		// The single-FString raw-args convention is OURS only — never applied to
		// imported AICallable tools, where a lone FString is a real argument.
		const bool bRaw = bAllowRawForm && Params.Num() == 1 && Params[0]->IsA<FStrProperty>();

		FResolvedTool R;
		R.FullName = FullName;
		R.Description = Description;
		R.Risk = Risk;
		R.Source = Source;
		if (const UClass* Owner = Fn->GetOwnerClass()) { R.ProviderDisplay = Owner->GetName(); }
		R.bGameThread = true; // reflection calls always marshal to the game thread
		R.Function = Fn;
		R.bRawStringForm = bRaw;

		bool bSchemaSet = false;
#if WITH_EDITOR
		if (Fn->HasMetaData(TEXT("NwiroSchema")))
		{
			R.InputSchemaJson = Fn->GetMetaData(TEXT("NwiroSchema"));
			bSchemaSet = true;
		}
#endif
		if (!bSchemaSet && bRaw)
		{
			R.InputSchemaJson = TEXT("{\"type\":\"object\"}");
			bSchemaSet = true;
		}
		if (!bSchemaSet)
		{
			FString Props, Required;
			for (FProperty* P : Params)
			{
				if (!Props.IsEmpty()) Props += TEXT(",");
				const FString Hint = SchemaHintOf(P);
				if (Hint.IsEmpty())
				{
					Props += FString::Printf(TEXT("\"%s\":{\"type\":\"%s\"}"), *P->GetName(), SchemaTypeOf(P));
				}
				else
				{
					Props += FString::Printf(TEXT("\"%s\":{\"type\":\"%s\",\"description\":\"%s\"}"),
						*P->GetName(), SchemaTypeOf(P), *JsonEscape(Hint));
				}
				if (!ParamHasDefault(Fn, P))
				{
					if (!Required.IsEmpty()) Required += TEXT(",");
					Required += FString::Printf(TEXT("\"%s\""), *P->GetName());
				}
			}
			R.InputSchemaJson = FString::Printf(
				TEXT("{\"type\":\"object\",\"properties\":{%s},\"required\":[%s]}"), *Props, *Required);
		}
		return R;
	}

	void ResolveReflectionTools(TArray<FResolvedTool>& Out, TSet<FString>& Seen)
	{
#if WITH_EDITOR
		for (TObjectIterator<UFunction> It; It; ++It)
		{
			UFunction* Fn = *It;
			if (!Fn || !Fn->HasMetaData(TEXT("NwiroTool"))) continue;

			const FString Leaf = Sanitize(Fn->GetMetaData(TEXT("NwiroTool")));
			if (Leaf.IsEmpty()) continue;

			// Authority id: explicit NwiroProvider meta, else owning class package.
			FString Id = Fn->HasMetaData(TEXT("NwiroProvider"))
				? Sanitize(Fn->GetMetaData(TEXT("NwiroProvider")))
				: FString();
			if (Id.IsEmpty())
			{
				if (const UClass* Owner = Fn->GetOwnerClass())
				{
					Id = Sanitize(FPackageName::GetShortName(Owner->GetOutermost()->GetName()));
				}
			}
			if (Id.IsEmpty() || IsReservedNamespace(Id)) continue;

			const FString FullName = FString::Printf(TEXT("plugin.%s.%s"), *Id, *Leaf);
			if (Seen.Contains(FullName)) continue;
			Seen.Add(FullName);

			const FString Desc = Fn->HasMetaData(TEXT("NwiroDesc")) ? Fn->GetMetaData(TEXT("NwiroDesc"))
				: (Fn->HasMetaData(TEXT("ToolTip")) ? Fn->GetMetaData(TEXT("ToolTip")) : FString());
			Out.Add(BuildReflectionTool(Fn, FullName, TEXT("third_party"),
				RiskFromStr(Fn->GetMetaData(TEXT("NwiroRisk"))), Desc, /*bAllowRawForm=*/true));
		}
#endif // WITH_EDITOR
	}

	/** Returns true once the ToolsetRegistry is available (so the caller can stop
	 *  retrying and freeze the set). Returns false when the registry/subsystem
	 *  isn't up yet — the registry populates a little after the MCP server starts,
	 *  so an early session-start call would otherwise freeze zero native tools. */
	bool ResolveEpicToolsets(TArray<FResolvedTool>& Out, TSet<FString>& Seen)
	{
#if NWIRO_HAS_TOOLSET_REGISTRY
		// UE 5.8 native MCP tools live in the engine's ToolsetRegistry (many are
		// Python-backed and registry-mediated, NOT plain AICallable UFUNCTIONs).
		// We import them THE SAME WAY UE's own MCP server does: enumerate the
		// registry, read each toolset's GetJsonSchema() (which already carries
		// per-tool name + inputSchema), and route execution through ExecuteTool.
		auto SubResult = UToolsetRegistrySubsystem::Get();
		if (SubResult.HasError()) return false;
		UToolsetRegistrySubsystem* Sub = SubResult.GetValue();
		if (!Sub) return false;

		// The registry fills in two waves: C++ AICallable toolsets first, then the
		// Python-backed editor_toolset.* toolsets (loaded late via init_unreal.py)
		// which carry most native tools. Freezing after only the first wave would
		// lock in ~262 tools instead of ~830. So wait until at least one Python-
		// backed toolset is present before declaring the registry ready.
		int32 ToolsetCount = 0;
		bool bHasPythonToolsets = false;
		Sub->ToolsetRegistry.ForEachToolset(
			[&ToolsetCount, &bHasPythonToolsets](const FString& Name, const UE::ToolsetRegistry::FToolset&)
			{
				++ToolsetCount;
				if (Name.Contains(TEXT("editor_toolset"))) { bHasPythonToolsets = true; }
			});
		if (ToolsetCount == 0 || !bHasPythonToolsets) return false;

		Sub->ToolsetRegistry.ForEachToolset(
			[&Out, &Seen](const FString& /*RegName*/, const UE::ToolsetRegistry::FToolset& Toolset)
		{
			const FString SchemaJson = Toolset.GetJsonSchema();
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(SchemaJson);
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;

			const FString FullToolsetName = Root->GetStringField(TEXT("name"));
			if (FullToolsetName.IsEmpty()) return;
			// Skip Epic's internal test/mock fixtures.
			if (FullToolsetName.Contains(TEXT("Fake")) || FullToolsetName.Contains(TEXT("Mock"))) return;

			// Short namespace id = last dotted segment, sanitized (BlueprintTools -> blueprinttools).
			FString ShortId = FullToolsetName;
			int32 LastDot = INDEX_NONE;
			if (FullToolsetName.FindLastChar(TEXT('.'), LastDot)) ShortId = FullToolsetName.RightChop(LastDot + 1);
			ShortId = Sanitize(ShortId);
			if (ShortId.IsEmpty()) return;

			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!Root->TryGetArrayField(TEXT("tools"), Tools) || !Tools) return;

			for (const TSharedPtr<FJsonValue>& TV : *Tools)
			{
				const TSharedPtr<FJsonObject> TObj = TV.IsValid() ? TV->AsObject() : nullptr;
				if (!TObj.IsValid()) continue;

				const FString FullToolName = TObj->GetStringField(TEXT("name"));
				if (FullToolName.IsEmpty()) continue;
				// Leaf = the tool name minus the "<toolset>." prefix (fallback: after last dot).
				FString Leaf = FullToolName;
				if (FullToolName.StartsWith(FullToolsetName + TEXT(".")))
				{
					Leaf = FullToolName.RightChop(FullToolsetName.Len() + 1);
				}
				else
				{
					int32 D = INDEX_NONE;
					if (FullToolName.FindLastChar(TEXT('.'), D)) Leaf = FullToolName.RightChop(D + 1);
				}
				const FString SanLeaf = Sanitize(Leaf);
				if (SanLeaf.IsEmpty()) continue;

				const FString NsName = FString::Printf(TEXT("ue.%s.%s"), *ShortId, *SanLeaf);
				if (Seen.Contains(NsName)) continue;
				Seen.Add(NsName);

				FResolvedTool Res;
				Res.FullName = NsName;
				Res.Description = TObj->HasField(TEXT("description")) ? TObj->GetStringField(TEXT("description")) : FString();
				// Use the registry's own inputSchema verbatim (already valid JSON-schema).
				const TSharedPtr<FJsonObject>* InSchema = nullptr;
				if (TObj->TryGetObjectField(TEXT("inputSchema"), InSchema) && InSchema)
				{
					FString S;
					TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
						TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&S);
					FJsonSerializer::Serialize(InSchema->ToSharedRef(), W);
					Res.InputSchemaJson = S;
				}
				Res.Risk = ENwiroToolRisk::Mutating; // registry tools are treated as mutating by default
				Res.Source = TEXT("epic_import");
				Res.ProviderDisplay = TEXT("Unreal Engine");
				Res.bEpic = true;
				Res.EpicToolsetName = FullToolsetName;
				Res.EpicToolName = Leaf;
				Out.Add(MoveTemp(Res));
			}
		});
		return true;
#else
		(void)Out; (void)Seen; // ToolsetRegistry module not available before UE 5.8
		return true; // nothing to wait for — freeze immediately on older engines
#endif
	}

	void EnsureFrozen()
	{
		if (GFrozen) return;
		// Re-resolve each call until the native ToolsetRegistry is up, THEN freeze.
		// The registry populates shortly after the MCP server starts, so freezing
		// on the very first call would lock in zero ue.* tools. Built-in/provider/
		// reflection resolution is cheap enough to repeat across the few early
		// turns before the registry is ready.
		GTools.Reset();
		TSet<FString> Seen;
		ResolveModularProviders(GTools, Seen);
		ResolveReflectionTools(GTools, Seen);
		const bool bEpicReady = ResolveEpicToolsets(GTools, Seen);
		if (bEpicReady) { GFrozen = true; }
	}

	FString InvokeReflection(const FResolvedTool& T, const FString& ArgsJson)
	{
		UFunction* Fn = T.Function;
		if (!Fn) return TEXT("{\"success\":false,\"error\":\"reflection function missing\"}");
		UClass* Owner = Fn->GetOwnerClass();
		UObject* Target = Owner ? Owner->GetDefaultObject() : nullptr;
		if (!Target) return TEXT("{\"success\":false,\"error\":\"no target object for reflection tool\"}");

		void* Buffer = FMemory::Malloc(FMath::Max<int32>(Fn->ParmsSize, 1));
		FMemory::Memzero(Buffer, Fn->ParmsSize);
		for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
		{
			PIt->InitializeValue_InContainer(Buffer);
		}

		// ── Inputs: params WITHOUT CPF_OutParm ──
		if (T.bRawStringForm)
		{
			// Our raw convention: the whole args blob goes into the single FString.
			for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
			{
				FProperty* P = *PIt;
				if (P->HasAnyPropertyFlags(CPF_OutParm)) continue;
				if (FStrProperty* SP = CastField<FStrProperty>(P))
				{
					SP->SetPropertyValue_InContainer(Buffer, ArgsJson);
				}
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Args;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
			FJsonSerializer::Deserialize(Reader, Args);
			FString RefError;
			for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
			{
				FProperty* P = *PIt;
				if (P->HasAnyPropertyFlags(CPF_OutParm)) continue;
				if (Args.IsValid() && Args->HasField(P->GetName()))
				{
					FString ParamErr;
					if (SetParamFromJson(P, P->ContainerPtrToValuePtr<void>(Buffer),
						Args->TryGetField(P->GetName()), ParamErr) == EParamApply::UnresolvedRef)
					{
						RefError = FString::Printf(TEXT("parameter '%s': %s"), *P->GetName(), *ParamErr);
						break;
					}
				}
			}
			if (!RefError.IsEmpty())
			{
				// Honest failure instead of running the function with null inputs.
				for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
				{
					PIt->DestroyValue_InContainer(Buffer);
				}
				FMemory::Free(Buffer);
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *JsonEscape(RefError));
			}
		}

		Target->ProcessEvent(Fn, Buffer);

		// Locate the return param (it carries CPF_ReturnParm + CPF_OutParm).
		FProperty* ReturnProp = nullptr;
		for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
		{
			if (PIt->HasAnyPropertyFlags(CPF_ReturnParm)) { ReturnProp = *PIt; break; }
		}

		FString Result;
		if (T.bRawStringForm && ReturnProp && ReturnProp->IsA<FStrProperty>())
		{
			// Our raw tools already return their own JSON envelope — pass through.
			Result = CastField<FStrProperty>(ReturnProp)->GetPropertyValue_InContainer(Buffer);
		}
		else
		{
			// Typed / imported tools: serialize the return + every out-param to real
			// JSON via FJsonObjectConverter → {"success":true,"return":<json>,"out":{...}}.
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("success"), true);
			TSharedRef<FJsonObject> OutObj = MakeShared<FJsonObject>();
			bool bHasReturn = false;
			int32 OutCount = 0;
			for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
			{
				FProperty* P = *PIt;
				if (!P->HasAnyPropertyFlags(CPF_OutParm)) continue;
				TSharedPtr<FJsonValue> JV = FJsonObjectConverter::UPropertyToJsonValue(
					P, P->ContainerPtrToValuePtr<void>(Buffer), 0, 0);
				if (!JV.IsValid()) { JV = MakeShared<FJsonValueNull>(); }
				if (P->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					Root->SetField(TEXT("return"), JV);
					bHasReturn = true;
				}
				else
				{
					OutObj->SetField(P->GetName(), JV);
					++OutCount;
				}
			}
			if (!bHasReturn) { Root->SetField(TEXT("return"), MakeShared<FJsonValueNull>()); }
			if (OutCount > 0) { Root->SetObjectField(TEXT("out"), OutObj); }

			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(Root, Writer);
		}

		for (TFieldIterator<FProperty> PIt(Fn); PIt && (PIt->PropertyFlags & CPF_Parm); ++PIt)
		{
			PIt->DestroyValue_InContainer(Buffer);
		}
		FMemory::Free(Buffer);
		return Result;
	}

	FString RunOnGameThread(TFunction<FString()> Work, bool bGameThread)
	{
		if (!bGameThread || IsInGameThread())
		{
			return Work();
		}
		TSharedRef<TPromise<FString>> Promise = MakeShared<TPromise<FString>>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [Promise, Work]() { Promise->SetValue(Work()); });
		if (Future.WaitFor(FTimespan::FromSeconds(60.0))) { return Future.Get(); }
		return TEXT("{\"success\":false,\"error\":\"provider tool timed out\"}");
	}

#if NWIRO_HAS_TOOLSET_REGISTRY
	// ─────────────────────────────────────────────────────────────────────────
	// Asynchronous ue.* (Epic ToolsetRegistry) execution.
	//
	// ToolsetRegistry.ExecuteTool returns a TFuture that, for some tools, is
	// only fulfilled after further GAME-THREAD work (message-bus delivery,
	// automation worker discovery, ...). Waiting on it FROM the game thread is
	// therefore a CIRCULAR wait, not a slow one. Measured with
	// ue.automationtesttoolset.discovertests: zero game-thread ticks across
	// 120s, and the tool's own work then completed 0.7s AFTER the wait gave up.
	//
	// Pumping the ticker instead of deferring is not an option: this runs
	// inside an FTSTicker element's own Fire() (FHttpServerModule is an
	// FTSTickerObjectBase), and FTSTicker::Tick is non-reentrant by assert
	// (Ticker.cpp:102) — a nested Tick() trips `checkf(GetThreadId(PrevState)
	// == 0)` and its Elements.Reset() invalidates the outer loop's reference.
	// The only way to let the ticker run is to UNWIND the handler.
	//
	// Hence: call ExecuteTool on the game thread (the engine requires it),
	// never wait, and when the future is not already fulfilled, park an
	// execution record and settle it later. Every mutation of the pending map
	// happens on the game thread, which is why it needs no lock — the future's
	// continuation only marshals an integer id and a finished string across.
	// ─────────────────────────────────────────────────────────────────────────

	/** Compile-gated tick probe. Proves the game thread advanced while an Epic
	 *  call was in flight (`delta > 0`). OFF by default — this is acceptance
	 *  instrumentation, not shipping behaviour. */
	#ifndef NWIRO_ASYNC_PROBE_ENABLED
	#define NWIRO_ASYNC_PROBE_ENABLED 0
	#endif

	#if NWIRO_ASYNC_PROBE_ENABLED
	FThreadSafeCounter64 GNwiroGameThreadTicks;
	FTSTicker::FDelegateHandle GProbeTickerHandle;

	int64 CurrentProbeTick() { return GNwiroGameThreadTicks.GetValue(); }

	void EnsureAsyncProbeTicker()
	{
		if (GProbeTickerHandle.IsValid() || !IsInGameThread()) { return; }
		GProbeTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float) { GNwiroGameThreadTicks.Increment(); return true; }),
			0.0f);
	}
	void ShutdownAsyncProbeTicker()
	{
		if (GProbeTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GProbeTickerHandle);
			GProbeTickerHandle.Reset();
		}
	}
	void LogAsyncProbe(const FString& ToolName, bool bDeferred, int64 StartTick,
		double StartTime, const TCHAR* Outcome)
	{
		UE_LOG(LogNwiroRegistry, Display,
			TEXT("NWIRO_ASYNC_PROBE tool=%s deferred=%s start_tick=%lld end_tick=%lld delta=%lld elapsed=%.1f outcome=%s"),
			*ToolName, bDeferred ? TEXT("true") : TEXT("false"), StartTick,
			GNwiroGameThreadTicks.GetValue(), GNwiroGameThreadTicks.GetValue() - StartTick,
			FPlatformTime::Seconds() - StartTime, Outcome);
	}
	#else
	inline int64 CurrentProbeTick() { return 0; }
	inline void EnsureAsyncProbeTicker() {}
	inline void ShutdownAsyncProbeTicker() {}
	inline void LogAsyncProbe(const FString&, bool, int64, double, const TCHAR*) {}
	#endif

	/** Wrap a ToolsetRegistry outcome in the {"success":..,"return":..} envelope.
	 *  Pure string work — safe to run on ANY thread, which is what lets the
	 *  future's continuation do it before marshalling to the game thread. */
	FString EnvelopeFromResult(TValueOrError<FString, FString> R)
	{
		if (R.HasError())
		{
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *JsonEscape(R.GetError()));
		}
		// The registry returns the tool's own JSON; embed it verbatim under
		// "return" (string-quote only if it isn't valid JSON) for a consistent
		// {"success":true,"return":...} envelope.
		FString Val = R.GetValue().TrimStartAndEnd();
		if (Val.IsEmpty())
		{
			Val = TEXT("null");
		}
		else
		{
			TSharedPtr<FJsonValue> Parsed;
			TSharedRef<TJsonReader<>> VR = TJsonReaderFactory<>::Create(Val);
			if (!FJsonSerializer::Deserialize(VR, Parsed) || !Parsed.IsValid())
			{
				Val = FString::Printf(TEXT("\"%s\""), *JsonEscape(Val));
			}
		}
		return FString::Printf(TEXT("{\"success\":true,\"return\":%s}"), *Val);
	}

	/** Stable, machine-assertable error identities. The English text may be
	 *  reworded; these codes may not. */
	const TCHAR* const kCodeTimeout   = TEXT("nwiro.epic.timeout");
	const TCHAR* const kCodeShutdown  = TEXT("nwiro.epic.shutdown");
	const TCHAR* const kCodeInFlight  = TEXT("nwiro.epic.duplicate_in_flight");
	const TCHAR* const kCodeNeedAsync = TEXT("nwiro.epic.async_required");
	const TCHAR* const kCodeNoSubsys  = TEXT("nwiro.epic.registry_unavailable");

	FString EpicError(const TCHAR* Code, const FString& Message)
	{
		return FString::Printf(
			TEXT("{\"success\":false,\"code\":\"%s\",\"error\":\"%s\",\"retryable\":false}"),
			Code, *JsonEscape(Message));
	}

	/** Recursively re-emit a JSON value with object keys SORTED, so that
	 *  {"a":1,"b":2} and {"b":2,"a":1} — semantically the same call — produce
	 *  the same text. A round-trip alone does not do this: UE preserves field
	 *  order, so reordered args would defeat duplicate suppression. */
	void WriteCanonical(const TSharedPtr<FJsonValue>& V,
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>& W,
		const FString& Name)
	{
		const bool bNamed = !Name.IsEmpty();
		switch (V->Type)
		{
		case EJson::Object:
		{
			bNamed ? W->WriteObjectStart(Name) : W->WriteObjectStart();
			const TSharedPtr<FJsonObject> Obj = V->AsObject();
			if (Obj.IsValid())
			{
				// FJsonObject::Values is keyed by a template FStringType, so the
				// key is converted explicitly rather than assumed to be FString.
				using FCanonPair = TPair<FString, TSharedPtr<FJsonValue>>;
				TArray<FCanonPair> Pairs;
				for (const auto& P : Obj->Values) { Pairs.Emplace(FString(P.Key), P.Value); }
				Pairs.Sort([](const FCanonPair& A, const FCanonPair& B) { return A.Key < B.Key; });
				for (const FCanonPair& P : Pairs) { WriteCanonical(P.Value, W, P.Key); }
			}
			W->WriteObjectEnd();
			break;
		}
		case EJson::Array:
		{
			bNamed ? W->WriteArrayStart(Name) : W->WriteArrayStart();
			for (const TSharedPtr<FJsonValue>& E : V->AsArray()) { WriteCanonical(E, W, FString()); }
			W->WriteArrayEnd();
			break;
		}
		case EJson::String: bNamed ? W->WriteValue(Name, V->AsString()) : W->WriteValue(V->AsString()); break;
		case EJson::Number: bNamed ? W->WriteValue(Name, V->AsNumber()) : W->WriteValue(V->AsNumber()); break;
		case EJson::Boolean: bNamed ? W->WriteValue(Name, V->AsBool()) : W->WriteValue(V->AsBool()); break;
		default: bNamed ? W->WriteNull(Name) : W->WriteNull(); break;
		}
	}

	/** Canonical dedup identity: FULL tool name (not the leaf — two toolsets may
	 *  share a leaf) plus canonicalized arguments.
	 *
	 *  Takes the EXECUTION INPUT, i.e. the string actually handed to
	 *  ExecuteTool, so that "" and "{}" — which execute identically — key
	 *  identically. Keys are sorted recursively so field order cannot defeat
	 *  suppression. */
	FString MakeEpicKey(const FString& FullToolName, const FString& ExecutionInput)
	{
		FString Canon = ExecutionInput;
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ExecutionInput);
		if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
		{
			FString Re;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Re);
			WriteCanonical(MakeShared<FJsonValueObject>(Obj), W, FString());
			W->Close();
			Canon = Re;
		}
		return FullToolName + TEXT("|") + Canon;
	}

	/** Hard cap on tracked operations. A never-completing engine op is retained
	 *  forever by design (its key must keep suppressing retries), so without a
	 *  bound a pathological tool could accumulate records that are rescanned
	 *  every frame. Refusing to START new work is the safe failure. */
	constexpr int32 kMaxPendingEpic = 64;

	/** One in-flight deferred ue.* execution. GAME-THREAD OWNED — created, read,
	 *  and destroyed only on the game thread, so no lock is required.
	 *
	 *  TWO INDEPENDENT STATES, deliberately not conflated:
	 *
	 *    RESPONSE  open -> settled   (bResponseSettled / OnResult)
	 *    OPERATION active -> done    (Future.IsReady())
	 *
	 *  A transport timeout settles the RESPONSE but does NOT cancel the engine
	 *  OPERATION — nothing can, since dropping a TFuture does not cancel work.
	 *  The record therefore SURVIVES a timeout so its key keeps suppressing
	 *  duplicates, and is only erased once the operation genuinely completes.
	 *  Collapsing these two into one lifetime is what would let an identical
	 *  retry at T+110s start a second concurrent engine operation.
	 *
	 *  The future is OWNED here and polled from the ticker. We deliberately do
	 *  NOT use TFuture::Next: that installs plugin-module code into an
	 *  engine-owned future which nothing can detach, so a completion arriving
	 *  after this module unloads (dynamic unloading is on by default —
	 *  ModuleInterface.h:88) would jump into freed DLL code. Dropping a polled
	 *  future registers nothing and is safe at any time. */
	struct FPendingEpicCall
	{
		int64                          ExecId = 0;
		FString                        ToolName;   // full identity, for logs + key
		FString                        Key;        // dedup identity
		TFuture<TValueOrError<FString, FString>> Future;  // OWNED; move-only
		TFunction<void(FString)>       OnResult;   // transport-neutral sink
		double                         Deadline = 0.0;
		bool                           bResponseSettled = false;
		int64                          StartTick = 0;
		double                         StartTime = 0.0;
	};

	TMap<int64, FPendingEpicCall> GPendingEpic;
	int64 GNextEpicExecId = 1;
	FTSTicker::FDelegateHandle GEpicTickHandle;

	/** Below the MCP layer's own 120s ceiling so the client receives a real
	 *  JSON-RPC error rather than a transport timeout. */
	constexpr double kEpicDeadlineSec = 110.0;

	void StopEpicTicker()
	{
		if (GEpicTickHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GEpicTickHandle);
			GEpicTickHandle.Reset();
		}
	}

	/** Settle the RESPONSE exactly once. Moving the sink out is the claim: a
	 *  second attempt finds an empty TFunction and does nothing. Says nothing
	 *  about the operation, which may still be running. */
	void SettleResponse(FPendingEpicCall& Call, FString Result, const TCHAR* Outcome)
	{
		check(IsInGameThread());
		if (Call.bResponseSettled) { return; }
		Call.bResponseSettled = true;
		LogAsyncProbe(Call.ToolName, /*bDeferred*/true, Call.StartTick, Call.StartTime, Outcome);
		TFunction<void(FString)> Sink = MoveTemp(Call.OnResult);
		Call.OnResult = nullptr;
		if (Sink) { Sink(MoveTemp(Result)); }
	}

	bool TickEpicCalls(float)
	{
		check(IsInGameThread());

		// Decide first, act second: a sink may start another tool call and
		// mutate GPendingEpic, which would invalidate a live iterator.
		struct FAction { int64 ExecId; FString Payload; const TCHAR* Outcome; bool bErase; };
		TArray<FAction> Actions;
		const double Now = FPlatformTime::Seconds();

		for (TPair<int64, FPendingEpicCall>& KV : GPendingEpic)
		{
			FPendingEpicCall& C = KV.Value;
			if (C.Future.IsReady())
			{
				// OPERATION done -> settle if still open, then free the key.
				Actions.Add({ KV.Key, EnvelopeFromResult(C.Future.Get()), TEXT("completed"), true });
			}
			else if (!C.bResponseSettled && Now >= C.Deadline)
			{
				// RESPONSE times out; OPERATION keeps running, record survives.
				Actions.Add({ KV.Key,
					EpicError(kCodeTimeout,
						TEXT("ue tool timed out; the engine operation is still running, do not retry")),
					TEXT("timeout"), false });
			}
		}

		for (FAction& A : Actions)
		{
			if (FPendingEpicCall* C = GPendingEpic.Find(A.ExecId))
			{
				SettleResponse(*C, MoveTemp(A.Payload), A.Outcome);
			}
			if (A.bErase) { GPendingEpic.Remove(A.ExecId); }
		}

		if (GPendingEpic.Num() == 0) { StopEpicTicker(); }
		return true;
	}

	void EnsureEpicTicker()
	{
		if (GEpicTickHandle.IsValid()) { return; }
		GEpicTickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&TickEpicCalls), 0.0f);
	}

	/** Park an already-started future through the real pending-call path.
	 *  Shared by production and the automation spec so the tested path IS the
	 *  shipping path. */
	int64 ParkEpicFuture(const FString& FullToolName, const FString& Key,
		TFuture<TValueOrError<FString, FString>>&& Future,
		TFunction<void(FString)> OnResult, double DeadlineSeconds,
		int64 StartTick, double StartTime)
	{
		check(IsInGameThread());
		const int64 ExecId = GNextEpicExecId++;
		FPendingEpicCall Call;
		Call.ExecId    = ExecId;
		Call.ToolName  = FullToolName;
		Call.Key       = Key;
		Call.Future    = MoveTemp(Future);
		Call.OnResult  = MoveTemp(OnResult);
		Call.Deadline  = FPlatformTime::Seconds() + DeadlineSeconds;
		Call.StartTick = StartTick;
		Call.StartTime = StartTime;
		GPendingEpic.Add(ExecId, MoveTemp(Call));
		EnsureEpicTicker();
		return ExecId;
	}
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  Public API
// ════════════════════════════════════════════════════════════════════════════
namespace
{
	void AppendDefsTail(FString& J, const TArray<FString>& Objects)
	{
		if (Objects.Num() == 0) return;

		if (!J.EndsWith(TEXT("[")) && !J.EndsWith(TEXT(",")))
		{
			J += TEXT(",");
		}
		J += FString::Join(Objects, TEXT(","));
	}
}

void FNwiroIKToolRegistry::AppendEnabledProviderDefs(FString& J)
{
	EnsureFrozen();

	TArray<FString> Objects;
	for (const FResolvedTool& T : GTools)
	{
		if (IsDisabled(T.FullName)) continue;
		Objects.Add(SerializeTool(T));
	}
	AppendDefsTail(J, Objects);
}

void FNwiroIKToolRegistry::AppendAllProviderDefsWithState(FString& J)
{
	EnsureFrozen();

	TArray<FString> Objects;
	for (const FResolvedTool& T : GTools)
	{
		Objects.Add(SerializeTool(T,
			IsDisabled(T.FullName) ? TEXT(",\"disabled\":true") : TEXT(",\"disabled\":false")));
	}
	AppendDefsTail(J, Objects);
}

ENwiroIKEpicStart FNwiroIKToolRegistry::BeginEpicTool(
	const FString& ToolName, const FString& ArgsJson,
	TFunction<void(FString)> OnResult, FString& OutResult)
{
#if !NWIRO_HAS_TOOLSET_REGISTRY
	return ENwiroIKEpicStart::NotEpic;
#else
	// ExecuteTool is an editor-subsystem API: it must be CALLED on the game
	// thread. Off-thread callers fall through to their existing path.
	if (!IsInGameThread() || !ToolName.StartsWith(TEXT("ue."))) { return ENwiroIKEpicStart::NotEpic; }
	if (IsDisabled(ToolName)) { return ENwiroIKEpicStart::NotEpic; }

	EnsureFrozen();
	const FResolvedTool* Found = GTools.FindByPredicate(
		[&ToolName](const FResolvedTool& C) { return C.FullName == ToolName; });
	if (!Found || !Found->bEpic) { return ENwiroIKEpicStart::NotEpic; }

	// Copy out of the registry BEFORE anything can Invalidate() and rebuild it.
	const FString ToolsetName = Found->EpicToolsetName;
	const FString LeafName    = Found->EpicToolName;

	auto SubResult = UToolsetRegistrySubsystem::Get();
	if (SubResult.HasError() || !SubResult.GetValue())
	{
		OutResult = EpicError(kCodeNoSubsys, TEXT("ToolsetRegistry not available"));
		return ENwiroIKEpicStart::Completed;
	}

	// Key off the EXECUTION INPUT, not the raw argument text, so "" and "{}"
	// (which execute identically) cannot occupy two different slots.
	const FString Trimmed = ArgsJson.TrimStartAndEnd();
	const FString Input = Trimmed.IsEmpty() ? TEXT("{}") : ArgsJson;

	// Reject a duplicate whose ENGINE OPERATION is still running — which
	// includes one whose response already timed out, because a timeout does
	// not cancel anything. Without this, an agent retrying after the 110s
	// deadline would start a second concurrent operation, and so on.
	const FString Key = MakeEpicKey(ToolName, Input);
	for (const TPair<int64, FPendingEpicCall>& KV : GPendingEpic)
	{
		if (KV.Value.Key == Key)
		{
			OutResult = EpicError(kCodeInFlight,
				TEXT("An identical ue.* operation is still running in the engine; wait for it rather than retrying"));
			return ENwiroIKEpicStart::Completed;
		}
	}

	// Refuse to start rather than grow without bound. Records for
	// never-completing operations are retained deliberately, so this is the
	// only place the growth can be stopped.
	if (GPendingEpic.Num() >= kMaxPendingEpic)
	{
		UE_LOG(LogNwiroRegistry, Warning,
			TEXT("Refusing ue.* call '%s': %d operations already in flight."),
			*ToolName, GPendingEpic.Num());
		OutResult = EpicError(kCodeInFlight,
			TEXT("Too many ue.* operations are already in flight; wait for them to finish"));
		return ENwiroIKEpicStart::Completed;
	}

	EnsureAsyncProbeTicker();
	const int64  StartTick = CurrentProbeTick();
	const double StartTime = FPlatformTime::Seconds();

	TFuture<TValueOrError<FString, FString>> Future =
		SubResult.GetValue()->ToolsetRegistry.ExecuteTool(
			UE::ToolsetRegistry::FToolDescriptor{ ToolsetName, LeafName }, Input);

	// Already fulfilled: the overwhelmingly common case, and byte-for-byte the
	// behaviour that shipped. No parking, no ticker, no extra frame.
	if (Future.IsReady())
	{
		OutResult = EnvelopeFromResult(Future.Get());
		LogAsyncProbe(ToolName, /*bDeferred*/false, StartTick, StartTime, TEXT("completed"));
		return ENwiroIKEpicStart::Completed;
	}

	// Deferred, but this caller cannot await a late result. NOT an assertion:
	// several Epic toolsets always defer, so this is an expected runtime
	// condition, not a violated invariant.
	//
	// The operation HAS started and cannot be cancelled, so it is parked with a
	// null sink regardless — that is what keeps the key suppressing duplicate
	// retries and what lets the ticker reap it when it finishes. The previous
	// version returned without tracking it, which leaked the operation.
	if (!OnResult)
	{
		UE_LOG(LogNwiroRegistry, Warning,
			TEXT("Deferred ue.* call '%s' requires async dispatch; this call site cannot await it."),
			*ToolName);
		LogAsyncProbe(ToolName, /*bDeferred*/true, StartTick, StartTime, TEXT("needs_async"));
		ParkEpicFuture(ToolName, Key, MoveTemp(Future), nullptr,
			kEpicDeadlineSec, StartTick, StartTime);
		OutResult = EpicError(kCodeNeedAsync,
			TEXT("This ue.* tool defers its result and must be dispatched asynchronously; this call site cannot await it"));
		return ENwiroIKEpicStart::Completed;
	}

	ParkEpicFuture(ToolName, Key, MoveTemp(Future), MoveTemp(OnResult),
		kEpicDeadlineSec, StartTick, StartTime);
	return ENwiroIKEpicStart::Deferred;
#endif
}

void FNwiroIKToolRegistry::DetachPendingEpicResponses(const FString& Reason)
{
#if NWIRO_HAS_TOOLSET_REGISTRY
	check(IsInGameThread());

	// Answer anyone still waiting, but DO NOT forget the operations. A server
	// restart tears down a transport; it does not cancel engine work. Keeping
	// the records (and the ticker that reaps them) is what stops a retry after
	// the restart from starting a second concurrent operation on the same key.
	const FString Payload = EpicError(kCodeShutdown, Reason);
	for (TPair<int64, FPendingEpicCall>& KV : GPendingEpic)
	{
		SettleResponse(KV.Value, Payload, TEXT("transport_restart"));
	}
#endif
}

void FNwiroIKToolRegistry::ShutdownPendingEpicCalls(const FString& Reason)
{
#if NWIRO_HAS_TOOLSET_REGISTRY
	check(IsInGameThread());

	const FString Payload = EpicError(kCodeShutdown, Reason);
	for (TPair<int64, FPendingEpicCall>& KV : GPendingEpic)
	{
		SettleResponse(KV.Value, Payload, TEXT("shutdown"));
	}
	// Now genuinely forget everything. Safe precisely because the futures were
	// only ever POLLED (never Next()-ed): destroying them unregisters nothing,
	// so after this no plugin code is reachable from engine-owned state.
	GPendingEpic.Reset();
	StopEpicTicker();
	ShutdownAsyncProbeTicker();
#endif
}

int32 FNwiroIKToolRegistry::NumPendingEpicCalls()
{
#if NWIRO_HAS_TOOLSET_REGISTRY
	return GPendingEpic.Num();
#else
	return 0;
#endif
}

bool FNwiroIKToolRegistry::TryDispatch(const FString& ToolName, const FString& ArgsJson, FString& OutResult)
{
	if (!(ToolName.StartsWith(TEXT("plugin.")) || ToolName.StartsWith(TEXT("ue.")) || ToolName.StartsWith(TEXT("mcp."))))
	{
		return false; // not a namespaced external tool — caller falls through
	}

	if (IsDisabled(ToolName))
	{
		OutResult = FString::Printf(
			TEXT("{\"success\":false,\"error\":\"tool source disabled: %s\"}"), *ToolName);
		return true;
	}

	EnsureFrozen();
	for (const FResolvedTool& T : GTools)
	{
		if (T.FullName != ToolName) continue;

#if NWIRO_HAS_TOOLSET_REGISTRY
		if (T.bEpic)
		{
			// Legacy total path: a null sink cannot defer, so this always
			// returns Completed — with the real result when the future is
			// already fulfilled, or a structured error when it is not. It
			// never blocks, on any thread. Callers that CAN await a late
			// result go through BeginEpicTool directly.
			FNwiroIKToolRegistry::BeginEpicTool(ToolName, ArgsJson, nullptr, OutResult);
			return true;
		}
#endif
		if (T.Provider)
		{
			INwiroToolProvider* P = T.Provider;
			const FString Leaf = T.ProviderLeaf;
			OutResult = RunOnGameThread([P, Leaf, ArgsJson]() { return P->Execute(Leaf, ArgsJson); }, T.bGameThread);
			return true;
		}
		if (T.Function)
		{
			const FResolvedTool ToolCopy = T;
			OutResult = RunOnGameThread([ToolCopy, ArgsJson]() { return InvokeReflection(ToolCopy, ArgsJson); }, true);
			return true;
		}
		break;
	}

	OutResult = FString::Printf(
		TEXT("{\"success\":false,\"error\":\"No provider registered for tool: %s\"}"), *ToolName);
	return true;
}

void FNwiroIKToolRegistry::SetDisabledNamespaces(const TArray<FString>& Namespaces)
{
	LoadDisabledOnce();
	GDisabled.Reset();
	TArray<FString> Lowered;
	for (const FString& N : Namespaces)
	{
		const FString L = N.ToLower();
		GDisabled.Add(L);
		Lowered.Add(L);
	}
	if (GConfig)
	{
		GConfig->SetArray(DisabledIniSection, DisabledIniKey, Lowered, GEditorPerProjectIni);
		GConfig->SetBool(DisabledIniSection, DisabledIniInitKey, true, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
}

TArray<FString> FNwiroIKToolRegistry::GetDisabledNamespaces()
{
	LoadDisabledOnce();
	return GDisabled.Array();
}

void FNwiroIKToolRegistry::Invalidate()
{
	GFrozen = false;
	GTools.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
// Included HERE (bottom of this TU) rather than from a test-only file so the
// spec can reach the anonymous-namespace settlement internals directly and
// therefore test the shipping code path itself.
#include "Tests/NwiroIKEpicAsyncSpec.inl"
#endif
