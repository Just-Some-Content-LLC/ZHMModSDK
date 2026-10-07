#include "TestHarness.h"

#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "RelayFrame.h"
#include "TestJson.h"

// The frame-order contract (RelayFrame.h): telemetry captured while the previous mission-playing
// state was authoritative is drained and published before the next observed lifecycle edge is
// processed. These tests exercise the production sequencing code, not a re-enactment of it.
namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

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

    TelemetryObservation Fixture(size_t p_Index)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[p_Index].json, static_cast<uint32_t>(p_Index + 1));
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
    };

    const SceneState k_Playing = Scene("mission", 8, true);
    const SceneState k_Fallen = Scene("mission", 8, false); // the fall frame as observed in every run so far
}

void RunRelayFrameTests()
{
    // End-of-attempt ordering: the authorized model.
    //   1. MissionPlaying is true.  2. A supported telemetry observation is captured late in the
    //   mission.  3. Before the next frame update the scene changes so the predicate will be false.
    //   4. The frame update drains telemetry before processing the fall.  5. The actor outcome
    //   publishes while the attempt is still open.  6. mission.stopped follows.
    {
        Rig s_Rig;
        auto s_First = s_Rig.Frame(k_Playing);
        CHECK(!s_First.playing_before && s_First.playing_after && s_First.edge_published);
        CHECK(s_Rig.Types() == std::vector<std::string>{"mission.playing"});

        CHECK(s_Rig.Queue.Push(Fixture(3))); // Kill Jacqueline Ducloitre, captured by the detour this frame

        auto s_Fall = s_Rig.Frame(k_Fallen);  // next frame: the scene already reports the fall
        CHECK(s_Fall.playing_before && !s_Fall.playing_after);
        CHECK(s_Fall.outcomes_published == 1 && s_Fall.outside_attempt == 0 && s_Fall.edge_published);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "actor.died", "mission.stopped"}));
        CHECK(s_Rig.Sink->Published[1].sequence == 2 && s_Rig.Sink->Published[2].sequence == 3);
        CHECK(s_Rig.Warnings.empty());
    }

    // Several late observations keep their capture order ahead of the edge.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Fixture(2))); // Pacify Ducloitre
        CHECK(s_Rig.Queue.Push(Fixture(3))); // Kill Ducloitre
        CHECK(s_Rig.Queue.Push(Fixture(4))); // Kill Quiron
        auto s_Fall = s_Rig.Frame(k_Fallen);
        CHECK(s_Fall.outcomes_published == 3);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "actor.pacified", "actor.died", "actor.died", "mission.stopped"}));
        for (size_t i = 0; i < s_Rig.Sink->Published.size(); ++i)
            CHECK(s_Rig.Sink->Published[i].sequence == i + 1);
    }

    // The mirror case: an observation captured after the fall was already processed has no open
    // attempt and is not published (counted and warned), even if the mission rises again next.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        s_Rig.Frame(k_Fallen);
        CHECK(s_Rig.Queue.Push(Fixture(3)));
        auto s_Between = s_Rig.Frame(Scene("mission", 0, false));
        CHECK(s_Between.outcomes_published == 0 && s_Between.outside_attempt == 1);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("no open mission attempt") != std::string::npos);
        auto s_Rise = s_Rig.Frame(k_Playing);
        CHECK(s_Rise.edge_published && s_Rise.outcomes_published == 0);
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "mission.stopped", "mission.playing"}));
    }

    // A telemetry observation queued before the first rise is judged against "not playing" and is
    // not attached to the attempt that opens in the same frame.
    {
        Rig s_Rig;
        CHECK(s_Rig.Queue.Push(Fixture(0)));
        auto s_First = s_Rig.Frame(k_Playing);
        CHECK(s_First.outside_attempt == 1 && s_First.outcomes_published == 0 && s_First.edge_published);
        CHECK(s_Rig.Types() == std::vector<std::string>{"mission.playing"});
    }

    // Unavailable engine state: the queue is still drained against the current state, the
    // observer keeps its state (as before B1), no edge.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        CHECK(s_Rig.Queue.Push(Fixture(5)));
        auto s_Blind = s_Rig.Frame(std::nullopt);
        CHECK(s_Blind.outcomes_published == 1 && s_Blind.playing_before && s_Blind.playing_after && !s_Blind.edge_published);
        CHECK(s_Rig.Observer.Playing());
        CHECK(s_Rig.Types() == (std::vector<std::string>{"mission.playing", "actor.died"}));
    }

    // Malformed observations are warned, counted, and do not consume a sequence number.
    {
        Rig s_Rig;
        s_Rig.Frame(k_Playing);
        auto s_Bad = Fixture(3);
        s_Bad.value = TestJson::Parse(R"({"RepositoryId":"x"})");
        CHECK(s_Rig.Queue.Push(s_Bad));
        CHECK(s_Rig.Queue.Push(Fixture(3)));
        auto s_Frame = s_Rig.Frame(k_Playing);
        CHECK(s_Frame.malformed == 1 && s_Frame.outcomes_published == 1);
        CHECK(s_Rig.Sink->Published.size() == 2 && s_Rig.Sink->Published[1].sequence == 2);
        CHECK(s_Rig.Warnings.size() == 1 && s_Rig.Warnings[0].find("not normalized") != std::string::npos);
    }

    // Without an adapter nothing is published and the warnings say why (the engine-init race).
    {
        TelemetryQueue s_Queue(8);
        TelemetryNormalizer s_Normalizer;
        MissionObserver s_Observer;
        std::vector<std::string> s_Warnings;
        s_Queue.Push(Fixture(3));
        auto s_Result = RelayFrame::Process(
            s_Queue, s_Normalizer, s_Observer, nullptr, k_Playing, std::nullopt,
            [&](const std::string& p_Line) { s_Warnings.push_back(p_Line); }
        );
        CHECK(s_Result.outcomes_published == 0 && s_Result.outside_attempt == 1 && !s_Result.edge_published);
        CHECK(s_Warnings.size() == 2);
    }
}
