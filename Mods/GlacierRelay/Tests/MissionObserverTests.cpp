#include "TestHarness.h"

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
}

void RunMissionObserverTests()
{
    // Predicate.
    CHECK(MissionObserver::IsMissionPlaying(Scene("mission", 8, true)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("mission", 8, false)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("mission", 7, true)));
    CHECK(!MissionObserver::IsMissionPlaying(Scene("", 8, true)));        // the frontend, which also has a local player
    CHECK(!MissionObserver::IsMissionPlaying(Scene("Mission", 8, true))); // exact match only
    CHECK(!MissionObserver::IsMissionPlaying(SceneState{}));              // unavailable counts as not playing

    // Edge detection on the sequence observed in experiment 4 (2026-10-06).
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

    // Stage 8 and loaded in the same frame: exactly one event.
    const auto s_First = s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-1"));
    CHECK(s_First.has_value());
    CHECK(s_First->scene_resource == "assembly:/paris.entity");
    CHECK(s_First->scene_type == "mission");
    CHECK(s_First->codename_hint == "Peacock");
    CHECK(s_First->game_session_id == std::string("session-1"));
    CHECK(s_Observer.Playing());

    // Still playing: nothing more, even if the session id changes.
    for (int i = 0; i < 100; ++i)
        CHECK(!s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-2")));

    // Mission restart: no LoadScene; loaded drops, stage 0, then 5, 6, 7, 8. One more event.
    CHECK(!s_Observer.Update(Scene("mission", 8, false, "assembly:/paris.entity"), s_NoId));
    CHECK(!s_Observer.Playing());
    CHECK(!s_Observer.Update(Scene("mission", 0, false, "assembly:/paris.entity"), s_NoId));
    for (int32_t s_Stage : {5, 6, 7})
        CHECK(!s_Observer.Update(Scene("mission", s_Stage, false, "assembly:/paris.entity"), s_NoId));
    const auto s_Second = s_Observer.Update(Scene("mission", 8, true, "assembly:/paris.entity"), std::string("session-2"));
    CHECK(s_Second.has_value());
    CHECK(s_Second->game_session_id == std::string("session-2"));

    // Back to the menu: the menu reaches stage 8 loaded but is not a mission. No event.
    CHECK(!s_Observer.Update(Scene("mission", 8, false, "assembly:/paris.entity"), s_NoId));
    CHECK(!s_Observer.Update(Scene("mission", 2, false, "assembly:/paris.entity"), s_NoId));
    for (int32_t s_Stage : {5, 6, 7})
        CHECK(!s_Observer.Update(Scene("", s_Stage, false), s_NoId));
    CHECK(!s_Observer.Update(Scene("", 8, true), s_NoId));
    CHECK(!s_Observer.Playing());

    // Session id missing does not suppress the event.
    MissionObserver s_Other;
    const auto s_NoIdEvent = s_Other.Update(Scene("mission", 8, true), s_NoId);
    CHECK(s_NoIdEvent.has_value());
    CHECK(!s_NoIdEvent->game_session_id.has_value());

    // Losing observability while playing drops the predicate; regaining it is a new edge.
    CHECK(!s_Other.Update(SceneState{}, s_NoId));
    CHECK(!s_Other.Playing());
    CHECK(s_Other.Update(Scene("mission", 8, true), s_NoId).has_value());
}
