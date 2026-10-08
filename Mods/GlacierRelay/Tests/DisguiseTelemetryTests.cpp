#include "TestHarness.h"

#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "Fixtures/B0ContractLifecycle.h"
#include "Fixtures/B0Disguise.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B3: StartingSuit / Disguise -> disguise.equipped v1 (kind initial / change), DisguiseBlown ->
// disguise.compromised v1, BrokenDisguiseCleared -> disguise.compromise_cleared v1, normalized from
// the 8 recorded B0 payloads (design section 30.1); the attempt-gated publication class; and the
// frame-order guarantee at both sides of a fall frame's drain (section 30.7).
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Disguise(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Disguise[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    TelemetryObservation Actor(size_t p_Index)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[p_Index].json, static_cast<uint32_t>(p_Index + 1));
    }

    std::string Mutate(std::string p_Json, const std::string& p_From, const std::string& p_To)
    {
        const auto s_Pos = p_Json.find(p_From);
        CHECK(s_Pos != std::string::npos);
        return p_Json.replace(s_Pos, p_From.size(), p_To);
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
    constexpr const char* k_SessionB = "2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76";
    constexpr const char* k_Suit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
    constexpr const char* k_OutfitA = "2018db77-aa8a-4bf9-9afb-56bdaa161156";
    constexpr const char* k_OutfitB = "992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5";
}

void RunDisguiseTelemetryTests()
{
    // Table: the four supported names, all attempt-gated; neighbours that are not normalized.
    CHECK(TelemetryNormalizer::IsSupportedSourceName("StartingSuit"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("Disguise"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("DisguiseBlown"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("BrokenDisguiseCleared"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("Spotted"));     // detection: deferred
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("Witnesses"));   // detection: deferred
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("Trespassing")); // player state: B6
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("disguise"));    // names are exact

    // The 8 recorded payloads, in emission order: kind, provenance, id, session, timestamp.
    {
        TelemetryNormalizer s_Normalizer;

        struct Expected { size_t index; DisguiseEvent::Kind kind; const char* name; const char* id; const char* session; double t; };
        const std::vector<Expected> s_Expected = {
            {B0Fixtures::k_StartingSuitA, DisguiseEvent::Kind::Initial, "StartingSuit", k_Suit, k_SessionA, 13.011781},
            {B0Fixtures::k_DisguiseChange1, DisguiseEvent::Kind::Change, "Disguise", k_OutfitA, k_SessionA, 202.104156},
            {B0Fixtures::k_DisguiseBlown1, DisguiseEvent::Kind::Compromised, "DisguiseBlown", k_OutfitA, k_SessionA, 222.863129},
            {B0Fixtures::k_DisguiseCleared1, DisguiseEvent::Kind::CompromiseCleared, "BrokenDisguiseCleared", k_OutfitA, k_SessionA, 393.788727},
            {B0Fixtures::k_DisguiseChange2, DisguiseEvent::Kind::Change, "Disguise", k_OutfitB, k_SessionA, 497.756409},
            {B0Fixtures::k_DisguiseBlown2, DisguiseEvent::Kind::Compromised, "DisguiseBlown", k_OutfitB, k_SessionA, 615.012756},
            {B0Fixtures::k_DisguiseCleared2, DisguiseEvent::Kind::CompromiseCleared, "BrokenDisguiseCleared", k_OutfitB, k_SessionA, 624.086853},
            {B0Fixtures::k_StartingSuitB, DisguiseEvent::Kind::Initial, "StartingSuit", k_Suit, k_SessionB, 2.280066},
        };

        for (const auto& s_Case : s_Expected)
        {
            const auto s_Result = s_Normalizer.Normalize(Disguise(s_Case.index));
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
            CHECK(s_Result.gating == TelemetryNormalizer::Gating::AttemptGated);
            CHECK(s_Result.disguise.has_value());
            CHECK(!s_Result.event.has_value() && !s_Result.contract_started.has_value() && !s_Result.contract_ended.has_value());

            const auto& e = *s_Result.disguise;
            CHECK(e.kind == s_Case.kind);
            CHECK(e.engine_event == s_Case.name);
            CHECK(e.disguise_repository_id == s_Case.id);
            CHECK(e.contract_session_id.has_value() && *e.contract_session_id == s_Case.session);
            CHECK(e.engine_timestamp_s.has_value() && *e.engine_timestamp_s > s_Case.t - 1e-6 && *e.engine_timestamp_s < s_Case.t + 1e-6);
        }

        CHECK(s_Normalizer.GetCounters().normalized == B0Fixtures::k_DisguiseCount);
        CHECK(s_Normalizer.GetCounters().malformed == 0 && s_Normalizer.GetCounters().unsupported == 0);
    }

    // Exact wire payloads: kind only on disguise.equipped; ids verbatim; nothing else.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Initial = *s_Normalizer.Normalize(Disguise(B0Fixtures::k_StartingSuitA)).disguise;
        CHECK(RelaySerialization::DisguisePayloadJson(s_Initial) ==
            "{\"source\":\"engine_telemetry\",\"kind\":\"initial\",\"engine_event\":\"StartingSuit\","
            "\"disguise_repository_id\":\"874c4c48-0a8b-49e9-883e-49fc5f1fb051\","
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"engine_timestamp_s\":13.011781}");

        const auto s_Change = *s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseChange1)).disguise;
        CHECK(RelaySerialization::DisguisePayloadJson(s_Change) ==
            "{\"source\":\"engine_telemetry\",\"kind\":\"change\",\"engine_event\":\"Disguise\","
            "\"disguise_repository_id\":\"2018db77-aa8a-4bf9-9afb-56bdaa161156\","
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"engine_timestamp_s\":202.104156}");

        const auto s_Blown = *s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseBlown1)).disguise;
        const std::string s_BlownJson = RelaySerialization::DisguisePayloadJson(s_Blown);
        CHECK(s_BlownJson ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"DisguiseBlown\","
            "\"disguise_repository_id\":\"2018db77-aa8a-4bf9-9afb-56bdaa161156\","
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"engine_timestamp_s\":222.863129}");
        CHECK(s_BlownJson.find("kind") == std::string::npos);

        const auto s_Cleared = *s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseCleared2)).disguise;
        const std::string s_ClearedJson = RelaySerialization::DisguisePayloadJson(s_Cleared);
        CHECK(s_ClearedJson.find("\"engine_event\":\"BrokenDisguiseCleared\"") != std::string::npos);
        CHECK(s_ClearedJson.find("\"disguise_repository_id\":\"992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5\"") != std::string::npos);
        CHECK(s_ClearedJson.find("kind") == std::string::npos);

        // Without envelope provenance the optional fields are absent, not empty.
        DisguiseEvent s_Bare;
        s_Bare.kind = DisguiseEvent::Kind::CompromiseCleared;
        s_Bare.engine_event = "BrokenDisguiseCleared";
        s_Bare.disguise_repository_id = "x";
        CHECK(RelaySerialization::DisguisePayloadJson(s_Bare) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"BrokenDisguiseCleared\",\"disguise_repository_id\":\"x\"}");
    }

    // The adapter maps the kind to the event type; the sequence is the one shared stream.
    {
        Rig s_Rig;
        TelemetryNormalizer s_Normalizer;
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Disguise(B0Fixtures::k_StartingSuitA)).disguise);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseChange1)).disguise);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseBlown1)).disguise);
        s_Rig.Adapter->Publish(*s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseCleared1)).disguise);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "disguise.equipped", "disguise.equipped", "disguise.compromised", "disguise.compromise_cleared"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Sink->Published[0].json.find("\"schema_version\":1") != std::string::npos);
        CHECK(s_Rig.Sink->Published[0].json.find("\"kind\":\"initial\"") != std::string::npos);
        CHECK(s_Rig.Sink->Published[1].json.find("\"kind\":\"change\"") != std::string::npos);
    }

    // Malformed: Value not a string (an object, the ContractStart shape; an array, the Spotted
    // shape; a number), empty, or absent. Counted per name, nothing published.
    {
        TelemetryNormalizer s_Normalizer;
        const std::string s_Base = B0Fixtures::k_Disguise[B0Fixtures::k_DisguiseChange1].json;

        auto s_Object = Disguise(B0Fixtures::k_DisguiseChange1);
        s_Object.value = TestJson::Parse(R"({"Disguise":"2018db77-aa8a-4bf9-9afb-56bdaa161156","IsHitmanSuit":false})");
        CHECK(s_Normalizer.Normalize(s_Object).outcome == TelemetryNormalizer::Outcome::Malformed);

        auto s_Array = Disguise(B0Fixtures::k_DisguiseBlown1);
        s_Array.value = TestJson::Parse(R"(["5dc7ede5-bb9d-4f93-a892-cb7fb2791b19"])");
        CHECK(s_Normalizer.Normalize(s_Array).outcome == TelemetryNormalizer::Outcome::Malformed);

        auto s_Number = Disguise(B0Fixtures::k_DisguiseCleared1);
        s_Number.value = TestJson::Parse("7");
        CHECK(s_Normalizer.Normalize(s_Number).outcome == TelemetryNormalizer::Outcome::Malformed);

        auto s_Empty = Disguise(B0Fixtures::k_StartingSuitA);
        s_Empty.value.text.clear();
        const auto s_EmptyResult = s_Normalizer.Normalize(s_Empty);
        CHECK(s_EmptyResult.outcome == TelemetryNormalizer::Outcome::Malformed);
        CHECK(s_EmptyResult.detail.find("empty") != std::string::npos);

        const auto s_Absent = s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(
            Mutate(s_Base, R"("Value":"2018db77-aa8a-4bf9-9afb-56bdaa161156",)", "")));
        CHECK(s_Absent.outcome == TelemetryNormalizer::Outcome::Malformed);

        CHECK(s_Normalizer.GetCounters().malformed == 5);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("Disguise") == 2);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("DisguiseBlown") == 1);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("BrokenDisguiseCleared") == 1);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("StartingSuit") == 1);
    }

    // Diagnostic detail (B3 runtime follow-up, design section 33): a rejected Value reports its
    // copied kind and, when Unsupported, the engine type name the intake copied — bounded and
    // escaped. Rejection, counters, publication and sequence are unchanged by the detail.
    {
        TelemetryNormalizer s_Normalizer;

        auto s_Case = [&](TelemetryValue p_Value) {
            auto s_Obs = Disguise(B0Fixtures::k_DisguiseBlown1);
            s_Obs.value = std::move(p_Value);
            const auto s_Result = s_Normalizer.Normalize(s_Obs);
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.disguise.has_value());
            return s_Result.detail;
        };

        TelemetryValue s_Unsupported;
        s_Unsupported.kind = TelemetryValue::Kind::Unsupported;
        s_Unsupported.text = "ZRepositoryID";
        CHECK(s_Case(s_Unsupported) == "Value is not a string (kind=Unsupported type='ZRepositoryID')");

        TelemetryValue s_Null;
        CHECK(s_Case(s_Null) == "Value is not a string (kind=Null)");

        CHECK(s_Case(TestJson::Parse("7")) == "Value is not a string (kind=Number)");
        CHECK(s_Case(TestJson::Parse("true")) == "Value is not a string (kind=Bool)");
        CHECK(s_Case(TestJson::Parse(R"(["a","b"])")) == "Value is not a string (kind=Array items=2)");
        CHECK(s_Case(TestJson::Parse(R"({"Disguise":"x","IsHitmanSuit":false})")) == "Value is not a string (kind=Object fields=2)");

        // Truncated-by-the-intake marker is just another Unsupported type text.
        TelemetryValue s_Truncated;
        s_Truncated.kind = TelemetryValue::Kind::Unsupported;
        s_Truncated.text = "<truncated>";
        CHECK(s_Case(s_Truncated) == "Value is not a string (kind=Unsupported type='<truncated>')");

        // Escaping: quotes, backslashes, control and non-ASCII bytes become \xNN; nothing raw leaks.
        TelemetryValue s_Hostile;
        s_Hostile.kind = TelemetryValue::Kind::Unsupported;
        s_Hostile.text = std::string("Z'\\\n\x01") + "\xC3\xA9" + "Q";
        CHECK(s_Case(s_Hostile) == "Value is not a string (kind=Unsupported type='Z\\x27\\x5C\\x0A\\x01\\xC3\\xA9Q')");

        // Bounding: at most 64 bytes of the type name, then "...".
        TelemetryValue s_Long;
        s_Long.kind = TelemetryValue::Kind::Unsupported;
        s_Long.text = std::string(100, 'T');
        const auto s_LongDetail = s_Case(s_Long);
        CHECK(s_LongDetail == "Value is not a string (kind=Unsupported type='" + std::string(64, 'T') + "...')");

        // An empty type name (the intake's Null branch covers most of these, but a registered type
        // with an empty name would land here) is shown as empty, not omitted.
        TelemetryValue s_Empty;
        s_Empty.kind = TelemetryValue::Kind::Unsupported;
        CHECK(s_Case(s_Empty) == "Value is not a string (kind=Unsupported type='')");

        CHECK(s_Normalizer.GetCounters().malformed == 10);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("DisguiseBlown") == 10);
        CHECK(s_Normalizer.GetCounters().normalized == 0);

        // A valid string is unaffected by the diagnostic path.
        CHECK(s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseBlown1)).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // The diagnostic reaches the durable log through the existing warning path with the event
    // name and index, and still consumes no sequence.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Obs = Disguise(B0Fixtures::k_StartingSuitA);
        s_Obs.value.kind = TelemetryValue::Kind::Unsupported;
        s_Obs.value.text = "ZRepositoryID";
        CHECK(s_Rig.Queue.Push(s_Obs));
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange1)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 1 && s_Frame.outcomes_published == 1 && s_Frame.outside_attempt == 0);
        CHECK(s_Rig.Sink->Published.size() == 2 && s_Rig.Sink->Published[1].sequence == 2);
        CHECK(s_Rig.Warnings.size() == 1);
        CHECK(s_Rig.Warnings[0] == "telemetry 'StartingSuit' (index 10) not normalized: Value is not a string (kind=Unsupported type='ZRepositoryID')");
    }

    // Envelope provenance is optional: a payload without ContractSessionId or Timestamp is still
    // a valid occurrence (the id is the subject), with the optional fields absent.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Obs = Disguise(B0Fixtures::k_DisguiseBlown2);
        s_Obs.contract_session_id.clear();
        s_Obs.has_timestamp = false;
        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_Result.disguise->contract_session_id.has_value());
        CHECK(!s_Result.disguise->engine_timestamp_s.has_value());
    }

    // _DONTSEND on a disguise name: the section 18 policy applies whatever the name (synthetic flag
    // on a real payload; never observed on these names).
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Flagged = TestJson::ObservationFromRecordedEvent(Mutate(
            B0Fixtures::k_Disguise[B0Fixtures::k_DisguiseChange1].json, R"("Name":"Disguise",)", R"("Name":"Disguise","_DONTSEND":true,)"));
        CHECK(s_Flagged.dont_send);
        CHECK(s_Normalizer.Normalize(s_Flagged).outcome == TelemetryNormalizer::Outcome::DontSend);
        CHECK(s_Normalizer.GetCounters().dont_send == 1 && s_Normalizer.GetCounters().normalized == 0);
    }

    // A repeated valid occurrence is two events: no deduplication of any kind.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_First = s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseBlown1));
        const auto s_Second = s_Normalizer.Normalize(Disguise(B0Fixtures::k_DisguiseBlown1));
        CHECK(s_First.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Second.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(*s_First.disguise == *s_Second.disguise);
        CHECK(s_Normalizer.GetCounters().normalized == 2);
    }

    // The B1 and B2 rows keep their classes beside the new ones.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(Actor(0)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(
            B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractStartA].json)).gating == TelemetryNormalizer::Gating::Ungated);
    }

    // Frame order, observed shape (B0 session 1 through the production sequencing): every disguise
    // event is captured inside the attempt and publishes in its drain, in the shared sequence,
    // interleaved with actor outcomes exactly as captured.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_StartingSuitA)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange1)));
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseBlown1)));
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Actor(0))); // Pacify Parker
        CHECK(s_Rig.Queue.Push(Actor(3))); // Kill Ducloitre
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseCleared1)));
        auto s_Drain = s_Rig.Frame(k_Playing);
        CHECK(s_Drain.outcomes_published == 3 && s_Drain.outside_attempt == 0 && s_Drain.ungated_published == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "mission.playing", "disguise.equipped", "disguise.equipped", "disguise.compromised",
            "actor.pacified", "actor.died", "disguise.compromise_cleared"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Warnings.empty());
    }

    // Fall-frame ordering (a), synthetic: a Disguise queued BEFORE the fall frame's drain is judged
    // against the playing state and publishes before mission.stopped.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange2)));
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.outcomes_published == 1 && s_Fall.outside_attempt == 0 && s_Fall.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "disguise.equipped", "mission.stopped"}));
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
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseChange2)));
        auto s_Next = s_Rig.Frame(k_Reloading);
        CHECK(s_Next.outside_attempt == 1 && s_Next.outcomes_published == 0 && !s_Next.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped"}));
        CHECK(s_Rig.Adapter->PublishedCount() == 2);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("no open mission attempt") != std::string::npos);
    }

    // An ungated contract event in the same drain as an outside-attempt disguise event still
    // publishes (B2 behaviour unchanged beside the new rows).
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_StartingSuitB)));
        CHECK(s_Rig.Queue.Push(TestJson::ObservationFromRecordedEvent(
            B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractFailedExitB].json, 211)));
        auto s_After = s_Rig.Frame(k_Reloading);
        CHECK(s_After.outside_attempt == 1 && s_After.ungated_published == 1 && s_After.outcomes_published == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "contract.ended"}));
    }

    // A malformed disguise observation consumes no sequence and is warned like any other.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Bad = Disguise(B0Fixtures::k_DisguiseBlown1);
        s_Bad.value.text.clear();
        CHECK(s_Rig.Queue.Push(s_Bad));
        CHECK(s_Rig.Queue.Push(Disguise(B0Fixtures::k_DisguiseBlown1)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 1 && s_Frame.outcomes_published == 1);
        CHECK(s_Rig.Sink->Published.size() == 2 && s_Rig.Sink->Published[1].sequence == 2);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("not normalized") != std::string::npos);
    }
}
