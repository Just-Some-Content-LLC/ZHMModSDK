#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "Fixtures/B0ContractLifecycle.h"
#include "Fixtures/B0Disguise.h"
#include "Fixtures/B0ItemBytes.h"
#include "Fixtures/B0Items.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "RepositoryId.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B4: ItemPickedUp -> item.picked_up v1, ItemThrown -> item.thrown v1, ItemRemovedFromInventory
// -> item.removed_from_inventory v1, normalized from the 24 recorded B0 payloads (design section
// 38.1); the field rules of 38.5; the per-field malformed diagnostic of 38.7; the attempt-gated
// publication class; and the frame-order guarantee at both sides of a fall frame's drain.
//
// Coverage boundary (design section 38.8), stated once here and true of every case below: the
// observations are parsed by TestJson or built by hand, so these tests exercise the normalizer,
// serialization, adapter, frame and sink — downstream consumers — and never the Glacier-facing
// TelemetryIntake::Copy, its TArray<ZString> branch, string truncation, node/depth budget or the
// Null/Unsupported classification. The one production helper shared with the intake is the
// RepositoryId renderer: the cases marked "formatter-derived" build the item's RepositoryId from
// a 16-byte little-endian image through RepositoryId::FromLittleEndianBytes(...).ToDashedLowercase()
// and therefore exercise the production formatter plus everything downstream. The engine field
// types of the item object are not covered offline.
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Item(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Items[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Actor(size_t p_Index)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[p_Index].json, static_cast<uint32_t>(p_Index + 1));
    }

    TelemetryObservation Disguise(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Disguise[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    std::string Mutate(std::string p_Json, const std::string& p_From, const std::string& p_To)
    {
        const auto s_Pos = p_Json.find(p_From);
        CHECK(s_Pos != std::string::npos);
        return p_Json.replace(s_Pos, p_From.size(), p_To);
    }

    // Replaces (or adds) one field of an item object observation.
    TelemetryObservation WithField(TelemetryObservation p_Obs, std::string_view p_Key, TelemetryValue p_Value)
    {
        for (auto& s_Field : p_Obs.value.fields)
            if (s_Field.first == p_Key)
            {
                s_Field.second = std::move(p_Value);
                return p_Obs;
            }

        p_Obs.value.fields.emplace_back(std::string(p_Key), std::move(p_Value));
        return p_Obs;
    }

    TelemetryObservation WithoutField(TelemetryObservation p_Obs, std::string_view p_Key)
    {
        auto& s_Fields = p_Obs.value.fields;
        for (auto it = s_Fields.begin(); it != s_Fields.end(); ++it)
            if (it->first == p_Key)
            {
                s_Fields.erase(it);
                return p_Obs;
            }

        CHECK(false);
        return p_Obs;
    }

    TelemetryValue Unsupported(const char* p_TypeName)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::Unsupported;
        s_Value.text = p_TypeName;
        return s_Value;
    }

    SceneState Scene(const char* p_Type, int32_t p_Stage, bool p_Loaded)
    {
        SceneState s_Scene;
        s_Scene.available = true;
        s_Scene.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        s_Scene.scene_type = p_Type;
        s_Scene.codename_hint = "Peacock";
        s_Scene.loading_stage = p_Stage;
        s_Scene.scene_loaded = p_Loaded;
        return s_Scene;
    }

    struct Rig
    {
        TelemetryQueue Queue{256};
        TelemetryNormalizer Normalizer;
        MissionObserver Observer;
        CapturingSink* Sink = nullptr;
        std::unique_ptr<RelayAdapter> Adapter;
        std::vector<std::string> Warnings;
        int Ticks = 0;

        Rig()
        {
            auto s_Sink = std::make_unique<CapturingSink>();
            Sink = s_Sink.get();
            Adapter = std::make_unique<RelayAdapter>(std::move(s_Sink), "id-1", [this] { return fmt::format("t{}", ++Ticks); });
        }

        RelayFrame::Result Frame(const std::optional<SceneState>& p_Scene)
        {
            return RelayFrame::Process(
                Queue, Normalizer, Observer, Adapter.get(), p_Scene, std::nullopt,
                [this](const std::string& p_Line) { Warnings.push_back(p_Line); }
            );
        }

        std::vector<std::string> Types() const
        {
            std::vector<std::string> s_Types;
            for (const auto& s_Envelope : Sink->Published)
                s_Types.push_back(s_Envelope.event_type);
            return s_Types;
        }

        bool Contiguous() const
        {
            for (size_t i = 0; i < Sink->Published.size(); ++i)
                if (Sink->Published[i].sequence != i + 1)
                    return false;
            return true;
        }
    };

    const SceneState k_Playing = Scene("mission", 8, true);
    const SceneState k_Fallen = Scene("mission", 8, false);
    const SceneState k_Reloading = Scene("mission", 0, false);

    constexpr const char* k_SessionA = "2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f";

}

void RunItemTelemetryTests()
{
    // Table: the three supported names, all attempt-gated; neighbours that are not normalized.
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ItemPickedUp"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ItemThrown"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ItemRemovedFromInventory"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ItemDropped"));          // research candidate (38.9)
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ItemDestroyed"));        // research candidate (38.9)
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ItemStashed"));          // NPC-side, out of scope
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("Guard_FoundItem"));      // NPC-side, out of scope
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("HoldingIllegalWeapon")); // player state: B6
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("itempickedup"));         // names are exact
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ItemPickedUp "));

    // The 24 recorded payloads, in emission order: kind, provenance, definition id, session,
    // timestamp, optional fields; the per-name counts 12 / 6 / 6 and the seven definitions.
    {
        TelemetryNormalizer s_Normalizer;
        std::map<std::string, size_t> s_ByName;
        std::vector<std::string> s_Definitions;

        for (size_t i = 0; i < B0Fixtures::k_ItemCount; ++i)
        {
            const auto s_Obs = Item(i);
            const auto s_Result = s_Normalizer.Normalize(s_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
            CHECK(s_Result.gating == TelemetryNormalizer::Gating::AttemptGated);
            CHECK(s_Result.item.has_value());
            CHECK(!s_Result.event.has_value() && !s_Result.disguise.has_value() && !s_Result.contract_started.has_value()
                  && !s_Result.contract_ended.has_value());

            const auto& e = *s_Result.item;
            CHECK(e.engine_event == s_Obs.name);
            CHECK((s_Obs.name == "ItemPickedUp") == (e.kind == ItemEvent::Kind::PickedUp));
            CHECK((s_Obs.name == "ItemThrown") == (e.kind == ItemEvent::Kind::Thrown));
            CHECK((s_Obs.name == "ItemRemovedFromInventory") == (e.kind == ItemEvent::Kind::RemovedFromInventory));
            CHECK(e.item_repository_id.size() == 36);
            CHECK(!e.item_instance_id.has_value()); // "" in 24/24: absent, not empty
            CHECK(e.item_name.has_value() && !e.item_name->empty());
            CHECK(e.item_type.has_value() && !e.item_type->empty());
            CHECK(e.online_traits.has_value() && !e.online_traits->empty());
            CHECK(e.contract_session_id.has_value() && *e.contract_session_id == k_SessionA);
            CHECK(e.engine_timestamp_s.has_value() && *e.engine_timestamp_s == s_Obs.timestamp_s);

            ++s_ByName[s_Obs.name];
            if (std::find(s_Definitions.begin(), s_Definitions.end(), e.item_repository_id) == s_Definitions.end())
                s_Definitions.push_back(e.item_repository_id);
        }

        CHECK(s_ByName["ItemPickedUp"] == 12 && s_ByName["ItemThrown"] == 6 && s_ByName["ItemRemovedFromInventory"] == 6);
        CHECK(s_Definitions.size() == 7);
        CHECK(s_Definitions == (std::vector<std::string>{
            B0Fixtures::k_ItemDefinition1, B0Fixtures::k_ItemDefinition2, B0Fixtures::k_ItemDefinition3, B0Fixtures::k_ItemDefinition4,
            B0Fixtures::k_ItemDefinition5, B0Fixtures::k_ItemDefinition6, B0Fixtures::k_ItemDefinition7}));
        CHECK(s_Normalizer.GetCounters().normalized == B0Fixtures::k_ItemCount);
        CHECK(s_Normalizer.GetCounters().malformed == 0 && s_Normalizer.GetCounters().unsupported == 0);

        // Spot values: the crowbar's engine type string is carried verbatim; traits keep order.
        const auto s_Crowbar = *s_Normalizer.Normalize(Item(B0Fixtures::k_ItemCrowbarPickup1)).item;
        CHECK(*s_Crowbar.item_type == "Unrecognized Item type" && *s_Crowbar.item_name == "Crowbar");
        const auto s_Propane = *s_Normalizer.Normalize(Item(B0Fixtures::k_ItemPropaneThrown)).item;
        CHECK(*s_Propane.online_traits == (std::vector<std::string>{"melee_nonlethal", "explosive", "accident_explosion", "throw_nonlethal_deprecated"}));
        CHECK(*s_Propane.item_type == "CC_FireExtinguisher_01");
    }

    // Exact wire payloads: one shape for the three types; optional fields absent, never empty.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Pickup = *s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).item;
        CHECK(RelaySerialization::ItemPayloadJson(s_Pickup) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemPickedUp\","
            "\"item_repository_id\":\"6adddf7e-6879-4d51-a7e2-6a25ffdca6ae\","
            "\"item_name\":\"Wrench\",\"item_type\":\"CC_Wrench\","
            "\"online_traits\":[\"melee_nonlethal\",\"throw_nonlethal_deprecated\"],"
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"engine_timestamp_s\":176.078949}");

        const auto s_Removed = *s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchRemoved1)).item;
        const auto s_Thrown = *s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchThrown1)).item;
        CHECK(RelaySerialization::ItemPayloadJson(s_Removed) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemRemovedFromInventory\","
            "\"item_repository_id\":\"6adddf7e-6879-4d51-a7e2-6a25ffdca6ae\","
            "\"item_name\":\"Wrench\",\"item_type\":\"CC_Wrench\","
            "\"online_traits\":[\"melee_nonlethal\",\"throw_nonlethal_deprecated\"],"
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"engine_timestamp_s\":209.153168}");
        CHECK(RelaySerialization::ItemPayloadJson(s_Thrown).find("\"engine_event\":\"ItemThrown\"") != std::string::npos);
        // The removal and the throw differ only in provenance: identical timestamp and definition,
        // as the engine emitted them. Both exist as separate events; nothing merges them.
        CHECK(s_Removed.item_repository_id == s_Thrown.item_repository_id);
        CHECK(*s_Removed.engine_timestamp_s == *s_Thrown.engine_timestamp_s);
        CHECK(s_Removed.kind != s_Thrown.kind && s_Removed.engine_event != s_Thrown.engine_event);

        // Bare: only the required fields.
        ItemEvent s_Bare;
        s_Bare.kind = ItemEvent::Kind::Thrown;
        s_Bare.engine_event = "ItemThrown";
        s_Bare.item_repository_id = "x";
        CHECK(RelaySerialization::ItemPayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemThrown\",\"item_repository_id\":\"x\"}");

        // A non-empty instance id is carried verbatim; an empty traits array is carried as [].
        s_Bare.item_instance_id = "9a3f1dbb-0000-4000-8000-000000000001";
        s_Bare.online_traits = std::vector<std::string>{};
        CHECK(RelaySerialization::ItemPayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemThrown\",\"item_repository_id\":\"x\","
            "\"item_instance_id\":\"9a3f1dbb-0000-4000-8000-000000000001\",\"online_traits\":[]}");

        // Engine strings are quoted, not trusted: quotes, backslashes and control bytes escape.
        s_Bare.item_instance_id.reset();
        s_Bare.online_traits.reset();
        s_Bare.item_name = std::string("A\"b\\c\n") + "\x01";
        CHECK(RelaySerialization::ItemPayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemThrown\",\"item_repository_id\":\"x\","
            "\"item_name\":\"A\\\"b\\\\c\\n\\u0001\"}");
    }

    // Formatter-derived ids (design section 38.8): each of the seven definition ids built from its
    // 16-byte little-endian image through the production renderer equals the B0 string, and every
    // one of the 24 events normalizes to the same event and the same wire payload whether its
    // RepositoryId came from the image or from the recorded JSON. This exercises the shared
    // production formatter plus downstream; the recursive intake copy itself is not executed here.
    {
        TelemetryNormalizer s_Normalizer;

        for (size_t i = 0; i < 7; ++i)
        {
            const auto& s_Image = *B0Fixtures::k_ItemDefinitionImages[i];
            CHECK(RepositoryId::FromLittleEndianBytes(s_Image.le_bytes).ToDashedLowercase() == s_Image.dashed);
        }
        CHECK(RepositoryId::FromLittleEndianBytes(B0Fixtures::k_ItemImage1.le_bytes).ToDashedLowercase() == B0Fixtures::k_ItemDefinition1);
        CHECK(RepositoryId::FromLittleEndianBytes(B0Fixtures::k_ItemImage7.le_bytes).ToDashedLowercase() == B0Fixtures::k_ItemDefinition7);

        for (size_t i = 0; i < B0Fixtures::k_ItemCount; ++i)
        {
            const auto s_Obs = Item(i);
            TelemetryValue s_Built;
            s_Built.kind = TelemetryValue::Kind::String;
            s_Built.text = RepositoryId::FromLittleEndianBytes(B0Fixtures::k_ItemImages[i]->le_bytes).ToDashedLowercase();
            CHECK(s_Built.text == s_Obs.value.Find("RepositoryId")->text);

            const auto s_FromImage = s_Normalizer.Normalize(WithField(s_Obs, "RepositoryId", s_Built));
            const auto s_FromJson = s_Normalizer.Normalize(s_Obs);
            CHECK(s_FromImage.outcome == TelemetryNormalizer::Outcome::Normalized);
            CHECK(s_FromJson.outcome == TelemetryNormalizer::Outcome::Normalized);
            CHECK(*s_FromImage.item == *s_FromJson.item);
            CHECK(RelaySerialization::ItemPayloadJson(*s_FromImage.item) == RelaySerialization::ItemPayloadJson(*s_FromJson.item));
        }

        CHECK(s_Normalizer.GetCounters().normalized == 48); // 24 events x 2 paths

        // A negative: dumping the image bytes in memory order is not the id (the section 35 rule).
        std::string s_InOrder;
        for (const auto s_Byte : B0Fixtures::k_ItemImage1.le_bytes)
            s_InOrder += fmt::format("{:02x}", s_Byte);
        CHECK(s_InOrder.rfind("7edfdd6a", 0) == 0);
        CHECK(std::string(B0Fixtures::k_ItemDefinition1).rfind("6adddf7e", 0) == 0);
    }

    // The adapter maps the kind to the event type; the sequence is the one shared stream.
    {
        Rig s_Rig;
        TelemetryNormalizer s_Normalizer;
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).item);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchRemoved1)).item);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchThrown1)).item);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"item.picked_up", "item.removed_from_inventory", "item.thrown"}));
        CHECK(s_Rig.Contiguous());
        for (const auto& s_Envelope : s_Rig.Sink->Published)
            CHECK(s_Envelope.json.find("\"schema_version\":1") != std::string::npos);
        CHECK(s_Rig.Sink->Published[0].json.find("\"payload\":{\"source\":\"engine_telemetry\",\"engine_event\":\"ItemPickedUp\"") != std::string::npos);
    }

    // Optional fields: absent and valid (38.5). Absent InstanceId / ItemName / ItemType /
    // OnlineTraits are fine; a non-empty InstanceId is carried; an empty one is dropped.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Base = Item(B0Fixtures::k_ItemKnifePickup1);

        auto s_Minimal = s_Base;
        s_Minimal.value = TestJson::Parse(R"({"RepositoryId":"e17172cc-bf70-4df6-9828-d9856b1a24fd"})");
        const auto s_MinimalResult = s_Normalizer.Normalize(s_Minimal);
        CHECK(s_MinimalResult.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_MinimalResult.item->item_instance_id && !s_MinimalResult.item->item_name && !s_MinimalResult.item->item_type
              && !s_MinimalResult.item->online_traits);

        TelemetryValue s_Instance;
        s_Instance.kind = TelemetryValue::Kind::String;
        s_Instance.text = "9a3f1dbb-6f6e-4d7b-9a51-2f0c1a7b4e21";
        const auto s_WithInstance = s_Normalizer.Normalize(WithField(s_Base, "InstanceId", s_Instance));
        CHECK(s_WithInstance.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_WithInstance.item->item_instance_id.has_value() && *s_WithInstance.item->item_instance_id == s_Instance.text);
        CHECK(RelaySerialization::ItemPayloadJson(*s_WithInstance.item).find("\"item_instance_id\":\"9a3f1dbb-6f6e-4d7b-9a51-2f0c1a7b4e21\"") != std::string::npos);

        const auto s_Empty = s_Normalizer.Normalize(s_Base); // InstanceId "" as recorded
        CHECK(!s_Empty.item->item_instance_id.has_value());
        CHECK(RelaySerialization::ItemPayloadJson(*s_Empty.item).find("item_instance_id") == std::string::npos);

        // An empty traits array is valid and carried as an empty list.
        const auto s_NoTraits = s_Normalizer.Normalize(WithField(s_Base, "OnlineTraits", TestJson::Parse("[]")));
        CHECK(s_NoTraits.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_NoTraits.item->online_traits.has_value() && s_NoTraits.item->online_traits->empty());

        // Unread fields of any kind are harmless: Category null (as recorded), an Unsupported
        // ActionRewardType, an extra unknown key.
        auto s_Extra = WithField(s_Base, "ActionRewardType", Unsupported("eActionRewardType"));
        s_Extra = WithField(s_Extra, "SpawnLocation", TestJson::Parse(R"({"x":1,"y":2})"));
        CHECK(s_Normalizer.Normalize(s_Extra).outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Normalizer.GetCounters().malformed == 0);
    }

    // Malformed (38.5): Value not an object (the disguise shape under an item name; an array; a
    // number; Null; Unsupported; absent); RepositoryId missing, non-string, empty; optional
    // fields of the wrong kind. Counted per name, nothing published, no sequence consumed.
    {
        TelemetryNormalizer s_Normalizer;
        size_t s_Expected = 0;

        auto s_Case = [&](TelemetryObservation p_Obs) {
            const auto s_Result = s_Normalizer.Normalize(p_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.item.has_value());
            ++s_Expected;
            return s_Result.detail;
        };

        auto s_Disguised = Item(B0Fixtures::k_ItemWrenchPickup1);
        s_Disguised.value = Disguise(B0Fixtures::k_DisguiseChange1).value; // a bare id string
        CHECK(s_Case(s_Disguised) == "Value is not an object (kind=String bytes=36)");

        auto s_Array = Item(B0Fixtures::k_ItemWrenchThrown1);
        s_Array.value = TestJson::Parse(R"([{"RepositoryId":"x"}])");
        CHECK(s_Case(s_Array) == "Value is not an object (kind=Array items=1)");

        auto s_Number = Item(B0Fixtures::k_ItemWrenchRemoved1);
        s_Number.value = TestJson::Parse("7");
        CHECK(s_Case(s_Number) == "Value is not an object (kind=Number)");

        auto s_Null = Item(B0Fixtures::k_ItemWrenchPickup1);
        s_Null.value = TelemetryValue{};
        CHECK(s_Case(s_Null) == "Value is not an object (kind=Null)");

        auto s_Unsupported = Item(B0Fixtures::k_ItemWrenchPickup1);
        s_Unsupported.value = Unsupported("SItemTelemetry");
        CHECK(s_Case(s_Unsupported) == "Value is not an object (kind=Unsupported type='SItemTelemetry')");

        const std::string s_Base = B0Fixtures::k_Items[B0Fixtures::k_ItemWrenchPickup1].json;
        const auto s_Absent = TestJson::ObservationFromRecordedEvent(Mutate(s_Base, R"("Value":{"InstanceId":"","ItemType":"CC_Wrench","ItemName":"Wrench","RepositoryId":"6adddf7e-6879-4d51-a7e2-6a25ffdca6ae","OnlineTraits":["melee_nonlethal","throw_nonlethal_deprecated"],"Category":null,"ActionRewardType":"AR_None"},)", ""));
        CHECK(s_Absent.value.kind == TelemetryValue::Kind::Null);
        CHECK(s_Case(s_Absent) == "Value is not an object (kind=Null)");

        // The subject.
        const auto s_Wrench = Item(B0Fixtures::k_ItemWrenchPickup1);
        CHECK(s_Case(WithoutField(s_Wrench, "RepositoryId")) ==
            "missing field 'RepositoryId'; fields: 'RepositoryId' absent, 'InstanceId' kind=String bytes=0, "
            "'ItemName' kind=String bytes=6, 'ItemType' kind=String bytes=9, 'OnlineTraits' kind=Array items=2");

        TelemetryValue s_EmptyId;
        s_EmptyId.kind = TelemetryValue::Kind::String;
        CHECK(s_Case(WithField(s_Wrench, "RepositoryId", s_EmptyId)) ==
            "field 'RepositoryId' is empty; fields: 'RepositoryId' kind=String bytes=0, 'InstanceId' kind=String bytes=0, "
            "'ItemName' kind=String bytes=6, 'ItemType' kind=String bytes=9, 'OnlineTraits' kind=Array items=2");

        CHECK(s_Case(WithField(s_Wrench, "RepositoryId", Unsupported("ZGuid"))) ==
            "field 'RepositoryId' is not a string (kind=Unsupported type='ZGuid'); fields: 'RepositoryId' kind=Unsupported type='ZGuid', "
            "'InstanceId' kind=String bytes=0, 'ItemName' kind=String bytes=6, 'ItemType' kind=String bytes=9, 'OnlineTraits' kind=Array items=2");

        CHECK(s_Case(WithField(s_Wrench, "RepositoryId", TestJson::Parse("7"))).rfind("field 'RepositoryId' is not a string (kind=Number); fields:", 0) == 0);

        // Optional fields present with the wrong kind.
        CHECK(s_Case(WithField(s_Wrench, "ItemType", Unsupported("eItemType"))) ==
            "field 'ItemType' is not a string (kind=Unsupported type='eItemType'); fields: 'RepositoryId' kind=String bytes=36, "
            "'InstanceId' kind=String bytes=0, 'ItemName' kind=String bytes=6, 'ItemType' kind=Unsupported type='eItemType', 'OnlineTraits' kind=Array items=2");
        CHECK(s_Case(WithField(s_Wrench, "ItemName", TestJson::Parse("true"))).rfind("field 'ItemName' is not a string (kind=Bool); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Wrench, "InstanceId", TestJson::Parse("null"))).rfind("field 'InstanceId' is not a string (kind=Null); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Wrench, "InstanceId", TestJson::Parse(R"({"a":1})"))).rfind("field 'InstanceId' is not a string (kind=Object fields=1); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Wrench, "OnlineTraits", TestJson::Parse(R"("melee_nonlethal")"))).rfind("field 'OnlineTraits' is not an array (kind=String bytes=15); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Wrench, "OnlineTraits", TestJson::Parse(R"(["melee_nonlethal",7])"))).rfind("field 'OnlineTraits' has a non-string item (item 1 kind=Number); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Wrench, "OnlineTraits", Unsupported("TArray<eOnlineTrait>"))).rfind("field 'OnlineTraits' is not an array (kind=Unsupported type='TArray<eOnlineTrait>'); fields:", 0) == 0);

        // The first failing field wins; the per-field listing still shows every expected key.
        auto s_TwoBad = WithField(s_Wrench, "ItemName", TestJson::Parse("1"));
        s_TwoBad = WithField(s_TwoBad, "OnlineTraits", TestJson::Parse("2"));
        CHECK(s_Case(s_TwoBad) ==
            "field 'ItemName' is not a string (kind=Number); fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=0, "
            "'ItemName' kind=Number, 'ItemType' kind=String bytes=9, 'OnlineTraits' kind=Number");

        // Escaping and bounding of a type name in the listing: nothing raw leaks, 64 bytes then "...".
        auto s_Hostile = WithField(s_Wrench, "ItemType", Unsupported(""));
        {
            TelemetryValue s_Bad;
            s_Bad.kind = TelemetryValue::Kind::Unsupported;
            s_Bad.text = std::string("e'\\\n") + "\xC3\xA9" + std::string(70, 'T');
            const auto s_Detail = s_Case(WithField(s_Wrench, "ItemType", s_Bad));
            CHECK(s_Detail.find("type='e\\x27\\x5C\\x0A\\xC3\\xA9" + std::string(58, 'T') + "...'") != std::string::npos);
            CHECK(s_Detail.find('\n') == std::string::npos && s_Detail.find('\xC3') == std::string::npos);
        }
        CHECK(s_Case(s_Hostile).find("'ItemType' kind=Unsupported type=''") != std::string::npos);

        CHECK(s_Normalizer.GetCounters().malformed == s_Expected);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("ItemPickedUp") + s_Normalizer.GetCounters().malformed_by_name.at("ItemThrown")
              + s_Normalizer.GetCounters().malformed_by_name.at("ItemRemovedFromInventory") == s_Expected);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("ItemThrown") == 1);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("ItemRemovedFromInventory") == 1);

        // A valid payload is unaffected by the diagnostic path.
        CHECK(s_Normalizer.Normalize(s_Wrench).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // The diagnostic reaches the durable log through the existing warning path with the event
    // name and index, and still consumes no sequence; the valid neighbour publishes.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Obs = WithField(Item(B0Fixtures::k_ItemWrenchPickup1), "ItemType", Unsupported("eItemType"));
        CHECK(s_Rig.Queue.Push(s_Obs));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchRemoved1)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 1 && s_Frame.outcomes_published == 1 && s_Frame.outside_attempt == 0);
        CHECK(s_Rig.Sink->Published.size() == 2 && s_Rig.Sink->Published[1].sequence == 2);
        CHECK(s_Rig.Warnings.size() == 1);
        CHECK(s_Rig.Warnings[0] ==
            "telemetry 'ItemPickedUp' (index 16) not normalized: field 'ItemType' is not a string (kind=Unsupported type='eItemType'); "
            "fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=0, 'ItemName' kind=String bytes=6, "
            "'ItemType' kind=Unsupported type='eItemType', 'OnlineTraits' kind=Array items=2");
    }

    // Envelope provenance is optional: without ContractSessionId or Timestamp the occurrence is
    // still valid (the definition id is the subject), with the optional fields absent.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Obs = Item(B0Fixtures::k_ItemLeadPipeThrown);
        s_Obs.contract_session_id.clear();
        s_Obs.has_timestamp = false;
        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_Result.item->contract_session_id.has_value());
        CHECK(!s_Result.item->engine_timestamp_s.has_value());
    }

    // _DONTSEND on an item name: the section 18 policy applies whatever the name (synthetic flag
    // on a real payload; never observed on these names).
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Flagged = TestJson::ObservationFromRecordedEvent(Mutate(
            B0Fixtures::k_Items[B0Fixtures::k_ItemWrenchThrown1].json, R"("Name":"ItemThrown",)", R"("Name":"ItemThrown","_DONTSEND":true,)"));
        CHECK(s_Flagged.dont_send);
        CHECK(s_Normalizer.Normalize(s_Flagged).outcome == TelemetryNormalizer::Outcome::DontSend);
        CHECK(s_Normalizer.GetCounters().dont_send == 1 && s_Normalizer.GetCounters().normalized == 0);
    }

    // Repeated occurrences are distinct events: the same definition picked up three times is
    // three events, equal in content; no deduplication of any kind.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_First = s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1));
        const auto s_Again = s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1));
        const auto s_Second = s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup2));
        CHECK(*s_First.item == *s_Again.item);
        CHECK(s_Second.item->item_repository_id == s_First.item->item_repository_id);
        CHECK(*s_Second.item->engine_timestamp_s != *s_First.item->engine_timestamp_s);
        CHECK(s_Normalizer.GetCounters().normalized == 3);
    }

    // Independence: a throw with no removal beside it, and a removal with no throw, each
    // normalize and publish on their own. Nothing waits for, requires or infers the other.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemKnifeThrown)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemPropaneRemoved)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 1);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "item.thrown", "item.removed_from_inventory"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Warnings.empty());
    }

    // Unsupported neighbouring names are counted by name and never published; the three rows
    // beside them are unaffected. (The intake filters these before queueing in production; here
    // they are pushed to show the normalizer's own answer.)
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        const std::string s_Base = B0Fixtures::k_Items[B0Fixtures::k_ItemWrenchPickup1].json;
        for (const char* s_Name : {"ItemDropped", "ItemDestroyed", "ItemStashed", "Guard_FoundItem"})
        {
            auto s_Obs = TestJson::ObservationFromRecordedEvent(Mutate(s_Base, R"("Name":"ItemPickedUp")", fmt::format(R"("Name":"{}")", s_Name)));
            CHECK(s_Rig.Normalizer.Normalize(s_Obs).outcome == TelemetryNormalizer::Outcome::Unsupported);
            CHECK(s_Rig.Queue.Push(s_Obs));
        }
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchPickup1)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 1 && s_Frame.malformed == 0 && s_Frame.outside_attempt == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "item.picked_up"}));
        CHECK(s_Rig.Normalizer.GetCounters().unsupported == 8);
        CHECK(s_Rig.Normalizer.GetCounters().unsupported_by_name.at("ItemDropped") == 2);
        CHECK(s_Rig.Normalizer.GetCounters().unsupported_by_name.at("ItemDestroyed") == 2);
        CHECK(s_Rig.Warnings.empty());
    }

    // The B1, B2 and B3 rows keep their classes beside the new ones.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(Actor(0)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseChange1)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(
            B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractStartA].json)).gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_Normalizer.GetCounters().normalized == 3);
    }

    // Frame order, observed shape (B0 session 1 through the production sequencing): the wrench's
    // pickup, its removal and throw in one drain (two consecutive sequences, removal first as
    // captured), the pacify that followed, the re-pickup; interleaved with disguise events exactly
    // as captured, all inside the attempt.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_StartingSuitA)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchPickup1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange1)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchRemoved1)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchThrown1)));
        auto s_Drain = s_Rig.Frame(k_Playing);
        CHECK(s_Drain.outcomes_published == 3 && s_Drain.outside_attempt == 0 && s_Drain.ungated_published == 0);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchPickup2)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchRemoved2)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchThrown2)));
        CHECK(s_Rig.Queue.Push(Actor(1))); // Pacify Rousseau, 81 ms after the second throw
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "mission.playing", "disguise.equipped", "item.picked_up", "disguise.equipped", "item.removed_from_inventory", "item.thrown",
            "item.picked_up", "item.removed_from_inventory", "item.thrown", "actor.pacified"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Warnings.empty());
        CHECK(s_Rig.Sink->Published[4].sequence + 1 == s_Rig.Sink->Published[5].sequence);
    }

    // Fall-frame ordering (a), synthetic: an item event queued BEFORE the fall frame's drain is
    // judged against the playing state and publishes before mission.stopped.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemCleaverPickup)));
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.outcomes_published == 1 && s_Fall.outside_attempt == 0 && s_Fall.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "item.picked_up", "mission.stopped"}));
        CHECK(s_Rig.Sink->Published[1].sequence == 2 && s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (b), synthetic: the same observation emitted AFTER that frame's drain is
    // presented on the next processed frame, with no attempt open. Attempt-gated: counted outside
    // attempt, warned, not published, no sequence consumed. The counter is the only evidence.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.edge_published);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemCleaverPickup)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemPropaneThrown)));
        auto s_Next = s_Rig.Frame(k_Reloading);
        CHECK(s_Next.outside_attempt == 2 && s_Next.outcomes_published == 0 && !s_Next.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped"}));
        CHECK(s_Rig.Adapter->PublishedCount() == 2);
        CHECK(s_Rig.Warnings.size() == 2 && s_Rig.Warnings[0].find("no open mission attempt") != std::string::npos);
        CHECK(s_Rig.Normalizer.GetCounters().normalized == 2); // normalized, then gated: the counters tell both
    }

    // An ungated contract event in the same drain as an outside-attempt item event still
    // publishes (B2 behaviour unchanged beside the new rows).
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemRatPoisonPickup)));
        CHECK(s_Rig.Queue.Push(TestJson::ObservationFromRecordedEvent(
            B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractFailedExitB].json, 211)));
        auto s_After = s_Rig.Frame(k_Reloading);
        CHECK(s_After.outside_attempt == 1 && s_After.ungated_published == 1 && s_After.outcomes_published == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "contract.ended"}));
    }

    // The whole B0 item corpus through the frame inside one attempt: 24 publications, 12/6/6 by
    // type, contiguous, no warnings; counts are direct per-name counts and nothing is paired.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        for (size_t i = 0; i < B0Fixtures::k_ItemCount; ++i)
            CHECK(s_Rig.Queue.Push(Item(i)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 24 && s_Frame.malformed == 0 && s_Frame.outside_attempt == 0);
        std::map<std::string, size_t> s_ByType;
        for (const auto& s_Type : s_Rig.Types())
            ++s_ByType[s_Type];
        CHECK(s_ByType["item.picked_up"] == 12 && s_ByType["item.thrown"] == 6 && s_ByType["item.removed_from_inventory"] == 6);
        CHECK(s_Rig.Contiguous() && s_Rig.Sink->Published.size() == 25 && s_Rig.Warnings.empty());
    }
}
