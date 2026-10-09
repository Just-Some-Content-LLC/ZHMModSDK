#include "TestHarness.h"

#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Fixtures/B0Disguise.h"
#include "Fixtures/B0DisguiseBytes.h"
#include "RelayAdapter.h"
#include "RelayEnvelope.h"
#include "RelayFrame.h"
#include "RepositoryId.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

// M2 B3 intake correction (design section 35): the engine-independent RepositoryId conversion the
// Glacier-facing intake renders a ZRepositoryID through. Known-answer vectors independent of the
// corpus, the B0 outfit ids from their 16-byte images, and the normalizer/frame path fed with values
// that went through that conversion rather than being lifted from the JSON corpus.
//
// What this covers offline: the field-to-text rendering and the little-endian image-to-field
// decoding, i.e. everything after the SDK field read. What it cannot cover: that the engine's
// ZRepositoryID at runtime has the layout the SDK declares (pinned by static_assert in
// TelemetryIntake.cpp, shown only by a run) and that the engine's own JSON writer renders the same
// text (expected from B0; shown only by a run).
namespace
{
    using Bytes = std::array<uint8_t, RepositoryId::k_Bytes>;

    bool IsDashedLowercase(const std::string& p_Text)
    {
        if (p_Text.size() != RepositoryId::k_TextLength)
            return false;

        for (size_t i = 0; i < p_Text.size(); ++i)
        {
            const bool s_Dash = i == 8 || i == 13 || i == 18 || i == 23;
            const unsigned char c = static_cast<unsigned char>(p_Text[i]);

            if (s_Dash ? c != '-' : !(std::isxdigit(c) && !std::isupper(c)))
                return false;
        }

        return true;
    }

    // The observation the intake would produce at runtime for one recorded disguise event: the
    // envelope fields from the recorded JSON, the Value from the 16-byte image through the
    // production conversion. The JSON's own "Value" string is deliberately discarded.
    TelemetryObservation DisguiseFromBytes(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Disguise[p_Index];
        auto s_Obs = TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
        s_Obs.value = TelemetryValue{};
        s_Obs.value.kind = TelemetryValue::Kind::String;
        s_Obs.value.text = RepositoryId::FromLittleEndianBytes(B0Fixtures::k_DisguiseImages[p_Index]->le_bytes).ToDashedLowercase();
        return s_Obs;
    }

    TelemetryObservation DisguiseFromJson(size_t p_Index)
    {
        const auto& s_Raw = B0Fixtures::k_Disguise[p_Index];
        return TestJson::ObservationFromRecordedEvent(s_Raw.json, static_cast<uint32_t>(s_Raw.event_index));
    }

    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };

    SceneState Playing()
    {
        SceneState s_Scene;
        s_Scene.available = true;
        s_Scene.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        s_Scene.scene_type = "mission";
        s_Scene.codename_hint = "Peacock";
        s_Scene.loading_stage = 8;
        s_Scene.scene_loaded = true;
        return s_Scene;
    }
}

void RunRepositoryIdTests()
{
    // Known-answer vector, independent of the corpus: the little-endian image of the GUID
    // 00112233-4455-6677-8899-aabbccddeeff, both as raw bytes and as typed fields.
    {
        const Bytes s_Raw = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        const RepositoryId s_FromRaw = RepositoryId::FromLittleEndianBytes(s_Raw);

        RepositoryId s_Typed;
        s_Typed.data1 = 0x00112233;
        s_Typed.data2 = 0x4455;
        s_Typed.data3 = 0x6677;
        s_Typed.data4 = {0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

        CHECK(s_FromRaw == s_Typed);
        CHECK(s_FromRaw.data1 == 0x00112233u && s_FromRaw.data2 == 0x4455u && s_FromRaw.data3 == 0x6677u);
        CHECK(s_FromRaw.ToDashedLowercase() == "00112233-4455-6677-8899-aabbccddeeff");
        CHECK(s_Typed.ToDashedLowercase() == "00112233-4455-6677-8899-aabbccddeeff");
        CHECK(s_Typed.ToDashedLowercase().size() == RepositoryId::k_TextLength);

        // Not the memory image hex-printed in order: the first three fields are byte-swapped.
        CHECK(s_FromRaw.ToDashedLowercase() != "33221100-5544-7766-8899-aabbccddeeff");
    }

    // Leading zeros are kept in every field; nothing is trimmed.
    {
        const Bytes s_Raw = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05};
        CHECK(RepositoryId::FromLittleEndianBytes(s_Raw).ToDashedLowercase() == "00000001-0002-0003-0004-000000000005");

        const Bytes s_Zero{};
        CHECK(RepositoryId::FromLittleEndianBytes(s_Zero).ToDashedLowercase() == "00000000-0000-0000-0000-000000000000");
        CHECK(RepositoryId{}.ToDashedLowercase() == "00000000-0000-0000-0000-000000000000");
    }

    // High-bit bytes: no sign extension, no uppercase.
    {
        Bytes s_All;
        s_All.fill(0xff);
        CHECK(RepositoryId::FromLittleEndianBytes(s_All).ToDashedLowercase() == "ffffffff-ffff-ffff-ffff-ffffffffffff");

        const Bytes s_Sign = {0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80};
        const auto s_Id = RepositoryId::FromLittleEndianBytes(s_Sign);
        CHECK(s_Id.data1 == 0x80000000u && s_Id.data2 == 0x8000u && s_Id.data3 == 0x8000u);
        CHECK(s_Id.ToDashedLowercase() == "80000000-8000-8000-8000-800000000080");

        RepositoryId s_Typed;
        s_Typed.data1 = 0xdeadbeef;
        s_Typed.data2 = 0xcafe;
        s_Typed.data3 = 0xf00d;
        s_Typed.data4 = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
        CHECK(s_Typed.ToDashedLowercase() == "deadbeef-cafe-f00d-fedc-ba9876543210");
    }

    // The B0 outfit ids from their 16-byte images render to the corpus text exactly; every rendering
    // is 36 dashed lowercase characters.
    {
        const B0Fixtures::RepositoryIdImage* s_Images[] = {&B0Fixtures::k_SuitImage, &B0Fixtures::k_OutfitAImage, &B0Fixtures::k_OutfitBImage};

        for (const auto* s_Image : s_Images)
        {
            const std::string s_Text = RepositoryId::FromLittleEndianBytes(s_Image->le_bytes).ToDashedLowercase();
            CHECK(s_Text == s_Image->dashed);
            CHECK(IsDashedLowercase(s_Text));
        }

        // Typed construction of the suit id agrees with its raw image.
        RepositoryId s_Suit;
        s_Suit.data1 = 0x874c4c48;
        s_Suit.data2 = 0x0a8b;
        s_Suit.data3 = 0x49e9;
        s_Suit.data4 = {0x88, 0x3e, 0x49, 0xfc, 0x5f, 0x1f, 0xb0, 0x51};
        CHECK(s_Suit == RepositoryId::FromLittleEndianBytes(B0Fixtures::k_SuitImage.le_bytes));
        CHECK(s_Suit.ToDashedLowercase() == "874c4c48-0a8b-49e9-883e-49fc5f1fb051");

        // Each of the 8 recorded events' image renders to that event's recorded Value.
        CHECK(B0Fixtures::k_DisguiseImageCount == B0Fixtures::k_DisguiseCount);

        for (size_t i = 0; i < B0Fixtures::k_DisguiseCount; ++i)
        {
            const auto s_Json = DisguiseFromJson(i);
            CHECK(s_Json.value.kind == TelemetryValue::Kind::String);
            CHECK(RepositoryId::FromLittleEndianBytes(B0Fixtures::k_DisguiseImages[i]->le_bytes).ToDashedLowercase() == s_Json.value.text);
        }
    }

    // Normalizer path: an observation whose Value came through the byte conversion normalizes to
    // the same DisguiseEvent as the JSON-corpus observation, for all 8 recorded events, and the
    // wire payload carries the rendered id.
    {
        TelemetryNormalizer s_FromBytes;
        TelemetryNormalizer s_FromJson;

        for (size_t i = 0; i < B0Fixtures::k_DisguiseCount; ++i)
        {
            const auto s_ByteResult = s_FromBytes.Normalize(DisguiseFromBytes(i));
            const auto s_JsonResult = s_FromJson.Normalize(DisguiseFromJson(i));
            CHECK(s_ByteResult.outcome == TelemetryNormalizer::Outcome::Normalized);
            CHECK(s_ByteResult.gating == TelemetryNormalizer::Gating::AttemptGated);
            CHECK(s_ByteResult.disguise.has_value() && s_JsonResult.disguise.has_value());
            CHECK(*s_ByteResult.disguise == *s_JsonResult.disguise);
            CHECK(s_ByteResult.disguise->disguise_repository_id == B0Fixtures::k_DisguiseImages[i]->dashed);
        }

        CHECK(s_FromBytes.GetCounters().normalized == B0Fixtures::k_DisguiseCount);
        CHECK(s_FromBytes.GetCounters().malformed == 0);

        const auto s_Initial = *s_FromBytes.Normalize(DisguiseFromBytes(B0Fixtures::k_StartingSuitA)).disguise;
        CHECK(RelaySerialization::DisguisePayloadJson(s_Initial).find("\"disguise_repository_id\":\"874c4c48-0a8b-49e9-883e-49fc5f1fb051\"") != std::string::npos);
    }

    // The Unsupported record the B3 and type-discovery runs produced (kind=Unsupported,
    // text="ZRepositoryID") is still rejected: the correction is in the intake, not a relaxation
    // of the normalizer. ZGuid by name is likewise not a string.
    {
        TelemetryNormalizer s_Normalizer;

        auto s_Obs = DisguiseFromJson(B0Fixtures::k_DisguiseBlown1);
        s_Obs.value = TelemetryValue{};
        s_Obs.value.kind = TelemetryValue::Kind::Unsupported;
        s_Obs.value.text = "ZRepositoryID";
        CHECK(s_Normalizer.Normalize(s_Obs).outcome == TelemetryNormalizer::Outcome::Malformed);

        s_Obs.value.text = "ZGuid";
        CHECK(s_Normalizer.Normalize(s_Obs).outcome == TelemetryNormalizer::Outcome::Malformed);
        CHECK(s_Normalizer.GetCounters().malformed == 2);
    }

    // Frame path: the B0 session-1 disguise order with byte-built values, through the production
    // sequencing, publishes the three types with the rendered ids in the shared sequence.
    {
        auto s_Sink = std::make_unique<CapturingSink>();
        auto* s_Captured = s_Sink.get();
        int s_Ticks = 0;
        RelayAdapter s_Adapter(std::move(s_Sink), "id-1", [&] { return fmt::format("t{}", ++s_Ticks); });
        TelemetryQueue s_Queue{256};
        TelemetryNormalizer s_Normalizer;
        MissionObserver s_Observer;
        std::vector<std::string> s_Warnings;

        auto s_Frame = [&] {
            return RelayFrame::Process(
                s_Queue, s_Normalizer, s_Observer, &s_Adapter, Playing(), std::nullopt,
                [&](const std::string& p_Line) { s_Warnings.push_back(p_Line); }
            );
        };

        s_Frame(); // mission.playing

        const size_t s_Order[] = {
            B0Fixtures::k_StartingSuitA, B0Fixtures::k_DisguiseChange1, B0Fixtures::k_DisguiseBlown1,
            B0Fixtures::k_DisguiseCleared1, B0Fixtures::k_DisguiseChange2, B0Fixtures::k_DisguiseBlown2,
            B0Fixtures::k_DisguiseCleared2,
        };

        for (const auto s_Index : s_Order)
            CHECK(s_Queue.Push(DisguiseFromBytes(s_Index)));

        const auto s_Drain = s_Frame();
        CHECK(s_Drain.outcomes_published == 7 && s_Drain.malformed == 0 && s_Drain.outside_attempt == 0);
        CHECK(s_Warnings.empty());
        CHECK(s_Captured->Published.size() == 8);

        const std::vector<std::string> s_ExpectedTypes = {
            "mission.playing", "disguise.equipped", "disguise.equipped", "disguise.compromised",
            "disguise.compromise_cleared", "disguise.equipped", "disguise.compromised", "disguise.compromise_cleared",
        };

        for (size_t i = 0; i < s_Captured->Published.size(); ++i)
        {
            CHECK(s_Captured->Published[i].event_type == s_ExpectedTypes[i]);
            CHECK(s_Captured->Published[i].sequence == i + 1);
        }

        for (size_t i = 0; i < 7; ++i)
        {
            const std::string s_Expected = fmt::format("\"disguise_repository_id\":\"{}\"", B0Fixtures::k_DisguiseImages[s_Order[i]]->dashed);
            CHECK(s_Captured->Published[i + 1].json.find(s_Expected) != std::string::npos);
        }
    }
}
