#include "TestHarness.h"

#include <vector>

#include <fmt/format.h>

#include "Json.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"

namespace
{
    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;

        void Publish(const PublishedEnvelope& p_Envelope) override
        {
            Published.push_back(p_Envelope);
        }
    };
}

void RunRelayAdapterTests()
{
    // JSON quoting.
    CHECK(Json::Quote("") == "\"\"");
    CHECK(Json::Quote("plain") == "\"plain\"");
    CHECK(Json::Quote("a\"b\\c") == "\"a\\\"b\\\\c\"");
    CHECK(Json::Quote("\n\t\x01") == "\"\\n\\t\\u0001\"");
    CHECK(Json::Quote("assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity")
        == "\"assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity\"");
    CHECK(Json::Quote("caf\xc3\xa9") == "\"caf\xc3\xa9\""); // UTF-8 passes through

    // Payload with and without the session id.
    MissionPlayingEvent s_Event;
    s_Event.scene_resource = "assembly:/paris.entity";
    s_Event.scene_type = "mission";
    s_Event.codename_hint = "Peacock";
    CHECK(RelaySerialization::MissionPlayingPayload(s_Event)
        == "{\"scene_resource\":\"assembly:/paris.entity\",\"scene_type\":\"mission\",\"codename_hint\":\"Peacock\"}");
    s_Event.game_session_id = "2516109819408417528-6f46ac15-6033-4821-9d65-5ed7659392bb";
    CHECK(RelaySerialization::MissionPlayingPayload(s_Event)
        == "{\"scene_resource\":\"assembly:/paris.entity\",\"scene_type\":\"mission\",\"codename_hint\":\"Peacock\","
           "\"game_session_id\":\"2516109819408417528-6f46ac15-6033-4821-9d65-5ed7659392bb\"}");

    // Envelope, exact text.
    RelayEnvelope s_Envelope;
    s_Envelope.adapter_instance_id = "00000000-0000-4000-8000-000000000001";
    s_Envelope.sequence = 7;
    s_Envelope.timestamp = "2026-10-06T20:34:34.787Z";
    s_Envelope.event_type = "mission.playing";
    s_Envelope.schema_version = 1;
    s_Envelope.payload_json = "{\"k\":\"v\"}";
    CHECK(RelaySerialization::Envelope(s_Envelope)
        == "{\"protocol_version\":1,\"adapter_instance_id\":\"00000000-0000-4000-8000-000000000001\",\"sequence\":7,"
           "\"timestamp\":\"2026-10-06T20:34:34.787Z\",\"event_type\":\"mission.playing\",\"schema_version\":1,"
           "\"payload\":{\"k\":\"v\"}}");

    // Adapter: sequence, instance id, injected clock, sink receives owned values only.
    auto s_Sink = std::make_unique<CapturingSink>();
    auto* s_SinkView = s_Sink.get();
    int s_Ticks = 0;
    RelayAdapter s_Adapter(std::move(s_Sink), "id-1", [&] { return fmt::format("t{}", ++s_Ticks); });

    s_Adapter.Publish(s_Event);
    s_Adapter.Publish(s_Event);
    CHECK(s_Adapter.PublishedCount() == 2);
    CHECK(s_SinkView->Published.size() == 2);
    CHECK(s_SinkView->Published[0].sequence == 1);
    CHECK(s_SinkView->Published[1].sequence == 2);
    CHECK(s_SinkView->Published[0].event_type == "mission.playing");
    CHECK(s_SinkView->Published[1].json.find("\"sequence\":2,\"timestamp\":\"t2\"") != std::string::npos);
    CHECK(s_SinkView->Published[1].json.find("\"adapter_instance_id\":\"id-1\"") != std::string::npos);
    CHECK(s_SinkView->Published[1].json.find('\n') == std::string::npos);

    // Instance ids: UUID shape, version 4, distinct.
    const auto s_IdA = RelayAdapter::NewInstanceId();
    const auto s_IdB = RelayAdapter::NewInstanceId();
    CHECK(s_IdA.size() == 36);
    CHECK(s_IdA[8] == '-' && s_IdA[13] == '-' && s_IdA[18] == '-' && s_IdA[23] == '-');
    CHECK(s_IdA[14] == '4');
    CHECK(s_IdA[19] == '8' || s_IdA[19] == '9' || s_IdA[19] == 'a' || s_IdA[19] == 'b');
    CHECK(s_IdA != s_IdB);

    // Clock: ISO 8601 UTC shape.
    const auto s_Now = RelayAdapter::UtcNow();
    CHECK(s_Now.size() == 24);
    CHECK(s_Now[10] == 'T' && s_Now[23] == 'Z' && s_Now[19] == '.');
}
