#include "TestHarness.h"

#include <vector>

#include "MissionObserver.h"

namespace
{
    SceneState Scene(const char* p_Type, int32_t p_Stage, bool p_Loaded, const char* p_Resource = "assembly:/x.entity")
    {
        SceneState s_Scene;
        s_Scene.available = true;
        s_Scene.scene_resource = p_Resource;
        s_Scene.scene_type = p_Type;
        s_Scene.codename_hint = "Peacock";
        s_Scene.loading_stage = p_Stage;
        s_Scene.scene_loaded = p_Loaded;
        return s_Scene;
    }

    bool IsPlaying(const std::optional<MissionEvent>& p_Event)
    {
        return p_Event && std::holds_alternative<MissionPlayingEvent>(*p_Event);
    }

    bool IsStopped(const std::optional<MissionEvent>& p_Event)
    {
        return p_Event && std::holds_alternative<MissionStoppedEvent>(*p_Event);
    }

    const MissionScenePayload& Payload(const std::optional<MissionEvent>& p_Event)
    {
        return std::visit([](const auto& p_Concrete) -> const MissionScenePayload& { return p_Concrete; }, *p_Event);
    }

    // Replays a whole timeline and returns the edges in order, as "+resource" / "-resource".
    std::vector<std::string> Edges(const std::vector<SceneState>& p_Frames)
    {
        MissionObserver s_Observer;
        std::vector<std::string> s_Edges;

        for (const auto& s_Frame : p_Frames)
        {
            const auto s_Event = s_Observer.Update(s_Frame, std::nullopt);

            if (IsPlaying(s_Event))
                s_Edges.push_back("+" + Payload(s_Event).scene_resource);
            else if (IsStopped(s_Event))
                s_Edges.push_back("-" + Payload(s_Event).scene_resource);
        }

        return s_Edges;
    }
}

void RunMissionObserverTests()
{
    // Predicate (unchanged since M1).
    CHECK(MissionObserver::IsMissionPlaying(Scene("mission", 8, true)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("mission", 8, false)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("mission", 7, true)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("", 8, true)));        // the frontend, which also has a local player
    CHECK(!MissionObserver::IsMissionPlaying(Scene("Mission", 8, true))); // exact match only
    CHECK(!MissionObserver::IsMissionPlaying(SceneState{}));              // unavailable counts as not playing

    // Edge detection on the sequence observed in experiment 4 (2026-10-06), now in both directions.
    MissionObserver s_Observer;
    const std::optional<std::string> s_NoId;

    // Boot and main menu: stages 5..8, loaded, type empty. No event.
    for (int32_t s_Stage : {5, 6, 7, 8})
        CHECK(!s_Observer.Update(Scene("", s_Stage, true), s_NoId));
    CHECK(!s_Observer.Playing());

    // Menu -> Paris: loaded drops, stage 0, 2, then the new scene's stages 5, 6, 7 unloaded.
    CHECK(!s_Observer.Update(Scene("", 8, false), s_NoId));
    CHECK(!s_Observer.Update(Scene("", 0, false), s_NoId));
    CHECK(!s_Observer.Update(Scene("", 2, false), s_NoId));
    for (int32_t s_Stage : {5, 6, 7})
        CHECK(!s_Observer.Update(Scene("mission", s_Stage, false, "assembly:/paris.entity"), s_NoId));

    // Stage 8 and loaded in the same frame: exactly one mission.playing.
    const auto s_First = s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-1"));
    CHECK(IsPlaying(s_First));
    CHECK(Payload(s_First).scene_resource == "assembly:/paris.entity");
    CHECK(Payload(s_First).scene_type == "mission");
    CHECK(Payload(s_First).codename_hint == "Peacock");
    CHECK(Payload(s_First).game_session_id == std::string("session-1"));
    CHECK(s_Observer.Playing());

    // Duplicate frames while true: nothing more, even if the session id changes.
    for (int i = 0; i < 100; ++i)
        CHECK(!s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-2")));

    // Mission restart: loaded drops with stage still 8 (as observed) -> exactly one mission.stopped,
    // carrying the scene the fall frame reports and whatever session id was readable then.
    const auto s_FirstStop = s_Observer.Update(Scene("mission", 8, false, "assembly:/paris.entity"), std::string("session-1"));
    CHECK(IsStopped(s_FirstStop));
    CHECK(Payload(s_FirstStop).scene_resource == "assembly:/paris.entity");
    CHECK(Payload(s_FirstStop).scene_type == "mission");
    CHECK(Payload(s_FirstStop).game_session_id == std::string("session-1"));
    CHECK(!s_Observer.Playing());

    // Duplicate frames while false, through the restart's 0, 5, 6, 7: nothing.
    for (int i = 0; i < 50; ++i)
        CHECK(!s_Observer.Update(Scene("mission", 8, false, "assembly:/paris.entity"), s_NoId));
    CHECK(!s_Observer.Update(Scene("mission", 0, false, "assembly:/paris.entity"), s_NoId));
    for (int32_t s_Stage : {5, 6, 7})
        CHECK(!s_Observer.Update(Scene("mission", s_Stage, false, "assembly:/paris.entity"), s_NoId));

    // Stage 8 loaded again, no LoadScene involved: one more mission.playing.
    const auto s_Second = s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-2"));
    CHECK(IsPlaying(s_Second));
    CHECK(Payload(s_Second).game_session_id == std::string("session-2"));

    // Back to the menu: one mission.stopped when loaded drops; the menu reaching stage 8 loaded is
    // not a mission, so nothing after that.
    CHECK(IsStopped(s_Observer.Update(Scene("mission", 8, false, "assembly:/paris.entity"), s_NoId)));
    CHECK(!s_Observer.Update(Scene("mission", 2, false, "assembly:/paris.entity"), s_NoId));
    for (int32_t s_Stage : {5, 6, 7})
        CHECK(!s_Observer.Update(Scene("", s_Stage, false), s_NoId));
    CHECK(!s_Observer.Update(Scene("", 8, true), s_NoId));
    CHECK(!s_Observer.Playing());

    // A session id missing on either edge does not suppress the event.
    MissionObserver s_Other;
    const auto s_NoIdEvent = s_Other.Update(Scene("mission", 8, true), s_NoId);
    CHECK(IsPlaying(s_NoIdEvent));
    CHECK(!Payload(s_NoIdEvent).game_session_id.has_value());
    const auto s_NoIdStop = s_Other.Update(Scene("mission", 7, true), s_NoId);
    CHECK(IsStopped(s_NoIdStop));
    CHECK(!Payload(s_NoIdStop).game_session_id.has_value());

    // Losing observability while playing is a fall of the predicate: mission.stopped with the empty
    // scene the fall frame reports. Regaining it is a new rise.
    CHECK(IsPlaying(s_Other.Update(Scene("mission", 8, true), s_NoId)));
    const auto s_LostStop = s_Other.Update(SceneState{}, s_NoId);
    CHECK(IsStopped(s_LostStop));
    CHECK(Payload(s_LostStop).scene_resource.empty());
    CHECK(!s_Other.Playing());
    CHECK(IsPlaying(s_Other.Update(Scene("mission", 8, true), s_NoId)));

    // Strict alternation: no two consecutive edges in the same direction, whatever the frames.
    {
        MissionObserver s_Alternating;
        bool s_LastWasPlaying = false;
        int s_EdgeCount = 0;
        const std::vector<SceneState> s_Frames = {
            Scene("mission", 8, true), Scene("mission", 8, true), Scene("mission", 7, true), Scene("mission", 7, true),
            Scene("mission", 8, true), Scene("", 8, true),        Scene("mission", 8, false), Scene("mission", 8, true),
            SceneState{},               Scene("mission", 8, true), Scene("mission", 8, true),
        };

        for (const auto& s_Frame : s_Frames)
        {
            if (const auto s_Event = s_Alternating.Update(s_Frame, s_NoId))
            {
                const bool s_IsPlaying = IsPlaying(s_Event);
                if (s_EdgeCount > 0)
                    CHECK(s_IsPlaying != s_LastWasPlaying);
                s_LastWasPlaying = s_IsPlaying;
                ++s_EdgeCount;
            }
        }

        CHECK(s_EdgeCount == 7); // +, -, +, -, +, -, + (frame 7 is a duplicate "false", frame 11 a duplicate "true")
    }

    // The whole M1 final-run timeline (menu, Paris, restart, menu, Sapienza, menu) yields exactly
    // three rise/fall pairs, in order, each naming the mission it brackets.
    {
        const char* s_Menu = "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
        const char* s_Paris = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        const char* s_Sapienza = "assembly:/_PRO/Scenes/Missions/CoastalTown/Mission01.entity";

        const std::vector<SceneState> s_Timeline = {
            Scene("", 5, true, s_Menu), Scene("", 8, true, s_Menu),
            Scene("", 8, false, s_Menu), Scene("", 0, false, s_Menu), Scene("", 2, false, s_Menu),
            Scene("mission", 5, false, s_Paris), Scene("mission", 7, false, s_Paris),
            Scene("mission", 8, true, s_Paris), Scene("mission", 8, true, s_Paris),
            Scene("mission", 8, false, s_Paris), Scene("mission", 0, false, s_Paris), Scene("mission", 7, false, s_Paris),
            Scene("mission", 8, true, s_Paris),
            Scene("mission", 8, false, s_Paris), Scene("mission", 2, false, s_Paris),
            Scene("", 7, false, s_Menu), Scene("", 8, true, s_Menu),
            Scene("", 8, false, s_Menu), Scene("", 2, false, s_Menu),
            Scene("mission", 7, true, s_Sapienza), // the loaded flag came first in Sapienza (stage 1 finding 1)
            Scene("mission", 8, true, s_Sapienza),
            Scene("mission", 8, false, s_Sapienza), Scene("", 8, true, s_Menu),
        };

        const std::vector<std::string> s_Expected = {
            std::string("+") + s_Paris, std::string("-") + s_Paris,
            std::string("+") + s_Paris, std::string("-") + s_Paris,
            std::string("+") + s_Sapienza, std::string("-") + s_Sapienza,
        };
        CHECK(Edges(s_Timeline) == s_Expected);
    }
}
