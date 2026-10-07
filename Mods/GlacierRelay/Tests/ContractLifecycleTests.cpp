#include "TestHarness.h"

#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "Fixtures/B0ContractLifecycle.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B2: ContractStart -> contract.started v1 and ContractFailed -> contract.ended v1, normalized
// from the recorded B0 payloads; the ungated publication class; and the frame ordering observed
// in B0 and B1 (design section 27.3) reproduced through the production sequencing code.
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Contract(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_ContractLifecycle[p_Index];
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

    const SceneState k_Menu = Scene("", 8, true);
    const SceneState k_Loading = Scene("mission", 7, false);
    const SceneState k_Playing = Scene("mission", 8, true);
    const SceneState k_Fallen = Scene("mission", 8, false);
    const SceneState k_Reloading = Scene("mission", 0, false);
}

void RunContractLifecycleTests()
{
    // Table: the four supported names, two gating classes.
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ContractStart"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("ContractFailed"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ContractEnd"));       // never observed; not in the table
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("HeroSpawn_Location")); // not normalized in B2
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("SegmentClosing"));     // backend-received, not on this hook

    // Reason kinds: the two observed strings, exactly; anything else is "other".
    CHECK(TelemetryNormalizer::ReasonKind("Contract ended manually: OnRestartLevel") == "restart");
    CHECK(TelemetryNormalizer::ReasonKind("Contract ended manually: User pressed exit to Main menu") == "exit_to_menu");
    CHECK(TelemetryNormalizer::ReasonKind("Contract ended manually: onrestartlevel") == "other");
    CHECK(TelemetryNormalizer::ReasonKind("Hero died") == "other");
    CHECK(TelemetryNormalizer::ReasonKind("") == "other");

    // The two recorded ContractStart payloads normalize to the bounded contract.started shape.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_A = s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractStartA));
        CHECK(s_A.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_A.gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_A.contract_started.has_value() && !s_A.event.has_value() && !s_A.contract_ended.has_value());

        const auto& e = *s_A.contract_started;
        CHECK(e.engine_event == "ContractStart");
        CHECK(e.contract_session_id == "2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f");
        CHECK(e.contract_id == "00000000-0000-0000-0000-000000000200");
        CHECK(e.location_id == "LOCATION_PARIS");
        CHECK(e.contract_type == "mission");
        CHECK(e.difficulty_level == 2);
        CHECK(e.starting_disguise_repository_id == "874c4c48-0a8b-49e9-883e-49fc5f1fb051");
        CHECK(e.is_hitman_suit);
        CHECK(e.engine_timestamp_s.has_value() && *e.engine_timestamp_s == 0.0);

        const auto s_B = s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractStartB));
        CHECK(s_B.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_B.contract_started->contract_session_id == "2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76");
        CHECK(s_B.contract_started->location_id == "LOCATION_PARIS");

        // Exact payload text: the deferred source fields (Loadout, GameChangers, IsVR,
        // SelectedCharacterId, Xbox*) are absent from the wire.
        const std::string s_Json = RelaySerialization::ContractStartedPayloadJson(e);
        CHECK(s_Json ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ContractStart\","
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"contract_id\":\"00000000-0000-0000-0000-000000000200\",\"location_id\":\"LOCATION_PARIS\","
            "\"contract_type\":\"mission\",\"difficulty_level\":2,"
            "\"starting_disguise_repository_id\":\"874c4c48-0a8b-49e9-883e-49fc5f1fb051\",\"is_hitman_suit\":true,"
            "\"engine_timestamp_s\":0}");
        CHECK(s_Json.find("Loadout") == std::string::npos && s_Json.find("loadout") == std::string::npos);
        CHECK(s_Json.find("GameChangers") == std::string::npos && s_Json.find("Xbox") == std::string::npos);
        CHECK(s_Normalizer.GetCounters().normalized == 2 && s_Normalizer.GetCounters().malformed == 0);
    }

    // The two recorded ContractFailed payloads: restart and exit to menu, reason verbatim.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Restart = s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractFailedRestartA));
        CHECK(s_Restart.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Restart.gating == TelemetryNormalizer::Gating::Ungated);
        CHECK(s_Restart.contract_ended.has_value() && !s_Restart.event.has_value());
        CHECK(s_Restart.contract_ended->engine_event == "ContractFailed");
        CHECK(s_Restart.contract_ended->contract_session_id == "2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f");
        CHECK(s_Restart.contract_ended->reason == "Contract ended manually: OnRestartLevel");
        CHECK(s_Restart.contract_ended->reason_kind == "restart");
        CHECK(s_Restart.contract_ended->engine_timestamp_s.has_value() && *s_Restart.contract_ended->engine_timestamp_s > 907.9 && *s_Restart.contract_ended->engine_timestamp_s < 908.0);

        const auto s_Exit = s_Normalizer.Normalize(Contract(B0Fixtures::k_ContractFailedExitB));
        CHECK(s_Exit.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Exit.contract_ended->contract_session_id == "2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76");
        CHECK(s_Exit.contract_ended->reason == "Contract ended manually: User pressed exit to Main menu");
        CHECK(s_Exit.contract_ended->reason_kind == "exit_to_menu");

        CHECK(RelaySerialization::ContractEndedPayloadJson(*s_Restart.contract_ended) ==
            "{\"source\":\"engine_telemetry\",\"engine_event\":\"ContractFailed\","
            "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
            "\"contract_id\":\"00000000-0000-0000-0000-000000000200\","
            "\"reason\":\"Contract ended manually: OnRestartLevel\",\"reason_kind\":\"restart\","
            "\"engine_timestamp_s\":907.95282}");
    }

    // An unknown reason string is a valid event: reason_kind "other", string preserved.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Obs = Contract(B0Fixtures::k_ContractFailedExitB);
        s_Obs.value.text = "Contract ended: some new IOI reason";
        const auto s_Result = s_Normalizer.Normalize(s_Obs);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Result.contract_ended->reason_kind == "other");
        CHECK(s_Result.contract_ended->reason == "Contract ended: some new IOI reason");
        CHECK(s_Normalizer.GetCounters().malformed == 0);
    }

    // Malformed ContractStart variants: each required field missing or mistyped; the session id
    // on the envelope is required because it is the event's subject.
    {
        TelemetryNormalizer s_Normalizer;
        const std::string s_Base = B0Fixtures::k_ContractLifecycle[B0Fixtures::k_ContractStartA].json;
        const std::vector<std::pair<std::string, std::string>> s_Mutations = {
            {R"("LocationId":"LOCATION_PARIS",)", ""},
            {R"("LocationId":"LOCATION_PARIS")", R"("LocationId":7)"},
            {R"("ContractType":"mission",)", ""},
            {R"("DifficultyLevel":2.0)", R"("DifficultyLevel":2.5)"},
            {R"("DifficultyLevel":2.0)", R"("DifficultyLevel":"2")"},
            {R"("Disguise":"874c4c48-0a8b-49e9-883e-49fc5f1fb051",)", ""},
            {R"("IsHitmanSuit":true)", R"("IsHitmanSuit":"yes")"},
            {R"("ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f",)", ""},
        };

        for (const auto& [s_From, s_To] : s_Mutations)
        {
            const auto s_Result = s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(Mutate(s_Base, s_From, s_To)));
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.detail.empty());
            CHECK(!s_Result.contract_started.has_value());
        }

        // Value not an object.
        auto s_NotObject = Contract(B0Fixtures::k_ContractStartA);
        s_NotObject.value = TestJson::Parse(R"("LOCATION_PARIS")");
        CHECK(s_Normalizer.Normalize(s_NotObject).outcome == TelemetryNormalizer::Outcome::Malformed);

        CHECK(s_Normalizer.GetCounters().malformed == s_Mutations.size() + 1);
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("ContractStart") == s_Mutations.size() + 1);
    }

    // Malformed ContractFailed variants: Value not a string, empty reason, no session id.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Object = Contract(B0Fixtures::k_ContractFailedRestartA);
        s_Object.value = TestJson::Parse(R"({"FailType":"OrphanedSession"})"); // the backend's shape, never sent by the client
        CHECK(s_Normalizer.Normalize(s_Object).outcome == TelemetryNormalizer::Outcome::Malformed);

        auto s_Empty = Contract(B0Fixtures::k_ContractFailedRestartA);
        s_Empty.value.text = "";
        CHECK(s_Normalizer.Normalize(s_Empty).outcome == TelemetryNormalizer::Outcome::Malformed);

        auto s_NoSession = Contract(B0Fixtures::k_ContractFailedRestartA);
        s_NoSession.contract_session_id.clear();
        CHECK(s_Normalizer.Normalize(s_NoSession).outcome == TelemetryNormalizer::Outcome::Malformed);

        CHECK(s_Normalizer.GetCounters().malformed == 3);
        CHECK(s_Normalizer.GetCounters().normalized == 0);
    }

    // Gating of the B1 rows is unchanged.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(Actor(0)).gating == TelemetryNormalizer::Gating::AttemptGated);
        CHECK(s_Normalizer.Normalize(Actor(3)).gating == TelemetryNormalizer::Gating::AttemptGated);
    }

    // Fresh load, as observed in B0 and B1: ContractStart is captured before the rise. It publishes
    // in the frame it is drained, while the predicate is still false, and is not counted as
    // outside-attempt; the rise follows in the same sequence.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Menu);
        s_Rig.Frame(k_Loading);
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractStartA)));
        auto s_Drain = s_Rig.Frame(k_Loading); // still loading: no edge
        CHECK(s_Drain.ungated_published == 1 && s_Drain.outside_attempt == 0 && s_Drain.outcomes_published == 0);
        CHECK(!s_Drain.edge_published && !s_Rig.Observer.Playing());
        auto s_Rise = s_Rig.Frame(k_Playing);
        CHECK(s_Rise.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"contract.started", "mission.playing"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Warnings.empty());
    }

    // Restart, as observed: ContractFailed(restart) is captured ~1.9 s before the fall, so it
    // publishes inside the attempt; then the fall; then the reload; the new ContractStart is
    // captured in the rise frame and drains the frame after, so it follows mission.playing.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Actor(3))); // a Kill during the attempt
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractFailedRestartA)));
        auto s_Before = s_Rig.Frame(k_Playing); // the predicate has not fallen yet
        CHECK(s_Before.ungated_published == 1 && !s_Before.edge_published);
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.edge_published && !s_Rig.Observer.Playing());
        s_Rig.Frame(k_Reloading);
        s_Rig.Frame(k_Loading);
        auto s_Rise = s_Rig.Frame(k_Playing);
        CHECK(s_Rise.edge_published);
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractStartB))); // emitted later in the rise frame
        auto s_Next = s_Rig.Frame(k_Playing);
        CHECK(s_Next.ungated_published == 1 && !s_Next.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{
            "mission.playing", "actor.died", "contract.ended", "mission.stopped", "mission.playing", "contract.started"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Warnings.empty());
    }

    // Exit to menu, as observed: the fall is processed, then ContractFailed(exit) is captured in
    // the same engine frame and drains the frame after. It is ungated, so it publishes after
    // mission.stopped instead of becoming an outside-attempt drop.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.edge_published);
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractFailedExitB)));
        auto s_After = s_Rig.Frame(Scene("mission", 2, false));
        CHECK(s_After.ungated_published == 1 && s_After.outside_attempt == 0 && s_After.outcomes_published == 0);
        s_Rig.Frame(k_Menu);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "contract.ended"}));
        CHECK(s_Rig.Contiguous());
        CHECK(s_Rig.Warnings.empty());
    }

    // The attempt-gated rule is untouched by the new class: an actor outcome captured after the
    // fall is still outside-attempt, even when an ungated event in the same drain publishes.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Queue.Push(Actor(3)));
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractFailedExitB)));
        auto s_After = s_Rig.Frame(k_Reloading);
        CHECK(s_After.outside_attempt == 1 && s_After.ungated_published == 1 && s_After.outcomes_published == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "contract.ended"}));
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("no open mission attempt") != std::string::npos);
    }

    // A malformed ungated observation consumes no sequence and is warned like any other.
    {
        Rig s_Rig;
        auto s_Bad = Contract(B0Fixtures::k_ContractFailedRestartA);
        s_Bad.value.text.clear();
        CHECK(s_Rig.Queue.Push(s_Bad));
        CHECK(s_Rig.Queue.Push(Contract(B0Fixtures::k_ContractStartA)));
        auto s_Frame = s_Rig.Frame(k_Loading);
        CHECK(s_Frame.malformed == 1 && s_Frame.ungated_published == 1);
        CHECK(s_Rig.Sink->Published.size() == 1 && s_Rig.Sink->Published[0].sequence == 1);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("not normalized") != std::string::npos);
    }

    // Without an adapter an ungated event is dropped with the engine-init warning, not counted as
    // outside-attempt.
    {
        TelemetryQueue s_Queue(8);
        TelemetryNormalizer s_Normalizer;
        MissionObserver s_Observer;
        std::vector<std::string> s_Warnings;
        s_Queue.Push(Contract(B0Fixtures::k_ContractStartA));
        auto s_Result = RelayFrame::Process(
            s_Queue, s_Normalizer, s_Observer, nullptr, k_Loading, std::nullopt,
            [&](const std::string& p_Line) { s_Warnings.push_back(p_Line); }
        );
        CHECK(s_Result.ungated_published == 0 && s_Result.outside_attempt == 0);
        CHECK(s_Warnings.size() == 1 && s_Warnings[0].find("before the adapter existed") != std::string::npos);
    }
}
