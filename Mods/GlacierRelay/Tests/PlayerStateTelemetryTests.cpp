#include "TestHarness.h"

#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "Fixtures/B0ContractLifecycle.h"
#include "Fixtures/B0Disguise.h"
#include "Fixtures/B0Items.h"
#include "Fixtures/B0Objectives.h"
#include "Fixtures/B0PlayerState.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B6: Trespassing -> player.trespassing v1 and HoldingIllegalWeapon -> player.illegal_weapon v1,
// normalized from the six recorded B0 payloads (design section 46.1) and synthetic variants; the
// field rules of 46.3; the per-field malformed diagnostic of 46.5; attempt-gated publication (46.4);
// no deduplication; the other rows unchanged beside them.
//
// Coverage boundary (design section 46.7), true of every case here: observations are parsed by
// TestJson or built by hand, so these tests exercise the normalizer, serialization, adapter, frame
// and sink — downstream consumers — and never TelemetryIntake::Copy, its bool, number or nested
// object branches, or Inspect. Parsing the fixture's envelope is TestJson's work. The engine field
// types of IsTrespassing, RoomId, IsHoldingIllegalWeapon, WeaponEquipped and its item fields are
// not covered here; a successful normalization establishes compatibility with the Relay contract,
// not those types.
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Player(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_PlayerState[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Actor(size_t p_Index)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[p_Index].json, static_cast<uint32_t>(p_Index + 1));
    }

    TelemetryObservation Item(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Items[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Disguise(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Disguise[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Objective()
    {
        const auto& s_Raw = B0Fixtures::k_Objectives[0];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Contract(size_t p_Index, uint32_t p_EventIndex)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ContractLifecycle[p_Index].json, p_EventIndex);
    }

    std::string Mutate(std::string p_Json, const std::string& p_From, const std::string& p_To)
    {
        const auto s_Pos = p_Json.find(p_From);
        CHECK(s_Pos != std::string::npos);
        return p_Json.replace(s_Pos, p_From.size(), p_To);
    }

    TelemetryValue WithFieldIn(TelemetryValue p_Object, std::string_view p_Key, TelemetryValue p_Value)
    {
        for (auto& s_Field : p_Object.fields)
            if (s_Field.first == p_Key)
            {
                s_Field.second = std::move(p_Value);
                return p_Object;
            }

        p_Object.fields.emplace_back(std::string(p_Key), std::move(p_Value));
        return p_Object;
    }

    TelemetryValue WithoutFieldIn(TelemetryValue p_Object, std::string_view p_Key)
    {
        auto& s_Fields = p_Object.fields;
        for (auto it = s_Fields.begin(); it != s_Fields.end(); ++it)
            if (it->first == p_Key)
            {
                s_Fields.erase(it);
                return p_Object;
            }

        CHECK(false);
        return p_Object;
    }

    TelemetryObservation WithField(TelemetryObservation p_Obs, std::string_view p_Key, TelemetryValue p_Value)
    {
        p_Obs.value = WithFieldIn(std::move(p_Obs.value), p_Key, std::move(p_Value));
        return p_Obs;
    }

    TelemetryObservation WithoutField(TelemetryObservation p_Obs, std::string_view p_Key)
    {
        p_Obs.value = WithoutFieldIn(std::move(p_Obs.value), p_Key);
        return p_Obs;
    }

    // The nested WeaponEquipped object with one of its fields replaced or removed.
    TelemetryObservation WithWeaponField(TelemetryObservation p_Obs, std::string_view p_Key, TelemetryValue p_Value)
    {
        const auto* s_Weapon = p_Obs.value.Find("WeaponEquipped");
        CHECK(s_Weapon && s_Weapon->kind == TelemetryValue::Kind::Object);
        return WithField(p_Obs, "WeaponEquipped", WithFieldIn(*s_Weapon, p_Key, std::move(p_Value)));
    }

    TelemetryObservation WithoutWeaponField(TelemetryObservation p_Obs, std::string_view p_Key)
    {
        const auto* s_Weapon = p_Obs.value.Find("WeaponEquipped");
        CHECK(s_Weapon && s_Weapon->kind == TelemetryValue::Kind::Object);
        return WithField(p_Obs, "WeaponEquipped", WithoutFieldIn(*s_Weapon, p_Key));
    }

    TelemetryValue Unsupported(const char* p_TypeName)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::Unsupported;
        s_Value.text = p_TypeName;
        return s_Value;
    }

    TelemetryValue Str(const char* p_Text)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::String;
        s_Value.text = p_Text;
        return s_Value;
    }

    TelemetryValue Num(double p_Number)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::Number;
        s_Value.number = p_Number;
        return s_Value;
    }

    TelemetryValue Bool(bool p_Bool)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::Bool;
        s_Value.boolean = p_Bool;
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

    constexpr const char* k_TrespassingEnteredPayload =
        "{\"source\":\"engine_telemetry\",\"engine_event\":\"Trespassing\",\"trespassing\":true,\"room_id\":6,"
        "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":166.198425}";
    constexpr const char* k_TrespassingLeftPayload =
        "{\"source\":\"engine_telemetry\",\"engine_event\":\"Trespassing\",\"trespassing\":false,\"room_id\":3,"
        "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":202.179184}";
    constexpr const char* k_WeaponJson =
        "\"weapon\":{\"item_repository_id\":\"e70adb5b-0646-4f88-bd4a-85bea7a2a654\",\"item_instance_id\":\"9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955\","
        "\"item_name\":\"ICA19 Silverballer\",\"item_type\":\"Gun_HardBaller_01\",\"online_traits\":[\"pistol\"],\"perceived_as_weapon\":true}";
    const std::string k_Drawn1Payload =
        std::string("{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":true,") + k_WeaponJson +
        ",\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":476.573425}";
    constexpr const char* k_Holstered1Payload =
        "{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":false,"
        "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":477.59137}";
    const std::string k_Drawn2Payload =
        std::string("{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":true,") + k_WeaponJson +
        ",\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":485.492706}";
    constexpr const char* k_Holstered2Payload =
        "{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":false,"
        "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":491.880981}";

    // The per-field listings of the recorded shapes, as every rejection detail prints them.
    constexpr const char* k_TrespassingListing = "fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=Number";
    constexpr const char* k_DrawnListing =
        "fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Object fields=8; weapon fields: 'RepositoryId' kind=String bytes=36, "
        "'InstanceId' kind=String bytes=36, 'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, 'OnlineTraits' kind=Array items=1, "
        "'IsPerceivedAsWeapon' kind=Bool";
    constexpr const char* k_HolsteredListing = "fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' absent";
}

void RunPlayerStateTelemetryTests()
{
    // Table: two supported names, attempt-gated; neighbours that are not normalized (research
    // candidates, detection, frisk and rating names stay counted by name).
    CHECK(TelemetryNormalizer::IsSupportedSourceName("Trespassing"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("HoldingIllegalWeapon"));
    for (const char* s_Name : {"Hero_Health", "Hero_Dead", "Spotted", "Witnesses", "BeingFrisked", "FriskedSuccess",
                               "SecuritySystemRecorder", "Investigate_Curious", "trespassing", "IsTrespassing", "IllegalWeapon"})
        CHECK(!TelemetryNormalizer::IsSupportedSourceName(s_Name));

    // The six recorded payloads: every field verbatim; room_id as the copied number; the weapon
    // object on the two true forms (with the non-empty instance id); absent on the two false forms.
    {
        TelemetryNormalizer s_Normalizer;

        const auto s_Entered = s_Normalizer.Normalize(Player(B0Fixtures::k_TrespassingEntered));
        CHECK(s_Entered.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Entered.gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Entered.trespassing.has_value() && !s_Entered.illegal_weapon);
        CHECK(!s_Entered.event && !s_Entered.disguise && !s_Entered.item && !s_Entered.objective && !s_Entered.contract_started && !s_Entered.contract_ended);
        {
            const auto& e = *s_Entered.trespassing;
            CHECK(e.engine_event == "Trespassing" && e.trespassing == true);
            CHECK(e.room_id.has_value() && *e.room_id == 6.0);
            CHECK(e.contract_session_id && *e.contract_session_id == k_SessionA);
            CHECK(e.engine_timestamp_s && *e.engine_timestamp_s == 166.198425);
            CHECK(RelaySerialization::PlayerTrespassingPayloadJson(e) == k_TrespassingEnteredPayload);
        }

        const auto s_Left = s_Normalizer.Normalize(Player(B0Fixtures::k_TrespassingLeft));
        CHECK(s_Left.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Left.trespassing->trespassing == false && *s_Left.trespassing->room_id == 3.0);
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_Left.trespassing) == k_TrespassingLeftPayload);

        const auto s_Drawn1 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn1));
        CHECK(s_Drawn1.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Drawn1.gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Drawn1.illegal_weapon.has_value() && !s_Drawn1.trespassing);
        {
            const auto& e = *s_Drawn1.illegal_weapon;
            CHECK(e.engine_event == "HoldingIllegalWeapon" && e.holding == true);
            CHECK(e.weapon.has_value());
            CHECK(e.weapon->item_repository_id == B0Fixtures::k_IllegalWeaponDefinition);
            CHECK(e.weapon->item_instance_id && *e.weapon->item_instance_id == B0Fixtures::k_IllegalWeaponInstance);
            CHECK(e.weapon->item_name && *e.weapon->item_name == "ICA19 Silverballer");
            CHECK(e.weapon->item_type && *e.weapon->item_type == "Gun_HardBaller_01");
            CHECK(e.weapon->online_traits && *e.weapon->online_traits == std::vector<std::string>{"pistol"});
            CHECK(e.weapon->perceived_as_weapon && *e.weapon->perceived_as_weapon == true);
            CHECK(e.contract_session_id && *e.contract_session_id == k_SessionA);
            CHECK(e.engine_timestamp_s && *e.engine_timestamp_s == 476.573425);
            CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(e) == k_Drawn1Payload);
        }

        const auto s_Holstered1 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponHolstered1));
        CHECK(s_Holstered1.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Holstered1.illegal_weapon->holding == false && !s_Holstered1.illegal_weapon->weapon.has_value());
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_Holstered1.illegal_weapon) == k_Holstered1Payload);

        const auto s_Drawn2 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn2));
        CHECK(s_Drawn2.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_Drawn2.illegal_weapon) == k_Drawn2Payload);
        CHECK(s_Drawn2.illegal_weapon->weapon == s_Drawn1.illegal_weapon->weapon); // the same object twice: two occurrences

        const auto s_Holstered2 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponHolstered2));
        CHECK(s_Holstered2.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_Holstered2.illegal_weapon) == k_Holstered2Payload);

        CHECK(s_Normalizer.GetCounters().normalized == 6 && s_Normalizer.GetCounters().malformed == 0);
    }

    // Optional room_id and the finite-number rule: absent is fine; a non-integral or negative finite
    // number is carried as copied; NaN and infinities are malformed. Nothing interprets the value.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Base = Player(B0Fixtures::k_TrespassingEntered);

        auto s_Minimal = s_Base;
        s_Minimal.value = TestJson::Parse(R"({"IsTrespassing":true})");
        const auto s_MinimalResult = s_Normalizer.Normalize(s_Minimal);
        CHECK(s_MinimalResult.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_MinimalResult.trespassing->room_id.has_value());
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_MinimalResult.trespassing).find("room_id") == std::string::npos);

        const auto s_Half = s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(6.5)));
        CHECK(s_Half.outcome == TelemetryNormalizer::Outcome::Normalized && *s_Half.trespassing->room_id == 6.5);
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_Half.trespassing).find("\"room_id\":6.5,") != std::string::npos);

        const auto s_Negative = s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(-1)));
        CHECK(s_Negative.outcome == TelemetryNormalizer::Outcome::Normalized && *s_Negative.trespassing->room_id == -1.0);
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_Negative.trespassing).find("\"room_id\":-1,") != std::string::npos);

        const auto s_Zero = s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(0)));
        CHECK(s_Zero.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_Zero.trespassing).find("\"room_id\":0,") != std::string::npos);

        CHECK(s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(std::numeric_limits<double>::quiet_NaN()))).detail
              == std::string("field 'RoomId' is not a finite number; ") + k_TrespassingListing);
        CHECK(s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(std::numeric_limits<double>::infinity()))).detail
              == std::string("field 'RoomId' is not a finite number; ") + k_TrespassingListing);
        CHECK(s_Normalizer.Normalize(WithField(s_Base, "RoomId", Num(-std::numeric_limits<double>::infinity()))).outcome
              == TelemetryNormalizer::Outcome::Malformed);
        CHECK(s_Normalizer.GetCounters().malformed == 3 && s_Normalizer.GetCounters().normalized == 4);

        // Bare events: the subject alone.
        PlayerTrespassingEvent s_Bare;
        s_Bare.engine_event = "Trespassing";
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"Trespassing\",\"trespassing\":false}");
    }

    // The weapon object: a true without WeaponEquipped is valid (the omission is evidence); a false
    // with a well-typed WeaponEquipped is valid and the object is preserved, not reinterpreted; a
    // minimal {RepositoryId} object; an empty InstanceId is dropped as in B4; perceived false is
    // written; unread nested fields of any kind are harmless.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Drawn = Player(B0Fixtures::k_IllegalWeaponDrawn1);
        const auto s_Holstered = Player(B0Fixtures::k_IllegalWeaponHolstered1);

        const auto s_TrueNoWeapon = s_Normalizer.Normalize(WithoutField(s_Drawn, "WeaponEquipped"));
        CHECK(s_TrueNoWeapon.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_TrueNoWeapon.illegal_weapon->holding == true && !s_TrueNoWeapon.illegal_weapon->weapon.has_value());
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_TrueNoWeapon.illegal_weapon) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":true,"
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":476.573425}");

        const auto s_FalseWithWeapon = s_Normalizer.Normalize(WithField(s_Holstered, "WeaponEquipped", *s_Drawn.value.Find("WeaponEquipped")));
        CHECK(s_FalseWithWeapon.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_FalseWithWeapon.illegal_weapon->holding == false && s_FalseWithWeapon.illegal_weapon->weapon.has_value());
        CHECK(s_FalseWithWeapon.illegal_weapon->weapon->item_repository_id == B0Fixtures::k_IllegalWeaponDefinition);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_FalseWithWeapon.illegal_weapon) ==
            std::string("{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":false,") + k_WeaponJson +
            ",\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":477.59137}");

        auto s_Minimal = s_Drawn;
        s_Minimal.value = TestJson::Parse(R"({"IsHoldingIllegalWeapon":true,"WeaponEquipped":{"RepositoryId":"x"}})");
        const auto s_MinimalResult = s_Normalizer.Normalize(s_Minimal);
        CHECK(s_MinimalResult.outcome == TelemetryNormalizer::Outcome::Normalized);
        {
            const auto& w = *s_MinimalResult.illegal_weapon->weapon;
            CHECK(w.item_repository_id == "x" && !w.item_instance_id && !w.item_name && !w.item_type && !w.online_traits && !w.perceived_as_weapon);
        }
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_MinimalResult.illegal_weapon).find(
            "\"holding\":true,\"weapon\":{\"item_repository_id\":\"x\"},\"contract_session_id\"") != std::string::npos);

        const auto s_EmptyInstance = s_Normalizer.Normalize(WithWeaponField(s_Drawn, "InstanceId", Str("")));
        CHECK(s_EmptyInstance.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_EmptyInstance.illegal_weapon->weapon->item_instance_id.has_value());
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_EmptyInstance.illegal_weapon).find("item_instance_id") == std::string::npos);

        const auto s_NotPerceived = s_Normalizer.Normalize(WithWeaponField(s_Drawn, "IsPerceivedAsWeapon", Bool(false)));
        CHECK(s_NotPerceived.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_NotPerceived.illegal_weapon->weapon->perceived_as_weapon.has_value() && *s_NotPerceived.illegal_weapon->weapon->perceived_as_weapon == false);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_NotPerceived.illegal_weapon).find("\"perceived_as_weapon\":false}") != std::string::npos);

        const auto s_EmptyName = s_Normalizer.Normalize(WithWeaponField(s_Drawn, "ItemName", Str("")));
        CHECK(s_EmptyName.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_EmptyName.illegal_weapon).find("\"item_name\":\"\"") != std::string::npos);

        const auto s_NoTraits = s_Normalizer.Normalize(WithWeaponField(s_Drawn, "OnlineTraits", TestJson::Parse("[]")));
        CHECK(s_NoTraits.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_NoTraits.illegal_weapon->weapon->online_traits.has_value() && s_NoTraits.illegal_weapon->weapon->online_traits->empty());

        auto s_Extra = WithWeaponField(s_Drawn, "ActionRewardType", Unsupported("eActionRewardType"));
        s_Extra = WithWeaponField(s_Extra, "Category", Unsupported("eItemCategory"));
        s_Extra = WithWeaponField(s_Extra, "SpawnLocation", TestJson::Parse(R"({"x":1})"));
        s_Extra = WithField(s_Extra, "Reason", Unsupported("EIllegalWeaponReason"));
        CHECK(s_Normalizer.Normalize(s_Extra).outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Normalizer.GetCounters().malformed == 0);

        // Bare event: the subject alone, false, no weapon.
        PlayerIllegalWeaponEvent s_Bare;
        s_Bare.engine_event = "HoldingIllegalWeapon";
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":false}");
    }

    // The adapter: type and schema per row; the sequence is the one shared stream across every
    // family (B1 through B6 in one adapter).
    {
        Rig s_Rig;
        TelemetryNormalizer s_Normalizer;
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Player(B0Fixtures::k_TrespassingEntered)).trespassing);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn1)).illegal_weapon);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Actor(13)).event);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractStartA, 2)).contract_started);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseChange1)).disguise);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).item);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Objective()).objective);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponHolstered1)).illegal_weapon);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "player.trespassing", "player.illegal_weapon", "actor.died", "contract.started", "disguise.equipped", "item.picked_up",
            "objective.completed", "player.illegal_weapon"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Adapter->PublishedCount() == 8);
        CHECK(s_Rig.Sink->Published[0].json.find("\"event_type\":\"player.trespassing\",\"schema_version\":1,\"payload\":{\"source\":\"engine_telemetry\",\"engine_event\":\"Trespassing\"") != std::string::npos);
        CHECK(s_Rig.Sink->Published[1].json.find("\"event_type\":\"player.illegal_weapon\",\"schema_version\":1,\"payload\":{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\"") != std::string::npos);
    }

    // Malformed Trespassing: Value not an object; the subject missing/not a bool (an Unsupported
    // copy reports the engine type name); RoomId of the wrong kind — each with the per-field
    // detail. Counted per name, nothing published.
    {
        TelemetryNormalizer s_Normalizer;
        size_t s_Expected = 0;

        auto s_Case = [&](TelemetryObservation p_Obs) {
            const auto s_Result = s_Normalizer.Normalize(p_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.trespassing.has_value() && !s_Result.illegal_weapon.has_value());
            ++s_Expected;
            return s_Result.detail;
        };

        const auto s_Ok = Player(B0Fixtures::k_TrespassingEntered);

        auto s_String = s_Ok;
        s_String.value = Str("true");
        CHECK(s_Case(s_String) == "Value is not an object (kind=String bytes=4)");

        auto s_Bool = s_Ok;
        s_Bool.value = Bool(true); // the subject alone, not wrapped in the object
        CHECK(s_Case(s_Bool) == "Value is not an object (kind=Bool)");

        auto s_Array = s_Ok;
        s_Array.value = TestJson::Parse(R"([{"IsTrespassing":true}])");
        CHECK(s_Case(s_Array) == "Value is not an object (kind=Array items=1)");

        auto s_Null = s_Ok;
        s_Null.value = TelemetryValue{};
        CHECK(s_Case(s_Null) == "Value is not an object (kind=Null)");

        auto s_Unsupported = s_Ok;
        s_Unsupported.value = Unsupported("STrespassingTelemetry");
        CHECK(s_Case(s_Unsupported) == "Value is not an object (kind=Unsupported type='STrespassingTelemetry')");

        const auto s_Absent = TestJson::ObservationFromRecordedEvent(Mutate(
            B0Fixtures::k_PlayerState[B0Fixtures::k_TrespassingEntered].json, R"("Value":{"IsTrespassing":true,"RoomId":6.000000},)", ""));
        CHECK(s_Case(s_Absent) == "Value is not an object (kind=Null)");

        CHECK(s_Case(WithoutField(s_Ok, "IsTrespassing")) == "missing field 'IsTrespassing'; fields: 'IsTrespassing' absent, 'RoomId' kind=Number");
        CHECK(s_Case(WithField(s_Ok, "IsTrespassing", Str("true"))) ==
            "field 'IsTrespassing' is not a bool (kind=String bytes=4); fields: 'IsTrespassing' kind=String bytes=4, 'RoomId' kind=Number");
        CHECK(s_Case(WithField(s_Ok, "IsTrespassing", Num(1))) ==
            "field 'IsTrespassing' is not a bool (kind=Number); fields: 'IsTrespassing' kind=Number, 'RoomId' kind=Number");
        CHECK(s_Case(WithField(s_Ok, "IsTrespassing", TestJson::Parse("null"))) ==
            "field 'IsTrespassing' is not a bool (kind=Null); fields: 'IsTrespassing' kind=Null, 'RoomId' kind=Number");
        CHECK(s_Case(WithField(s_Ok, "IsTrespassing", Unsupported("ZTrespassingSituation_ESituationState"))) ==
            "field 'IsTrespassing' is not a bool (kind=Unsupported type='ZTrespassingSituation_ESituationState'); "
            "fields: 'IsTrespassing' kind=Unsupported type='ZTrespassingSituation_ESituationState', 'RoomId' kind=Number");
        CHECK(s_Case(WithField(s_Ok, "RoomId", Str("6"))) ==
            "field 'RoomId' is not a number (kind=String bytes=1); fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=String bytes=1");
        CHECK(s_Case(WithField(s_Ok, "RoomId", Bool(true))) ==
            "field 'RoomId' is not a number (kind=Bool); fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=Bool");
        CHECK(s_Case(WithField(s_Ok, "RoomId", TestJson::Parse("null"))) ==
            "field 'RoomId' is not a number (kind=Null); fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=Null");
        CHECK(s_Case(WithField(s_Ok, "RoomId", TestJson::Parse(R"({"id":6})"))) ==
            "field 'RoomId' is not a number (kind=Object fields=1); fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=Object fields=1");
        CHECK(s_Case(WithField(s_Ok, "RoomId", Unsupported("ZEntityRef"))) ==
            "field 'RoomId' is not a number (kind=Unsupported type='ZEntityRef'); fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=Unsupported type='ZEntityRef'");

        // The first failing field wins; the listing still shows every expected key.
        auto s_TwoBad = WithField(s_Ok, "IsTrespassing", Num(1));
        s_TwoBad = WithField(s_TwoBad, "RoomId", Str("6"));
        CHECK(s_Case(s_TwoBad) == "field 'IsTrespassing' is not a bool (kind=Number); fields: 'IsTrespassing' kind=Number, 'RoomId' kind=String bytes=1");

        // Escaping and bounding of a type name in the listing.
        TelemetryValue s_Bad;
        s_Bad.kind = TelemetryValue::Kind::Unsupported;
        s_Bad.text = std::string("e'\\\n") + "\xC3\xA9" + std::string(70, 'T');
        const auto s_Detail = s_Case(WithField(s_Ok, "RoomId", s_Bad));
        CHECK(s_Detail.find("type='e\\x27\\x5C\\x0A\\xC3\\xA9" + std::string(58, 'T') + "...'") != std::string::npos);
        CHECK(s_Detail.find('\n') == std::string::npos && s_Detail.find('\xC3') == std::string::npos);

        CHECK(s_Normalizer.GetCounters().malformed == s_Expected);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("Trespassing") == s_Expected);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
        CHECK(s_Normalizer.Normalize(s_Ok).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // Malformed HoldingIllegalWeapon: Value not an object; the subject missing/not a bool;
    // WeaponEquipped present but not an object; a nested field of the wrong kind (with the nested
    // listing); the nested definition id missing/empty/non-string; unknown nested kinds reported by
    // type name. Counted per name, nothing published.
    {
        TelemetryNormalizer s_Normalizer;
        size_t s_Expected = 0;

        auto s_Case = [&](TelemetryObservation p_Obs) {
            const auto s_Result = s_Normalizer.Normalize(p_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.illegal_weapon.has_value() && !s_Result.trespassing.has_value());
            ++s_Expected;
            return s_Result.detail;
        };

        const auto s_Drawn = Player(B0Fixtures::k_IllegalWeaponDrawn1);
        const auto s_Holstered = Player(B0Fixtures::k_IllegalWeaponHolstered1);

        auto s_Bool = s_Drawn;
        s_Bool.value = Bool(true);
        CHECK(s_Case(s_Bool) == "Value is not an object (kind=Bool)");

        auto s_Array = s_Drawn;
        s_Array.value = TestJson::Parse(R"([{"IsHoldingIllegalWeapon":true}])");
        CHECK(s_Case(s_Array) == "Value is not an object (kind=Array items=1)");

        auto s_Null = s_Holstered;
        s_Null.value = TelemetryValue{};
        CHECK(s_Case(s_Null) == "Value is not an object (kind=Null)");

        auto s_Unsupported = s_Drawn;
        s_Unsupported.value = Unsupported("SIllegalWeaponTelemetry");
        CHECK(s_Case(s_Unsupported) == "Value is not an object (kind=Unsupported type='SIllegalWeaponTelemetry')");

        // The subject, on both recorded forms.
        CHECK(s_Case(WithoutField(s_Holstered, "IsHoldingIllegalWeapon")) ==
            "missing field 'IsHoldingIllegalWeapon'; fields: 'IsHoldingIllegalWeapon' absent, 'WeaponEquipped' absent");
        CHECK(s_Case(WithoutField(s_Drawn, "IsHoldingIllegalWeapon")) ==
            std::string("missing field 'IsHoldingIllegalWeapon'; fields: 'IsHoldingIllegalWeapon' absent, 'WeaponEquipped' kind=Object fields=8; ")
            + "weapon fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=36, 'ItemName' kind=String bytes=18, "
              "'ItemType' kind=String bytes=17, 'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=Bool");
        CHECK(s_Case(WithField(s_Holstered, "IsHoldingIllegalWeapon", Str("false"))) ==
            "field 'IsHoldingIllegalWeapon' is not a bool (kind=String bytes=5); fields: 'IsHoldingIllegalWeapon' kind=String bytes=5, 'WeaponEquipped' absent");
        CHECK(s_Case(WithField(s_Holstered, "IsHoldingIllegalWeapon", Num(0))) ==
            "field 'IsHoldingIllegalWeapon' is not a bool (kind=Number); fields: 'IsHoldingIllegalWeapon' kind=Number, 'WeaponEquipped' absent");
        CHECK(s_Case(WithField(s_Drawn, "IsHoldingIllegalWeapon", Unsupported("bool"))) ==
            std::string("field 'IsHoldingIllegalWeapon' is not a bool (kind=Unsupported type='bool'); fields: 'IsHoldingIllegalWeapon' kind=Unsupported type='bool', ")
            + "'WeaponEquipped' kind=Object fields=8; weapon fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=36, "
              "'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, 'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=Bool");

        // WeaponEquipped present but not an object (no nested listing: there is nothing to list).
        CHECK(s_Case(WithField(s_Drawn, "WeaponEquipped", Str("e70adb5b-0646-4f88-bd4a-85bea7a2a654"))) ==
            "field 'WeaponEquipped' is not an object (kind=String bytes=36); fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=String bytes=36");
        CHECK(s_Case(WithField(s_Drawn, "WeaponEquipped", TestJson::Parse(R"([{"RepositoryId":"x"}])"))) ==
            "field 'WeaponEquipped' is not an object (kind=Array items=1); fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Array items=1");
        CHECK(s_Case(WithField(s_Holstered, "WeaponEquipped", TestJson::Parse("null"))) ==
            "field 'WeaponEquipped' is not an object (kind=Null); fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Null");
        CHECK(s_Case(WithField(s_Drawn, "WeaponEquipped", Num(7))) ==
            "field 'WeaponEquipped' is not an object (kind=Number); fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Number");
        CHECK(s_Case(WithField(s_Drawn, "WeaponEquipped", Unsupported("ZHM5ItemWeapon"))) ==
            "field 'WeaponEquipped' is not an object (kind=Unsupported type='ZHM5ItemWeapon'); fields: 'IsHoldingIllegalWeapon' kind=Bool, "
            "'WeaponEquipped' kind=Unsupported type='ZHM5ItemWeapon'");

        // The nested subject.
        CHECK(s_Case(WithoutWeaponField(s_Drawn, "RepositoryId")) ==
            "missing field 'WeaponEquipped.RepositoryId'; fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Object fields=7; "
            "weapon fields: 'RepositoryId' absent, 'InstanceId' kind=String bytes=36, 'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, "
            "'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=Bool");
        CHECK(s_Case(WithWeaponField(s_Drawn, "RepositoryId", Str(""))) ==
            "field 'WeaponEquipped.RepositoryId' is empty; fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Object fields=8; "
            "weapon fields: 'RepositoryId' kind=String bytes=0, 'InstanceId' kind=String bytes=36, 'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, "
            "'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=Bool");
        CHECK(s_Case(WithWeaponField(s_Drawn, "RepositoryId", Unsupported("ZRepositoryID"))).rfind(
            "field 'WeaponEquipped.RepositoryId' is not a string (kind=Unsupported type='ZRepositoryID'); fields: 'IsHoldingIllegalWeapon' kind=Bool, "
            "'WeaponEquipped' kind=Object fields=8; weapon fields: 'RepositoryId' kind=Unsupported type='ZRepositoryID', ", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "RepositoryId", Num(7))).rfind("field 'WeaponEquipped.RepositoryId' is not a string (kind=Number); fields:", 0) == 0);

        // Nested optional fields of the wrong kind, including unknown/unsupported kinds.
        CHECK(s_Case(WithWeaponField(s_Drawn, "ItemName", Num(1))) ==
            "WeaponEquipped: field 'ItemName' is not a string (kind=Number); fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Object fields=8; "
            "weapon fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=36, 'ItemName' kind=Number, 'ItemType' kind=String bytes=17, "
            "'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=Bool");
        CHECK(s_Case(WithWeaponField(s_Drawn, "ItemType", Unsupported("eItemType"))).rfind(
            "WeaponEquipped: field 'ItemType' is not a string (kind=Unsupported type='eItemType'); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "InstanceId", TestJson::Parse("null"))).rfind(
            "WeaponEquipped: field 'InstanceId' is not a string (kind=Null); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "InstanceId", TestJson::Parse(R"({"a":1})"))).rfind(
            "WeaponEquipped: field 'InstanceId' is not a string (kind=Object fields=1); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "OnlineTraits", Str("pistol"))).rfind(
            "WeaponEquipped: field 'OnlineTraits' is not an array (kind=String bytes=6); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "OnlineTraits", TestJson::Parse(R"(["pistol",7])"))).rfind(
            "WeaponEquipped: field 'OnlineTraits' has a non-string item (item 1 kind=Number); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "OnlineTraits", Unsupported("TArray<eOnlineTrait>"))).rfind(
            "WeaponEquipped: field 'OnlineTraits' is not an array (kind=Unsupported type='TArray<eOnlineTrait>'); fields:", 0) == 0);
        CHECK(s_Case(WithWeaponField(s_Drawn, "IsPerceivedAsWeapon", Str("true"))) ==
            "WeaponEquipped: field 'IsPerceivedAsWeapon' is not a bool (kind=String bytes=4); fields: 'IsHoldingIllegalWeapon' kind=Bool, "
            "'WeaponEquipped' kind=Object fields=8; weapon fields: 'RepositoryId' kind=String bytes=36, 'InstanceId' kind=String bytes=36, "
            "'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, 'OnlineTraits' kind=Array items=1, 'IsPerceivedAsWeapon' kind=String bytes=4");
        CHECK(s_Case(WithWeaponField(s_Drawn, "IsPerceivedAsWeapon", Num(1))).rfind(
            "WeaponEquipped: field 'IsPerceivedAsWeapon' is not a bool (kind=Number); fields:", 0) == 0);

        // The first failing field wins, at either level.
        auto s_TwoBad = WithField(s_Drawn, "IsHoldingIllegalWeapon", Num(1));
        s_TwoBad = WithWeaponField(s_TwoBad, "ItemName", Num(2));
        CHECK(s_Case(s_TwoBad).rfind("field 'IsHoldingIllegalWeapon' is not a bool (kind=Number); fields:", 0) == 0);
        auto s_TwoNested = WithWeaponField(s_Drawn, "ItemName", Num(1));
        s_TwoNested = WithWeaponField(s_TwoNested, "IsPerceivedAsWeapon", Num(2));
        const auto s_TwoNestedDetail = s_Case(s_TwoNested);
        CHECK(s_TwoNestedDetail.rfind("WeaponEquipped: field 'ItemName' is not a string (kind=Number); fields:", 0) == 0);
        CHECK(s_TwoNestedDetail.find("'ItemName' kind=Number") != std::string::npos && s_TwoNestedDetail.find("'IsPerceivedAsWeapon' kind=Number") != std::string::npos);

        // Escaping and bounding of a nested type name.
        TelemetryValue s_Bad;
        s_Bad.kind = TelemetryValue::Kind::Unsupported;
        s_Bad.text = std::string("e'\\\n") + "\xC3\xA9" + std::string(70, 'T');
        const auto s_Detail = s_Case(WithWeaponField(s_Drawn, "ItemType", s_Bad));
        CHECK(s_Detail.find("type='e\\x27\\x5C\\x0A\\xC3\\xA9" + std::string(58, 'T') + "...'") != std::string::npos);
        CHECK(s_Detail.find('\n') == std::string::npos && s_Detail.find('\xC3') == std::string::npos);

        CHECK(s_Normalizer.GetCounters().malformed == s_Expected);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("HoldingIllegalWeapon") == s_Expected);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
        CHECK(s_Normalizer.Normalize(s_Drawn).outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Normalizer.Normalize(s_Holstered).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // The diagnostic reaches the durable log through the existing warning path with the event name
    // and index; a malformed occurrence consumes no sequence; the valid neighbours publish.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(WithField(Player(B0Fixtures::k_TrespassingEntered), "RoomId", Str("6"))));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingLeft)));
        CHECK(s_Rig.Queue.Push(WithWeaponField(Player(B0Fixtures::k_IllegalWeaponDrawn1), "RepositoryId", Str(""))));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered1)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 2 && s_Frame.outcomes_published == 2 && s_Frame.outside_attempt == 0);
        CHECK(s_Rig.Sink->Published.size() == 3 && s_Rig.Sink->Published[1].sequence == 2 && s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "player.trespassing", "player.illegal_weapon"}));
        CHECK(s_Rig.Warnings.size() == 2);
        CHECK(s_Rig.Warnings[0] == "telemetry 'Trespassing' (index 15) not normalized: field 'RoomId' is not a number (kind=String bytes=1); "
            "fields: 'IsTrespassing' kind=Bool, 'RoomId' kind=String bytes=1");
        CHECK(s_Rig.Warnings[1] == std::string("telemetry 'HoldingIllegalWeapon' (index 65) not normalized: field 'WeaponEquipped.RepositoryId' is empty; ")
            + "fields: 'IsHoldingIllegalWeapon' kind=Bool, 'WeaponEquipped' kind=Object fields=8; weapon fields: 'RepositoryId' kind=String bytes=0, "
              "'InstanceId' kind=String bytes=36, 'ItemName' kind=String bytes=18, 'ItemType' kind=String bytes=17, 'OnlineTraits' kind=Array items=1, "
              "'IsPerceivedAsWeapon' kind=Bool");
    }

    // Provenance optional; _DONTSEND on both names; repeated identical observations are distinct
    // occurrences (no deduplication of any kind).
    {
        TelemetryNormalizer s_Normalizer;

        auto s_Obs = Player(B0Fixtures::k_TrespassingLeft);
        s_Obs.contract_session_id.clear();
        s_Obs.has_timestamp = false;
        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_Result.trespassing->contract_session_id && !s_Result.trespassing->engine_timestamp_s);
        CHECK(RelaySerialization::PlayerTrespassingPayloadJson(*s_Result.trespassing) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"Trespassing\",\"trespassing\":false,\"room_id\":3}");

        auto s_Weapon = Player(B0Fixtures::k_IllegalWeaponDrawn2);
        s_Weapon.contract_session_id.clear();
        s_Weapon.has_timestamp = false;
        const auto s_WeaponResult = s_Normalizer.Normalize(s_Weapon);
        CHECK(s_WeaponResult.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_WeaponResult.illegal_weapon->contract_session_id && !s_WeaponResult.illegal_weapon->engine_timestamp_s);
        CHECK(RelaySerialization::PlayerIllegalWeaponPayloadJson(*s_WeaponResult.illegal_weapon) ==
            std::string("{\"source\":\"engine_telemetry\",\"engine_event\":\"HoldingIllegalWeapon\",\"holding\":true,") + k_WeaponJson + "}");

        for (size_t s_Index : {B0Fixtures::k_TrespassingEntered, B0Fixtures::k_IllegalWeaponDrawn1})
        {
            const auto& s_Raw = B0Fixtures::k_PlayerState[s_Index];
            const std::string s_Name = s_Index == B0Fixtures::k_TrespassingEntered ? "Trespassing" : "HoldingIllegalWeapon";
            auto s_Flagged = TestJson::ObservationFromRecordedEvent(Mutate(
                s_Raw.json, fmt::format(R"("Name":"{}",)", s_Name), fmt::format(R"("Name":"{}","_DONTSEND":true,)", s_Name)));
            CHECK(s_Flagged.dont_send);
            CHECK(s_Normalizer.Normalize(s_Flagged).outcome == TelemetryNormalizer::Outcome::DontSend);
        }
        CHECK(s_Normalizer.GetCounters().dont_send == 2);

        const auto s_First = s_Normalizer.Normalize(Player(B0Fixtures::k_TrespassingEntered));
        const auto s_Again = s_Normalizer.Normalize(Player(B0Fixtures::k_TrespassingEntered));
        CHECK(*s_First.trespassing == *s_Again.trespassing);
        const auto s_Drawn1 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn1));
        const auto s_DrawnAgain = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn1));
        const auto s_Drawn2 = s_Normalizer.Normalize(Player(B0Fixtures::k_IllegalWeaponDrawn2));
        CHECK(*s_Drawn1.illegal_weapon == *s_DrawnAgain.illegal_weapon);
        CHECK(s_Drawn2.illegal_weapon->weapon == s_Drawn1.illegal_weapon->weapon && *s_Drawn2.illegal_weapon->engine_timestamp_s != *s_Drawn1.illegal_weapon->engine_timestamp_s);
        CHECK(s_Normalizer.GetCounters().normalized == 7);
    }

    // Repeated identical observations through the frame: five identical Trespassing statements in
    // one drain are five publications with five sequences.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        for (int i = 0; i < 5; ++i)
            CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 5 && s_Frame.malformed == 0);
        CHECK(s_Rig.Sink->Published.size() == 6 && s_Rig.Contiguous());
        for (size_t i = 1; i < 6; ++i)
            CHECK(s_Rig.Sink->Published[i].json.find(k_TrespassingEnteredPayload) != std::string::npos);
    }

    // Unknown neighbouring names are counted by name and never published; the two rows beside
    // them are unaffected. (The intake filters these before queueing in production; here they
    // are pushed to show the normalizer's own answer.)
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        const std::string s_Base = B0Fixtures::k_PlayerState[B0Fixtures::k_TrespassingEntered].json;
        for (const char* s_Name : {"Hero_Health", "Hero_Dead", "Spotted", "BeingFrisked", "SecuritySystemRecorder"})
        {
            auto s_Obs = TestJson::ObservationFromRecordedEvent(Mutate(s_Base, R"("Name":"Trespassing")", fmt::format(R"("Name":"{}")", s_Name)));
            CHECK(s_Rig.Normalizer.Normalize(s_Obs).outcome == TelemetryNormalizer::Outcome::Unsupported);
            CHECK(s_Rig.Queue.Push(s_Obs));
        }
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered2)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 2 && s_Frame.malformed == 0 && s_Frame.outside_attempt == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "player.trespassing", "player.illegal_weapon"}));
        CHECK(s_Rig.Normalizer.GetCounters().unsupported == 10);
        CHECK(s_Rig.Normalizer.GetCounters().unsupported_by_name.at("Hero_Health") == 2);
        CHECK(s_Rig.Normalizer.GetCounters().unsupported_by_name.at("Hero_Dead") == 2);
        CHECK(s_Rig.Warnings.empty());
    }

    // The other families keep their classes beside the new rows.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(Actor(13)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseChange1)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractStartA, 2)).gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_Normalizer.Normalize(Objective()).gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_Normalizer.GetCounters().normalized == 5);
    }

    // Frame order, observed shape (B0 session 1): the trespass entry; the first Disguise with the
    // Trespassing false 25 ms later in one drain (two consecutive sequences, as captured, nothing
    // inferred between them); the four illegal-weapon statements as emitted. All inside the attempt.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_StartingSuitA)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange1)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingLeft)));
        auto s_Drain = s_Rig.Frame(k_Playing);
        CHECK(s_Drain.outcomes_published == 2 && s_Drain.outside_attempt == 0 && s_Drain.ungated_published == 0);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponDrawn1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponDrawn2)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered2)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "mission.playing", "disguise.equipped", "player.trespassing", "disguise.equipped", "player.trespassing",
            "player.illegal_weapon", "player.illegal_weapon", "player.illegal_weapon", "player.illegal_weapon"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Warnings.empty());
        CHECK(s_Rig.Sink->Published[3].sequence + 1 == s_Rig.Sink->Published[4].sequence);
    }

    // One monotonic adapter sequence across the B1–B6 vocabulary through the production frame
    // sequencing: contract start, rise, initial suit, trespass, item, kill with its objective,
    // illegal weapon drawn and holstered, contract end, fall. Contiguous, no warnings.
    {
        Rig s_Rig;
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractStartA, 2)));
        s_Rig.Frame(k_Reloading);
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_StartingSuitA)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemWrenchPickup1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Actor(13)));
        CHECK(s_Rig.Queue.Push(Objective()));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponDrawn1)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractFailedRestartA, 196)));
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "contract.started", "mission.playing", "disguise.equipped", "player.trespassing", "item.picked_up", "actor.died",
            "objective.completed", "player.illegal_weapon", "player.illegal_weapon", "contract.ended", "mission.stopped"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Adapter->PublishedCount() == 11 && s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (a): a player-state occurrence queued BEFORE the fall frame's drain is
    // judged against the playing state and publishes before mission.stopped.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered2)));
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.outcomes_published == 1 && s_Fall.outside_attempt == 0 && s_Fall.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "player.illegal_weapon", "mission.stopped"}));
        CHECK(s_Rig.Sink->Published[1].sequence == 2 && s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (b): the same observations presented AFTER that frame's drain arrive on
    // the next processed frame with no attempt open. Attempt-gated: counted outside attempt,
    // warned, not published, no sequence consumed — the chosen cost of the gate (46.4). An ungated
    // objective in the same drain still publishes.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.edge_published);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingLeft)));
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_IllegalWeaponHolstered2)));
        CHECK(s_Rig.Queue.Push(Objective()));
        auto s_Next = s_Rig.Frame(k_Reloading);
        CHECK(s_Next.outside_attempt == 2 && s_Next.outcomes_published == 0 && s_Next.ungated_published == 1 && !s_Next.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "objective.completed"}));
        CHECK(s_Rig.Adapter->PublishedCount() == 3);
        CHECK(s_Rig.Warnings.size() == 2);
        CHECK(s_Rig.Warnings[0] == "telemetry 'Trespassing' (index 19) observed with no open mission attempt; not published");
        CHECK(s_Rig.Warnings[1] == "telemetry 'HoldingIllegalWeapon' (index 73) observed with no open mission attempt; not published");
        CHECK(s_Rig.Normalizer.GetCounters().normalized == 3); // normalized, then gated: the counters tell both
    }

    // Before any rise: gated the same way; after a later rise the rows publish into the new attempt.
    {
        Rig s_Rig;
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        auto s_Before = s_Rig.Frame(k_Reloading);
        CHECK(s_Before.outside_attempt == 1 && s_Before.outcomes_published == 0);
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Player(B0Fixtures::k_TrespassingEntered)));
        auto s_After = s_Rig.Frame(k_Playing);
        CHECK(s_After.outside_attempt == 0 && s_After.outcomes_published == 1);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "player.trespassing"}));
        CHECK(s_Rig.Contiguous());
    }

    // The whole B0 player-state corpus through the frame inside one attempt: six publications,
    // 2/4 by type, contiguous, no warnings; nothing folded, paired or deduplicated natively.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        for (size_t i = 0; i < B0Fixtures::k_PlayerStateCount; ++i)
            CHECK(s_Rig.Queue.Push(Player(i)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.outcomes_published == 6 && s_Frame.malformed == 0 && s_Frame.outside_attempt == 0);
        std::map<std::string, size_t> s_ByType;
        for (const auto& s_Type : s_Rig.Types())
            ++s_ByType[s_Type];
        CHECK(s_ByType["player.trespassing"] == 2 && s_ByType["player.illegal_weapon"] == 4);
        CHECK(s_Rig.Contiguous() && s_Rig.Sink->Published.size() == 7 && s_Rig.Warnings.empty());
    }
}
