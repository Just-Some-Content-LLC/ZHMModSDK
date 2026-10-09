#include "TestHarness.h"

#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "Fixtures/B0ContractLifecycle.h"
#include "Fixtures/B0Disguise.h"
#include "Fixtures/B0Items.h"
#include "Fixtures/B0Objectives.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "RepositoryId.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B5: ObjectiveCompleted -> objective.completed v1, normalized from the one recorded B0 payload
// (design section 42.1) and synthetic variants; the field rules of 42.6; the per-field malformed
// diagnostic; UNGATED publication (42.4): the occurrence publishes wherever it drains relative to
// the mission predicate's edge, with its payload intact, and is never counted outside-attempt.
//
// Coverage boundary (design section 42.9), true of every case here: observations are parsed by
// TestJson or built by hand, so these tests exercise the normalizer, serialization, adapter, frame
// and sink — downstream consumers — and never TelemetryIntake::Copy, its bool branch or the
// Null/Unsupported classification. Parsing the fixture's "Name-first" envelope is TestJson's work
// and tests nothing about production Inspect (that Inspect reads this variant by key at runtime is
// evidenced by B2's live ContractFailed). The formatter-derived case exercises the shared
// RepositoryId renderer plus downstream; it does not establish that the engine uses ZRepositoryID
// for Value.Id. The engine field types of Id/Type/Category/ExcludeFromScoring are not covered here.
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Objective()
    {
        const auto& s_Raw = B0Fixtures::k_Objectives[B0Fixtures::k_ObjectiveNovikovKill];
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

    std::string Mutate(std::string p_Json, const std::string& p_From, const std::string& p_To)
    {
        const auto s_Pos = p_Json.find(p_From);
        CHECK(s_Pos != std::string::npos);
        return p_Json.replace(s_Pos, p_From.size(), p_To);
    }

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

    TelemetryValue Str(const char* p_Text)
    {
        TelemetryValue s_Value;
        s_Value.kind = TelemetryValue::Kind::String;
        s_Value.text = p_Text;
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
    constexpr const char* k_ObjectiveId = "aca8cd5b-e3a3-4a60-b953-c590484f0491";

    // k_ObjectiveId as a 16-byte little-endian GUID image (data1/data2/data3 LSB first, data4 in
    // order) — used only to exercise the shared renderer; whether the engine stores this id as a
    // ZRepositoryID is unknown.
    constexpr std::array<uint8_t, 16> k_ObjectiveIdImage = {
        0x5b, 0xcd, 0xa8, 0xac, 0xa3, 0xe3, 0x60, 0x4a, 0xb9, 0x53, 0xc5, 0x90, 0x48, 0x4f, 0x04, 0x91};

    constexpr const char* k_ExpectedPayload =
        "{\"source\":\"engine_telemetry\",\"engine_event\":\"ObjectiveCompleted\","
        "\"objective_id\":\"aca8cd5b-e3a3-4a60-b953-c590484f0491\",\"objective_type\":\"kill\",\"objective_category\":\"primary\","
        "\"exclude_from_scoring\":false,"
        "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\",\"engine_timestamp_s\":759.401611}";
}

void RunObjectiveTelemetryTests()
{
    // Table: one supported name, ungated; neighbours that are not normalized.
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ObjectiveCompleted"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ObjectiveFailed"));     // never captured; the pin exists
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ObjectiveUpdate"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ObjectiveActivate"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("OpportunityEvents"));   // not objectives
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("OpportunityStageEvent"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ChallengeCompleted"));  // _DONTSEND policy, and no row
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ContractEnd"));         // unobserved; B2's if it exists
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("objectivecompleted"));

    // The recorded payload: the Name-first envelope's keys are read by TestJson by name (fixture
    // parsing, not production Inspect); every field verbatim; false carried as false.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Obs = Objective();
        CHECK(s_Obs.name == "ObjectiveCompleted" && s_Obs.has_timestamp && s_Obs.contract_session_id == k_SessionA);
        CHECK(s_Obs.event_index == 163);

        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Result.gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_Result.objective.has_value());
        CHECK(!s_Result.event && !s_Result.disguise && !s_Result.item && !s_Result.contract_started && !s_Result.contract_ended);

        const auto& e = *s_Result.objective;
        CHECK(e.engine_event == "ObjectiveCompleted");
        CHECK(e.objective_id == k_ObjectiveId);
        CHECK(e.objective_type && *e.objective_type == "kill");
        CHECK(e.objective_category && *e.objective_category == "primary");
        CHECK(e.exclude_from_scoring.has_value() && *e.exclude_from_scoring == false);
        CHECK(e.contract_session_id && *e.contract_session_id == k_SessionA);
        CHECK(e.engine_timestamp_s && *e.engine_timestamp_s == 759.401611);
        CHECK(RelaySerialization::ObjectivePayloadJson(e) == k_ExpectedPayload);
        CHECK(s_Normalizer.GetCounters().normalized == 1 && s_Normalizer.GetCounters().malformed == 0);
    }

    // Wire shape: true written; absent optionals absent; a bare event.
    {
        TelemetryNormalizer s_Normalizer;
        TelemetryValue s_True;
        s_True.kind = TelemetryValue::Kind::Bool;
        s_True.boolean = true;
        const auto s_WithTrue = s_Normalizer.Normalize(WithField(Objective(), "ExcludeFromScoring", s_True));
        CHECK(s_WithTrue.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(RelaySerialization::ObjectivePayloadJson(*s_WithTrue.objective).find("\"exclude_from_scoring\":true") != std::string::npos);

        auto s_Minimal = Objective();
        s_Minimal.value = TestJson::Parse(R"({"Id":"x"})");
        const auto s_MinimalResult = s_Normalizer.Normalize(s_Minimal);
        CHECK(s_MinimalResult.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_MinimalResult.objective->objective_type && !s_MinimalResult.objective->objective_category
              && !s_MinimalResult.objective->exclude_from_scoring);
        const auto s_MinimalJson = RelaySerialization::ObjectivePayloadJson(*s_MinimalResult.objective);
        CHECK(s_MinimalJson.find("exclude_from_scoring") == std::string::npos && s_MinimalJson.find("objective_type") == std::string::npos);

        ObjectiveEvent s_Bare;
        s_Bare.engine_event = "ObjectiveCompleted";
        s_Bare.objective_id = "x";
        CHECK(RelaySerialization::ObjectivePayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ObjectiveCompleted\",\"objective_id\":\"x\"}");

        // Empty display strings are carried as sent (the grouping rule on BEAM treats them as no value).
        const auto s_EmptyType = s_Normalizer.Normalize(WithField(Objective(), "Type", Str("")));
        CHECK(s_EmptyType.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_EmptyType.objective->objective_type && s_EmptyType.objective->objective_type->empty());
        CHECK(RelaySerialization::ObjectivePayloadJson(*s_EmptyType.objective).find("\"objective_type\":\"\"") != std::string::npos);

        // Unread fields are harmless, whatever their kind.
        auto s_Extra = WithField(Objective(), "Progress", Unsupported("IContractObjective_SCounterData"));
        s_Extra = WithField(s_Extra, "State", TestJson::Parse("1"));
        CHECK(s_Normalizer.Normalize(s_Extra).outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Normalizer.GetCounters().malformed == 0);
    }

    // Formatter-derived id: the id built from its 16-byte image through the production renderer
    // equals the corpus string and normalizes to the same event and payload. Exercises the shared
    // RepositoryId formatter plus downstream; establishes nothing about the engine's type for Id.
    {
        TelemetryNormalizer s_Normalizer;
        const std::string s_Rendered = RepositoryId::FromLittleEndianBytes(k_ObjectiveIdImage).ToDashedLowercase();
        CHECK(s_Rendered == k_ObjectiveId);
        TelemetryValue s_Built;
        s_Built.kind = TelemetryValue::Kind::String;
        s_Built.text = s_Rendered;
        const auto s_FromImage = s_Normalizer.Normalize(WithField(Objective(), "Id", s_Built));
        const auto s_FromJson = s_Normalizer.Normalize(Objective());
        CHECK(s_FromImage.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(*s_FromImage.objective == *s_FromJson.objective);
        CHECK(RelaySerialization::ObjectivePayloadJson(*s_FromImage.objective) == k_ExpectedPayload);
    }

    // The adapter: type and schema; the sequence is the one shared stream.
    {
        Rig s_Rig;
        TelemetryNormalizer s_Normalizer;
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Objective()).objective);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).item);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"objective.completed", "item.picked_up"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Sink->Published[0].json.find("\"event_type\":\"objective.completed\",\"schema_version\":1,\"payload\":{\"source\":\"engine_telemetry\",\"engine_event\":\"ObjectiveCompleted\"") != std::string::npos);
    }

    // Malformed: Value not an object; Id missing/empty/non-string; optional fields of the wrong
    // kind — each with the per-field detail (copied kinds; Unsupported type names only; no string
    // values). Counted per name, nothing published.
    {
        TelemetryNormalizer s_Normalizer;
        size_t s_Expected = 0;

        auto s_Case = [&](TelemetryObservation p_Obs) {
            const auto s_Result = s_Normalizer.Normalize(p_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.objective.has_value());
            ++s_Expected;
            return s_Result.detail;
        };

        auto s_String = Objective();
        s_String.value = Str("aca8cd5b-e3a3-4a60-b953-c590484f0491"); // the disguise shape under this name
        CHECK(s_Case(s_String) == "Value is not an object (kind=String bytes=36)");

        auto s_Array = Objective();
        s_Array.value = TestJson::Parse(R"([{"Id":"x"}])");
        CHECK(s_Case(s_Array) == "Value is not an object (kind=Array items=1)");

        auto s_Null = Objective();
        s_Null.value = TelemetryValue{};
        CHECK(s_Case(s_Null) == "Value is not an object (kind=Null)");

        auto s_Unsupported = Objective();
        s_Unsupported.value = Unsupported("SObjectiveTelemetry");
        CHECK(s_Case(s_Unsupported) == "Value is not an object (kind=Unsupported type='SObjectiveTelemetry')");

        const std::string s_Base = B0Fixtures::k_Objectives[0].json;
        const auto s_Absent = TestJson::ObservationFromRecordedEvent(Mutate(s_Base,
            R"("Value":{"Id":"aca8cd5b-e3a3-4a60-b953-c590484f0491","Type":"kill","Category":"primary","ExcludeFromScoring":false},)", ""));
        CHECK(s_Case(s_Absent) == "Value is not an object (kind=Null)");

        const auto s_Ok = Objective();
        CHECK(s_Case(WithoutField(s_Ok, "Id")) ==
            "missing field 'Id'; fields: 'Id' absent, 'Type' kind=String bytes=4, 'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=Bool");
        CHECK(s_Case(WithField(s_Ok, "Id", Str(""))) ==
            "field 'Id' is empty; fields: 'Id' kind=String bytes=0, 'Type' kind=String bytes=4, 'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=Bool");
        CHECK(s_Case(WithField(s_Ok, "Id", Unsupported("ZRepositoryID"))) ==
            "field 'Id' is not a string (kind=Unsupported type='ZRepositoryID'); fields: 'Id' kind=Unsupported type='ZRepositoryID', "
            "'Type' kind=String bytes=4, 'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=Bool");
        CHECK(s_Case(WithField(s_Ok, "Id", TestJson::Parse("7"))).rfind("field 'Id' is not a string (kind=Number); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Ok, "Type", TestJson::Parse("0"))).rfind("field 'Type' is not a string (kind=Number); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Ok, "Category", Unsupported("IContractObjective_Category"))) ==
            "field 'Category' is not a string (kind=Unsupported type='IContractObjective_Category'); fields: 'Id' kind=String bytes=36, "
            "'Type' kind=String bytes=4, 'Category' kind=Unsupported type='IContractObjective_Category', 'ExcludeFromScoring' kind=Bool");
        CHECK(s_Case(WithField(s_Ok, "ExcludeFromScoring", Str("false"))) ==
            "field 'ExcludeFromScoring' is not a bool (kind=String bytes=5); fields: 'Id' kind=String bytes=36, 'Type' kind=String bytes=4, "
            "'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=String bytes=5");
        CHECK(s_Case(WithField(s_Ok, "ExcludeFromScoring", TestJson::Parse("null"))).rfind("field 'ExcludeFromScoring' is not a bool (kind=Null); fields:", 0) == 0);
        CHECK(s_Case(WithField(s_Ok, "ExcludeFromScoring", TestJson::Parse("0"))).rfind("field 'ExcludeFromScoring' is not a bool (kind=Number); fields:", 0) == 0);

        // The first failing field wins; the listing still shows every expected key.
        auto s_TwoBad = WithField(s_Ok, "Type", TestJson::Parse("1"));
        s_TwoBad = WithField(s_TwoBad, "ExcludeFromScoring", TestJson::Parse("2"));
        CHECK(s_Case(s_TwoBad) ==
            "field 'Type' is not a string (kind=Number); fields: 'Id' kind=String bytes=36, 'Type' kind=Number, 'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=Number");

        // Escaping and bounding of a type name in the listing.
        TelemetryValue s_Bad;
        s_Bad.kind = TelemetryValue::Kind::Unsupported;
        s_Bad.text = std::string("e'\\\n") + "\xC3\xA9" + std::string(70, 'T');
        const auto s_Detail = s_Case(WithField(s_Ok, "Category", s_Bad));
        CHECK(s_Detail.find("type='e\\x27\\x5C\\x0A\\xC3\\xA9" + std::string(58, 'T') + "...'") != std::string::npos);
        CHECK(s_Detail.find('\n') == std::string::npos);

        CHECK(s_Normalizer.GetCounters().malformed == s_Expected);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("ObjectiveCompleted") == s_Expected);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
        CHECK(s_Normalizer.Normalize(s_Ok).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // Provenance optional; _DONTSEND policy on this name; a repeated occurrence with the same Id is
    // two events; a client ChallengeCompleted beside it stays DontSend.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Obs = Objective();
        s_Obs.contract_session_id.clear();
        s_Obs.has_timestamp = false;
        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_Result.objective->contract_session_id && !s_Result.objective->engine_timestamp_s);

        auto s_Flagged = TestJson::ObservationFromRecordedEvent(Mutate(
            B0Fixtures::k_Objectives[0].json, R"("Name":"ObjectiveCompleted",)", R"("Name":"ObjectiveCompleted","_DONTSEND":true,)"));
        CHECK(s_Flagged.dont_send);
        CHECK(s_Normalizer.Normalize(s_Flagged).outcome == TelemetryNormalizer::Outcome::DontSend);

        const auto s_First = s_Normalizer.Normalize(Objective());
        const auto s_Second = s_Normalizer.Normalize(Objective());
        CHECK(*s_First.objective == *s_Second.objective);
        CHECK(s_Normalizer.GetCounters().normalized == 3);

        auto s_Challenge = TestJson::ObservationFromRecordedEvent(
            R"({"Name":"ChallengeCompleted","_DONTSEND":true,"Value":{"ChallengeId":"3485dbc5-a088-49b8-aaa4-14b26045e6ae"},"Timestamp":759.401611})");
        CHECK(s_Normalizer.Normalize(s_Challenge).outcome == TelemetryNormalizer::Outcome::DontSend);
        CHECK(s_Normalizer.GetCounters().dont_send == 2);
    }

    // The other families keep their classes beside the new row.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(Actor(13)).gating == TelemetryNormalizer::Gating::AttemptGated); // Kill Novikov
        CHECK(s_Normalizer.Normalize(Item(B0Fixtures::k_ItemWrenchPickup1)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(B0Fixtures::k_Disguise[B0Fixtures::k_DisguiseChange1].json)).gating
              == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(
            B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractStartA].json)).gating == TelemetryNormalizer::Gating::Ungated);
    }

    // Frame order, observed shape (B0): the target kill and the objective 13 ms later drain in one
    // frame as consecutive sequences inside the attempt.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Actor(13)));   // Kill Novikov, index 162
        CHECK(s_Rig.Queue.Push(Objective())); // index 163
        auto s_Drain = s_Rig.Frame(k_Playing);
        CHECK(s_Drain.outcomes_published == 1 && s_Drain.ungated_published == 1 && s_Drain.outside_attempt == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "actor.died", "objective.completed"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (a): an objective queued BEFORE the fall frame's drain publishes before
    // mission.stopped.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Objective()));
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.ungated_published == 1 && s_Fall.outside_attempt == 0 && s_Fall.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "objective.completed", "mission.stopped"}));
        CHECK(s_Rig.Sink->Published[1].sequence == 2 && s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (b): the same observation presented AFTER that frame's drain publishes on
    // the next processed frame, AFTER mission.stopped, with its full payload and its own sequence.
    // Not outside-attempt, no warning (42.4). An attempt-gated item beside it is still gated.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.edge_published);
        CHECK(s_Rig.Queue.Push(Objective()));
        CHECK(s_Rig.Queue.Push(Item(B0Fixtures::k_ItemCleaverPickup)));
        auto s_Next = s_Rig.Frame(k_Reloading);
        CHECK(s_Next.ungated_published == 1 && s_Next.outside_attempt == 1 && s_Next.outcomes_published == 0 && !s_Next.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "objective.completed"}));
        CHECK(s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Sink->Published[2].json.find(k_ExpectedPayload) != std::string::npos);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("'ItemPickedUp'") != std::string::npos
              && s_Rig.Warnings[0].find("no open mission attempt") != std::string::npos);
    }

    // Before any rise and after a stop: both publish with the payload intact; nothing is gated.
    {
        Rig s_Rig;
        CHECK(s_Rig.Queue.Push(Objective()));
        auto s_Before = s_Rig.Frame(k_Reloading);
        CHECK(s_Before.ungated_published == 1 && s_Before.outside_attempt == 0);
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Queue.Push(Objective()));
        auto s_After = s_Rig.Frame(k_Reloading);
        CHECK(s_After.ungated_published == 1 && s_After.outside_attempt == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"objective.completed", "mission.playing", "mission.stopped", "objective.completed"}));
        CHECK(s_Rig.Contiguous() && s_Rig.Warnings.empty());
        for (const auto& s_Envelope : s_Rig.Sink->Published)
            if (s_Envelope.event_type == "objective.completed")
                CHECK(s_Envelope.json.find(k_ExpectedPayload) != std::string::npos);
    }

    // A malformed objective consumes no sequence and is warned like any other; the valid
    // neighbour publishes.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(WithField(Objective(), "Id", Str(""))));
        CHECK(s_Rig.Queue.Push(Objective()));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 1 && s_Frame.ungated_published == 1);
        CHECK(s_Rig.Sink->Published.size() == 2 && s_Rig.Sink->Published[1].sequence == 2);
        CHECK(s_Rig.Warnings.size() == 1);
        CHECK(s_Rig.Warnings[0] == "telemetry 'ObjectiveCompleted' (index 163) not normalized: field 'Id' is empty; fields: 'Id' kind=String bytes=0, "
            "'Type' kind=String bytes=4, 'Category' kind=String bytes=7, 'ExcludeFromScoring' kind=Bool");
    }
}
