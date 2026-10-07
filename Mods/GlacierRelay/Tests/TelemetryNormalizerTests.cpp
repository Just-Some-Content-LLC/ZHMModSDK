#include "TestHarness.h"

#include <map>
#include <set>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0ActorOutcomes.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    TelemetryObservation Fixture(size_t p_Index)
    {
        return TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[p_Index].json, static_cast<uint32_t>(p_Index + 1));
    }

    // A minimal well-formed Kill as the base for malformed variants.
    const char* k_MinimalKill = R"json({"Timestamp":1.5,"Name":"Kill","ContractSessionId":"sess-1","ContractId":"c-1","Value":{
        "RepositoryId":"aaaa","ActorId":42,"ActorName":"Test Actor","ActorType":1,"IsTarget":true,"KillType":4,"KillContext":4,
        "KillClass":"ballistic","Accident":false,"KillMethodBroad":"pistol","KillMethodStrict":"","DamageEvents":["Shoot"],
        "KillItemRepositoryId":"item-1"}})json";

    std::string Mutate(std::string p_Json, const std::string& p_From, const std::string& p_To)
    {
        const auto s_Pos = p_Json.find(p_From);
        CHECK(s_Pos != std::string::npos);
        return p_Json.replace(s_Pos, p_From.size(), p_To);
    }
}

void RunTelemetryNormalizerTests()
{
    // Table: exactly the two supported source names.
    CHECK(TelemetryNormalizer::IsSupportedSourceName("Kill"));
    CHECK(TelemetryNormalizer::IsSupportedSourceName("Pacify"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("kill"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("BodyFound"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName("ChallengeCompleted"));
    CHECK(!TelemetryNormalizer::IsSupportedSourceName(""));

    // Enum names.
    CHECK(TelemetryNormalizer::DeathTypeName(3) == "pacify" && TelemetryNormalizer::DeathTypeName(4) == "kill"
        && TelemetryNormalizer::DeathTypeName(5) == "bloody_kill" && TelemetryNormalizer::DeathTypeName(9) == "unknown");
    CHECK(TelemetryNormalizer::DeathContextName(3) == "accident" && TelemetryNormalizer::DeathContextName(4) == "murder"
        && TelemetryNormalizer::DeathContextName(1) == "not_hero" && TelemetryNormalizer::DeathContextName(42) == "unknown");
    CHECK(TelemetryNormalizer::ActorTypeName(0) == "civilian" && TelemetryNormalizer::ActorTypeName(1) == "guard"
        && TelemetryNormalizer::ActorTypeName(2) == "hitman" && TelemetryNormalizer::ActorTypeName(7) == "unknown");

    // Every B0 payload normalizes; Kill -> died, Pacify -> pacified; no dedup; counts as observed.
    {
        TelemetryNormalizer s_Normalizer;
        std::vector<ActorOutcomeEvent> s_Events;
        CHECK(B0Fixtures::k_ActorOutcomeCount == 16);

        for (size_t i = 0; i < B0Fixtures::k_ActorOutcomeCount; ++i)
        {
            const auto s_Result = s_Normalizer.Normalize(Fixture(i));
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
            if (s_Result.event)
                s_Events.push_back(*s_Result.event);
        }

        CHECK(s_Events.size() == 16);
        CHECK(s_Normalizer.GetCounters().normalized == 16);
        CHECK(s_Normalizer.GetCounters().malformed == 0);

        size_t s_Died = 0, s_Pacified = 0, s_Targets = 0, s_Guards = 0, s_Civilians = 0, s_Accidents = 0, s_Murder = 0;
        for (const auto& s_Event : s_Events)
        {
            s_Died += s_Event.kind == ActorOutcomeEvent::Kind::Died;
            s_Pacified += s_Event.kind == ActorOutcomeEvent::Kind::Pacified;
            s_Targets += s_Event.is_target;
            s_Guards += s_Event.actor_type == "guard";
            s_Civilians += s_Event.actor_type == "civilian";
            s_Accidents += s_Event.death_context == "accident";
            s_Murder += s_Event.death_context == "murder";
            CHECK(!s_Event.repository_id.empty());
            CHECK(!s_Event.actor_name.empty());
            CHECK(s_Event.engine_actor_id != 0);
            CHECK(!s_Event.actor_type_code.has_value());
            CHECK(!s_Event.death_type_code.has_value());
            CHECK(!s_Event.death_context_code.has_value());
            CHECK(s_Event.contract_session_id.has_value());
            CHECK(s_Event.engine_timestamp_s.has_value());
            CHECK((s_Event.kind == ActorOutcomeEvent::Kind::Pacified) == (s_Event.death_type == "pacify"));
            CHECK(s_Event.accident == (s_Event.death_context == "accident"));
        }
        CHECK(s_Died == 10 && s_Pacified == 6);
        CHECK(s_Targets == 2);   // Novikov pacified, then killed
        CHECK(s_Guards == 2 && s_Civilians == 14);
        CHECK(s_Accidents == 2 && s_Murder == 14);

        // Pacify then Kill of the same engine actor: two events, same observations, in order.
        const auto s_Ducloitre = [&](ActorOutcomeEvent::Kind p_Kind) {
            for (const auto& e : s_Events)
                if (e.actor_name == "Jacqueline Ducloitre" && e.kind == p_Kind)
                    return e;
            return ActorOutcomeEvent{};
        };
        const auto s_Pac = s_Ducloitre(ActorOutcomeEvent::Kind::Pacified);
        const auto s_Kill = s_Ducloitre(ActorOutcomeEvent::Kind::Died);
        CHECK(s_Pac.repository_id == "5dc7ede5-bb9d-4f93-a892-cb7fb2791b19");
        CHECK(s_Kill.repository_id == s_Pac.repository_id);
        CHECK(s_Kill.engine_actor_id == 195054661 && s_Pac.engine_actor_id == 195054661);
        CHECK(s_Pac.death_type == "pacify" && s_Pac.damage_events == std::vector<std::string>{"Subdue"} && s_Pac.method_broad == "unarmed");
        CHECK(s_Kill.death_type == "kill" && s_Kill.damage_events == std::vector<std::string>{"CoupDeGrace"});
        CHECK(!s_Kill.item_repository_id.has_value()); // the neck snap carried no item
        CHECK(*s_Kill.engine_timestamp_s > *s_Pac.engine_timestamp_s);

        // Classification and method fields on specific occurrences.
        const auto& s_Quiron = s_Events[4];
        CHECK(s_Quiron.actor_name == "Philippe Quiron" && s_Quiron.actor_type == "guard" && s_Quiron.death_type == "bloody_kill"
            && s_Quiron.kill_class == "ballistic" && s_Quiron.method_broad == "pistol"
            && s_Quiron.item_repository_id == std::string("e70adb5b-0646-4f88-bd4a-85bea7a2a654"));
        const auto& s_Donovan = s_Events[10];
        CHECK(s_Donovan.actor_name == "Kurt Donovan" && s_Donovan.actor_type == "guard" && s_Donovan.death_context == "accident"
            && s_Donovan.accident && s_Donovan.kill_class == "explosion" && s_Donovan.method_broad == "accident"
            && s_Donovan.method_strict == "accident_explosion");
        const auto& s_NovikovPacify = s_Events[12];
        CHECK(s_NovikovPacify.actor_name == "Viktor Novikov" && s_NovikovPacify.is_target && s_NovikovPacify.kind == ActorOutcomeEvent::Kind::Pacified
            && s_NovikovPacify.death_context == "murder" && !s_NovikovPacify.accident);
        const auto& s_NovikovKill = s_Events[13];
        CHECK(s_NovikovKill.is_target && s_NovikovKill.kind == ActorOutcomeEvent::Kind::Died
            && s_NovikovKill.engine_actor_id == s_NovikovPacify.engine_actor_id);

        // Provenance from the stream envelope.
        CHECK(*s_Events[0].contract_session_id == "2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f");
    }

    // Malformed variants of a well-formed Kill: each required field missing or mistyped.
    {
        TelemetryNormalizer s_Normalizer;
        CHECK(s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(k_MinimalKill)).outcome == TelemetryNormalizer::Outcome::Normalized);

        const std::vector<std::pair<std::string, std::string>> s_Mutations = {
            {R"("RepositoryId":"aaaa",)", ""},                      // missing
            {R"("RepositoryId":"aaaa")", R"("RepositoryId":"")"},  // empty
            {R"("ActorId":42)", R"("ActorId":"42")"},              // wrong type
            {R"("ActorId":42)", R"("ActorId":42.5)"},              // not integral
            {R"("ActorId":42)", R"("ActorId":-1)"},                // out of range
            {R"("ActorType":1)", R"("ActorType":true)"},
            {R"("IsTarget":true)", R"("IsTarget":1)"},
            {R"("KillType":4,)", ""},
            {R"("KillContext":4)", R"("KillContext":"murder")"},
            {R"("Accident":false)", R"("Accident":"no")"},
            {R"("KillClass":"ballistic")", R"("KillClass":7)"},
            {R"("KillMethodBroad":"pistol",)", ""},
            {R"("KillMethodStrict":"")", R"("KillMethodStrict":null)"},
            {R"("DamageEvents":["Shoot"])", R"("DamageEvents":"Shoot")"},
            {R"("DamageEvents":["Shoot"])", R"("DamageEvents":[1])"},
            {R"("KillItemRepositoryId":"item-1")", R"("KillItemRepositoryId":5)"}, // optional, but typed when present
        };

        size_t s_Malformed = 0;
        for (const auto& [s_From, s_To] : s_Mutations)
        {
            const auto s_Result = s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(Mutate(k_MinimalKill, s_From, s_To)));
            CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Malformed);
            CHECK(!s_Result.detail.empty());
            CHECK(!s_Result.event.has_value());
            s_Malformed += s_Result.outcome == TelemetryNormalizer::Outcome::Malformed;
        }
        CHECK(s_Malformed == s_Mutations.size());
        CHECK(s_Normalizer.GetCounters().malformed == s_Mutations.size());
        CHECK(s_Normalizer.GetCounters().malformed_by_name.at("Kill") == s_Mutations.size());

        // Value not an object.
        auto s_NotObject = TestJson::ObservationFromRecordedEvent(k_MinimalKill);
        s_NotObject.value = TestJson::Parse(R"("just a string")");
        CHECK(s_Normalizer.Normalize(s_NotObject).outcome == TelemetryNormalizer::Outcome::Malformed);

        // Optional item absent is fine; absent envelope fields are fine.
        auto s_NoItem = TestJson::ObservationFromRecordedEvent(Mutate(k_MinimalKill, R"(,
        "KillItemRepositoryId":"item-1")", ""));
        s_NoItem.contract_session_id.clear();
        s_NoItem.has_timestamp = false;
        const auto s_Result = s_Normalizer.Normalize(s_NoItem);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(!s_Result.event->item_repository_id.has_value());
        CHECK(!s_Result.event->contract_session_id.has_value());
        CHECK(!s_Result.event->engine_timestamp_s.has_value());

        // Unknown enum codes keep their number beside "unknown".
        const auto s_Unknown = s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(
            Mutate(Mutate(Mutate(k_MinimalKill, R"("ActorType":1)", R"("ActorType":9)"), R"("KillType":4)", R"("KillType":7)"), R"("KillContext":4)", R"("KillContext":12)")));
        CHECK(s_Unknown.outcome == TelemetryNormalizer::Outcome::Normalized);
        CHECK(s_Unknown.event->actor_type == "unknown" && s_Unknown.event->actor_type_code == 9);
        CHECK(s_Unknown.event->death_type == "unknown" && s_Unknown.event->death_type_code == 7);
        CHECK(s_Unknown.event->death_context == "unknown" && s_Unknown.event->death_context_code == 12);
    }

    // Unknown source event: counted by name, nothing produced.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Other = TestJson::ObservationFromRecordedEvent(R"json({"Name":"BodyFound","Value":{"DeadBody":{"RepositoryId":"x"}}})json");
        const auto s_Result = s_Normalizer.Normalize(s_Other);
        CHECK(s_Result.outcome == TelemetryNormalizer::Outcome::Unsupported);
        CHECK(!s_Result.event.has_value());
        CHECK(s_Normalizer.GetCounters().unsupported == 1);
        CHECK(s_Normalizer.GetCounters().unsupported_by_name.at("BodyFound") == 1);

        // The per-name map is bounded.
        for (int i = 0; i < 200; ++i)
            s_Normalizer.Normalize(TestJson::ObservationFromRecordedEvent(fmt::format(R"json({{"Name":"Ev{}","Value":{{}}}})json", i)));
        CHECK(s_Normalizer.GetCounters().unsupported_by_name.size() <= 65);
        CHECK(s_Normalizer.GetCounters().unsupported == 201);
    }

    // _DONTSEND: not normalized whatever the name, including a supported one.
    {
        TelemetryNormalizer s_Normalizer;
        auto s_Challenge = TestJson::ObservationFromRecordedEvent(
            R"json({"Name":"ChallengeCompleted","_DONTSEND":true,"Value":{"ChallengeId":"c"},"Timestamp":1.0})json");
        CHECK(s_Challenge.dont_send);
        CHECK(s_Normalizer.Normalize(s_Challenge).outcome == TelemetryNormalizer::Outcome::DontSend);

        auto s_FlaggedKill = TestJson::ObservationFromRecordedEvent(Mutate(k_MinimalKill, R"("Name":"Kill",)", R"("Name":"Kill","_DONTSEND":true,)"));
        CHECK(s_FlaggedKill.dont_send);
        CHECK(s_Normalizer.Normalize(s_FlaggedKill).outcome == TelemetryNormalizer::Outcome::DontSend);
        CHECK(s_Normalizer.GetCounters().dont_send == 2);
        CHECK(s_Normalizer.GetCounters().normalized == 0);

        // _DONTSEND false is not a flag.
        auto s_NotFlagged = TestJson::ObservationFromRecordedEvent(Mutate(k_MinimalKill, R"("Name":"Kill",)", R"("Name":"Kill","_DONTSEND":false,)"));
        CHECK(!s_NotFlagged.dont_send);
        CHECK(s_Normalizer.Normalize(s_NotFlagged).outcome == TelemetryNormalizer::Outcome::Normalized);
    }

    // Serialization: exact payload text for a normalized B0 event, and the adapter sequence spans
    // lifecycle and actor events.
    {
        TelemetryNormalizer s_Normalizer;
        const auto s_Kill = s_Normalizer.Normalize(Fixture(3)); // Kill Jacqueline Ducloitre (#55)
        CHECK(s_Kill.outcome == TelemetryNormalizer::Outcome::Normalized);
        const std::string s_Payload = RelaySerialization::ActorOutcomePayloadJson(*s_Kill.event);
        CHECK(s_Payload
            == "{\"source\":\"engine_telemetry\",\"repository_id\":\"5dc7ede5-bb9d-4f93-a892-cb7fb2791b19\","
               "\"actor_name\":\"Jacqueline Ducloitre\",\"engine_actor_id\":195054661,\"actor_type\":\"civilian\","
               "\"is_target\":false,\"death_type\":\"kill\",\"death_context\":\"murder\",\"accident\":false,"
               "\"kill_class\":\"melee\",\"method_broad\":\"unarmed\",\"method_strict\":\"\",\"damage_events\":[\"CoupDeGrace\"],"
               "\"contract_session_id\":\"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f\","
               "\"engine_timestamp_s\":393.780243}");

        auto s_Sink = std::make_unique<CapturingSink>();
        auto* s_SinkView = s_Sink.get();
        int s_Ticks = 0;
        RelayAdapter s_Adapter(std::move(s_Sink), "id-1", [&] { return fmt::format("t{}", ++s_Ticks); });

        MissionPlayingEvent s_Playing;
        s_Playing.scene_resource = "assembly:/paris.entity";
        s_Playing.scene_type = "mission";
        s_Playing.codename_hint = "Peacock";
        s_Adapter.Publish(s_Playing);
        s_Adapter.Publish(*s_Normalizer.Normalize(Fixture(2)).event); // Pacify Ducloitre
        s_Adapter.Publish(*s_Kill.event);                            // Kill Ducloitre
        MissionStoppedEvent s_Stopped;
        s_Stopped.scene_resource = "assembly:/paris.entity";
        s_Stopped.scene_type = "mission";
        s_Stopped.codename_hint = "Peacock";
        s_Adapter.Publish(s_Stopped);

        CHECK(s_SinkView->Published.size() == 4);
        CHECK(s_SinkView->Published[0].event_type == "mission.playing" && s_SinkView->Published[0].sequence == 1);
        CHECK(s_SinkView->Published[1].event_type == "actor.pacified" && s_SinkView->Published[1].sequence == 2);
        CHECK(s_SinkView->Published[2].event_type == "actor.died" && s_SinkView->Published[2].sequence == 3);
        CHECK(s_SinkView->Published[3].event_type == "mission.stopped" && s_SinkView->Published[3].sequence == 4);
        CHECK(s_SinkView->Published[2].json.find("\"event_type\":\"actor.died\",\"schema_version\":1,\"payload\":{\"source\":\"engine_telemetry\"") != std::string::npos);
        CHECK(s_SinkView->Published[1].json.find("\"event_type\":\"actor.pacified\",\"schema_version\":1,") != std::string::npos);
        CHECK(s_SinkView->Published[2].json.find('\n') == std::string::npos);

        // No native deduplication: publishing the same normalized event twice yields two envelopes.
        s_Adapter.Publish(*s_Kill.event);
        s_Adapter.Publish(*s_Kill.event);
        CHECK(s_SinkView->Published.size() == 6);
        CHECK(s_SinkView->Published[5].sequence == 6);
    }
}
