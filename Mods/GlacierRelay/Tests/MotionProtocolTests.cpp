// M7-B2: the motion link's protocol (MotionProtocol; glacier-relay Motion.Codec, design section 15.5),
// the motion attestation MAC against BEAM's own Schema.motion_attest_mac/5, the two motion-gated
// command entries' decoding (Command.Schema's bounds exactly) and the command link's advertisement
// with the [motion] setting on and off (off: the hello is exactly M6's).

#include <chrono>
#include <cstdio>
#include <string>

#include "Attestation.h"
#include "CommandJson.h"
#include "CommandLink.h"
#include "MotionProtocol.h"
#include "TestHarness.h"
#include "TestListener.h"

using namespace CommandProtocol;

namespace
{
    constexpr const char* k_Uuid = "6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f";

    bool Contains(const std::string& p_Text, const std::string& p_Part)
    {
        return p_Text.find(p_Part) != std::string::npos;
    }

    std::string Line(const std::string& p_Body)
    {
        return "{\"motion_protocol_version\":1," + p_Body + "}";
    }

    void Codec()
    {
        // BEAM -> native: each kind, exactly its fields.
        {
            const auto s_Welcome = MotionProtocol::DecodeFromBeam(Line("\"kind\":\"motion.welcome\",\"motion_link_generation\":7,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\""));
            CHECK(s_Welcome.ok && s_Welcome.message.kind == MotionProtocol::Kind::Welcome && s_Welcome.message.motion_link_generation == 7 && s_Welcome.message.nonce == "514f1d221aefccd9d7fc4193040369cf");
            const auto s_Target = MotionProtocol::DecodeFromBeam(Line("\"kind\":\"ghost.target\",\"session_id\":\"" + std::string(k_Uuid) + "\",\"seq\":3,\"pos\":\"c2b2fae1,416d47ae\""));
            CHECK(s_Target.ok && s_Target.message.kind == MotionProtocol::Kind::Target && s_Target.message.seq == 3 && s_Target.message.x == 0xc2b2fae1u && s_Target.message.y == 0x416d47aeu && s_Target.message.id == k_Uuid);
            const auto s_Close = MotionProtocol::DecodeFromBeam(Line("\"kind\":\"ghost.close\",\"session_id\":\"" + std::string(k_Uuid) + "\",\"reason\":\"operator\""));
            CHECK(s_Close.ok && s_Close.message.kind == MotionProtocol::Kind::Close && s_Close.message.reason == "operator");
            const auto s_ShareClose = MotionProtocol::DecodeFromBeam(Line("\"kind\":\"share.close\",\"share_id\":\"" + std::string(k_Uuid) + "\",\"reason\":\"duration\""));
            CHECK(s_ShareClose.ok && s_ShareClose.message.kind == MotionProtocol::Kind::ShareClose);
            const auto s_Refused = MotionProtocol::DecodeFromBeam(Line("\"kind\":\"motion.refused\",\"reason\":\"command_link_not_token_verified\""));
            CHECK(s_Refused.ok && s_Refused.message.kind == MotionProtocol::Kind::Refused && s_Refused.message.reason == "command_link_not_token_verified");
        }

        // Strictness: nothing unknown, nothing missing, nothing out of bounds becomes a message.
        const std::string s_Id = k_Uuid;
        const char* s_Bad[] = {
            "{\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"x\"}",                         // no version
            "{\"motion_protocol_version\":2,\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"x\"}", // another version
            "{\"motion_protocol_version\":\"1\",\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"x\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.teleport\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\"}",          // unknown kind
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"x\",\"extend_ms\":1}", // unknown field
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\"}",            // missing reason
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":null,\"reason\":\"x\"}",                                 // null required
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"6B9C8F0E-3D1A-4C2B-9E7F-0A1B2C3D4E5F\",\"reason\":\"x\"}", // uppercase uuid
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":0,\"pos\":\"c2b2fae1,416d47ae\"}",  // seq 0
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":1.5,\"pos\":\"c2b2fae1,416d47ae\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":1,\"pos\":\"c2b2fae1,416d47ae,bfc2d6ba\"}", // three words
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":1,\"pos\":\"7f800000,416d47ae\"}",  // infinity
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":1,\"pos\":\"C2B2FAE1,416d47ae\"}",  // uppercase word
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":1,\"pos\":\"-40.5,-24.2\"}",          // decimals
            "{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":0,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":2147483648,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":1,\"nonce\":\"514F1D221AEFCCD9D7FC4193040369CF\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":1,\"nonce\":\"514f1d22\"}",
            "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e50\",\"reason\":\"x\"}", // duplicate key
            "[1,2]",
            "not json",
            "",
        };
        int s_Index = 0;
        for (const char* s_Text : s_Bad)
        {
            const auto s_Decoded = MotionProtocol::DecodeFromBeam(s_Text);
            if (s_Decoded.ok)
                std::printf("  motion decode accepted bad line %d: %s\n", s_Index, s_Text);
            CHECK(!s_Decoded.ok && !s_Decoded.error.empty());
            ++s_Index;
        }
        // the edges that are valid: the largest generation; a 64-byte reason; a 65-byte one is not
        CHECK(MotionProtocol::DecodeFromBeam(Line("\"kind\":\"motion.welcome\",\"motion_link_generation\":2147483647,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\"")).ok);
        CHECK(MotionProtocol::DecodeFromBeam(Line("\"kind\":\"ghost.close\",\"session_id\":\"" + s_Id + "\",\"reason\":\"" + std::string(64, 'r') + "\"")).ok);
        CHECK(!MotionProtocol::DecodeFromBeam(Line("\"kind\":\"ghost.close\",\"session_id\":\"" + s_Id + "\",\"reason\":\"" + std::string(65, 'r') + "\"")).ok);
        // the line bound: 1,024 bytes
        const std::string s_Padded = Line("\"kind\":\"ghost.close\",\"session_id\":\"" + s_Id + "\",\"reason\":\"r\"");
        CHECK(!MotionProtocol::DecodeFromBeam(s_Padded + std::string(1025 - s_Padded.size(), ' ')).ok);

        // Native -> BEAM: exactly BEAM's field names and order-independent shapes; null where BEAM
        // treats a field as optional.
        const uint32_t s_Pos[3] = {0xc2a77ccfu, 0x416b27e8u, 0xbfc2d6bau};
        CHECK(MotionProtocol::EncodeHello("aaf540dd-c6df-4429-8fa5-84bc8933a13e", 7) == "{\"motion_protocol_version\":1,\"kind\":\"motion.hello\",\"adapter_instance_id\":\"aaf540dd-c6df-4429-8fa5-84bc8933a13e\",\"command_link_generation\":7}\n");
        CHECK(MotionProtocol::EncodeHeroSample(k_Uuid, 3, 1200, 5000, s_Pos, false) == "{\"motion_protocol_version\":1,\"kind\":\"hero.sample\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":3,\"frame\":1200,\"native_mono_ms\":5000,\"pos\":\"c2a77ccf,416b27e8,bfc2d6ba\",\"dirty\":false}\n");
        CHECK(MotionProtocol::EncodeShareClosed(k_Uuid, "duration", std::nullopt, 0) == "{\"motion_protocol_version\":1,\"kind\":\"share.closed\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"duration\",\"last_seq\":null,\"sent\":0}\n");
        MotionProtocol::Applied s_Applied;
        s_Applied.session_id = k_Uuid;
        s_Applied.seq = 4;
        s_Applied.frame = 9;
        s_Applied.native_mono_ms = 77;
        s_Applied.outcome = MotionProtocol::Outcome::Applied;
        s_Applied.post_x = 0xc2b2fae1u;
        s_Applied.post_y = 0x416d47aeu;
        s_Applied.exact = true;
        CHECK(MotionProtocol::EncodeApplied(s_Applied) == "{\"motion_protocol_version\":1,\"kind\":\"ghost.applied\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"seq\":4,\"frame\":9,\"native_mono_ms\":77,\"outcome\":\"applied\",\"reason\":null,\"post\":\"c2b2fae1,416d47ae\",\"exact\":true}\n");
        MotionProtocol::Closed s_Closed;
        s_Closed.session_id = k_Uuid;
        s_Closed.reason = "beam:" + std::string(70, 'z'); // bounded to 64 bytes
        s_Closed.clean = false;
        CHECK(Contains(MotionProtocol::EncodeClosed(s_Closed), "\"reason\":\"beam:" + std::string(59, 'z') + "\",") && Contains(MotionProtocol::EncodeClosed(s_Closed), "\"final_world\":null"));
        CHECK(MotionProtocol::ReasonText(std::string(63, 'a') + "\xc3\xa9") == std::string(63, 'a')); // never cut inside a UTF-8 sequence

        // The worst case of every native line within BEAM's 1,024-byte bound.
        for (const char* s_Kind : {"motion.hello", "motion.attest", "hero.sample", "share.closed", "ghost.applied", "ghost.report", "ghost.closed"})
        {
            const size_t s_Bytes = MotionProtocol::WorstCaseLineBytes(s_Kind);
            std::printf("line bound: motion %s worst case = %zu bytes of %zu\n", s_Kind, s_Bytes, MotionProtocol::k_MaxLineBytes);
            CHECK(s_Bytes > 0 && s_Bytes <= MotionProtocol::k_MaxLineBytes);
        }
    }

    void Mac()
    {
        // The cross-side vector: BEAM's Schema.motion_attest_mac/5 (Erlang :crypto) over the same
        // inputs with the same test-only token produced exactly this MAC (M7-B2, computed with
        // `mix run` from the relay tree at b6d4b4f).
        const std::string s_Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string s_Nonce = "514f1d221aefccd9d7fc4193040369cf";
        const std::string s_Adapter = "aaf540dd-c6df-4429-8fa5-84bc8933a13e";
        CHECK(Attestation::MotionCanonicalInput(s_Nonce, 3, 7, s_Adapter) == "motion|514f1d221aefccd9d7fc4193040369cf|3|7|aaf540dd-c6df-4429-8fa5-84bc8933a13e");
        CHECK(Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter) == "58a550cb13e3a8f4aca3cffac2239aa769a57c038ab9e68658c041df144b5144");
        // domain separation: neither MAC answers the other's challenge; every input binds
        CHECK(Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter) != Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter));
        CHECK(Attestation::MotionAttestMac(s_Token, s_Nonce, 4, 7, s_Adapter) != Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter));
        CHECK(Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 8, s_Adapter) != Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter));
        CHECK(Attestation::MotionAttestMac(s_Token, "614f1d221aefccd9d7fc4193040369cf", 3, 7, s_Adapter) != Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter));
        CHECK(Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, "aaf540dd-c6df-4429-8fa5-84bc8933a13f") != Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter));
        const std::string s_Attest = MotionProtocol::EncodeAttest(3, 7, s_Adapter, Attestation::MotionAttestMac(s_Token, s_Nonce, 3, 7, s_Adapter));
        CHECK(s_Attest == "{\"motion_protocol_version\":1,\"kind\":\"motion.attest\",\"motion_link_generation\":3,\"command_link_generation\":7,\"adapter_instance_id\":\"aaf540dd-c6df-4429-8fa5-84bc8933a13e\",\"mac\":\"58a550cb13e3a8f4aca3cffac2239aa769a57c038ab9e68658c041df144b5144\"}\n");
        CHECK(!Contains(s_Attest, s_Token));
    }

    std::string CommandLine(const std::string& p_Name, const std::string& p_Params, const std::string& p_Preconditions = "{\"mission_generation\":\"gen-a\"}")
    {
        return "{\"command_protocol_version\":2,\"kind\":\"command\",\"command_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-000000000001\",\"link_generation\":3,\"issued_at\":\"t\",\"ttl_ms\":5000,\"name\":\"" + p_Name
            + "\",\"version\":1,\"params\":" + p_Params + ",\"preconditions\":" + p_Preconditions + "}";
    }

    const std::string k_World = "3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,c2b2fae1,416d47ae,bfc2d6ba";
    const std::string k_Local = "c2b2fae1,416d47ae,bfc2d6ba,80000000,00000000,80000000,3f800000";
    const std::string k_Preimage = "3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,c2b2fae2,416d47ae,bfc2d6ba";

    std::string OpenParams(const std::string& p_Drop = std::string(), const std::string& p_Extra = std::string())
    {
        std::string s_Params = "{\"mode\":\"open\",\"session_id\":\"" + std::string(k_Uuid) + "\",\"motion_link_generation\":5,\"expect_world\":\"" + k_World + "\",\"expect_local\":\"" + k_Local
            + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"duration_ms\":120000,\"half_extent_mm\":8000,\"max_speed_mmps\":2000,\"credit_ms\":100,\"min_apply_interval_ms\":40,\"silence_close_ms\":1000" + p_Extra + "}";
        if (!p_Drop.empty())
        {
            // remove ,"name":value — a quoted value may itself contain commas (the word groups)
            const size_t s_At = s_Params.find(",\"" + p_Drop + "\":");
            const size_t s_Value = s_At + p_Drop.size() + 4;
            const size_t s_End = s_Params[s_Value] == '"' ? s_Params.find('"', s_Value + 1) + 1 : s_Params.find_first_of(",}", s_Value);
            s_Params.erase(s_At, s_End - s_At);
        }
        return s_Params;
    }

    void Entries()
    {
        DecodeOptions s_On;
        s_On.protocol_version = 2;
        s_On.effectful = true;
        s_On.motion = true;
        DecodeOptions s_NoMotion = s_On;
        s_NoMotion.motion = false;
        DecodeOptions s_NoEffect = s_On;
        s_NoEffect.effectful = false;

        // player.pose.share/1
        const std::string s_Share = "{\"mode\":\"open\",\"share_id\":\"" + std::string(k_Uuid) + "\",\"motion_link_generation\":5,\"duration_ms\":1000,\"interval_ms\":50}";
        {
            const auto s_Ok = DecodeCommand(CommandLine("player.pose.share", s_Share), s_NoEffect);
            CHECK(s_Ok.ok && s_Ok.command.entry == Entry::PlayerPoseShare1 && s_Ok.command.share && s_Ok.command.share->interval_ms == 50 && s_Ok.command.share->motion_link_generation == 5);
            CHECK(!IsEffectful(Entry::PlayerPoseShare1));
            const auto s_Off = DecodeCommand(CommandLine("player.pose.share", s_Share), s_NoMotion);
            CHECK(!s_Off.ok && Contains(s_Off.reason, "motion setting disabled") && s_Off.command_id == "aaaaaaaa-aaaa-4aaa-8aaa-000000000001");
            const char* s_BadShares[] = {
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":999,\"interval_ms\":50}",
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":300001,\"interval_ms\":50}",
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":1000,\"interval_ms\":49}",
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":1000,\"interval_ms\":1001}",
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":0,\"duration_ms\":1000,\"interval_ms\":50}",
                "{\"mode\":\"close\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":1000,\"interval_ms\":50}",
                "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":5,\"duration_ms\":1000,\"interval_ms\":50,\"peer\":\"x\"}",
            };
            for (const char* s_Params : s_BadShares)
                CHECK(!DecodeCommand(CommandLine("player.pose.share", s_Params), s_On).ok);
            CHECK(!DecodeCommand(CommandLine("player.pose.share", s_Share, "{}"), s_On).ok); // the mission_generation precondition is mandatory
            // the edges that are valid
            CHECK(DecodeCommand(CommandLine("player.pose.share", "{\"mode\":\"open\",\"share_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"motion_link_generation\":2147483647,\"duration_ms\":300000,\"interval_ms\":1000}"), s_On).ok);
        }

        // candidate.motion.session/1
        {
            const auto s_Open = DecodeCommand(CommandLine("candidate.motion.session", OpenParams()), s_On);
            CHECK(s_Open.ok && s_Open.command.entry == Entry::CandidateMotionSession1 && s_Open.command.motion && s_Open.command.motion->mode == MotionSessionMode::Open);
            CHECK(s_Open.ok && s_Open.command.motion->limits.silence_close_ms == 1000 && s_Open.command.motion->limits.min_apply_interval_ms == 40 && *s_Open.command.motion->motion_link_generation == 5);
            CHECK(IsEffectful(Entry::CandidateMotionSession1));
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", OpenParams()), s_NoMotion).ok);
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", OpenParams()), s_NoEffect).ok);
            // every limit is required on an open; each bound exactly
            for (const char* s_Limit : {"motion_link_generation", "duration_ms", "half_extent_mm", "max_speed_mmps", "credit_ms", "min_apply_interval_ms", "silence_close_ms", "expect_world", "expect_local", "expect_parent"})
            {
                const auto s_Missing = DecodeCommand(CommandLine("candidate.motion.session", OpenParams(s_Limit)), s_On);
                if (s_Missing.ok)
                    std::printf("  open without %s accepted\n", s_Limit);
                CHECK(!s_Missing.ok);
            }
            struct Bound { const char* name; int below; int above; int min; int max; };
            const Bound s_Bounds[] = {{"duration_ms", 999, 300001, 1000, 300000}, {"half_extent_mm", 99, 8001, 100, 8000}, {"max_speed_mmps", 99, 3001, 100, 3000}, {"credit_ms", 49, 201, 50, 200}, {"min_apply_interval_ms", 39, 1001, 40, 1000}, {"silence_close_ms", 249, 2001, 250, 2000}};
            for (const auto& s_Bound : s_Bounds)
            {
                auto s_With = [&](int p_Value) { return DecodeCommand(CommandLine("candidate.motion.session", OpenParams(s_Bound.name, ",\"" + std::string(s_Bound.name) + "\":" + std::to_string(p_Value))), s_On); };
                const bool s_Ok = !s_With(s_Bound.below).ok && !s_With(s_Bound.above).ok && s_With(s_Bound.min).ok && s_With(s_Bound.max).ok;
                if (!s_Ok)
                    std::printf("  limit bounds of %s not exact\n", s_Bound.name);
                CHECK(s_Ok);
            }
            // an open names no target
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", OpenParams(std::string(), ",\"target_world\":\"" + k_Preimage + "\"")), s_On).ok);
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", OpenParams(std::string(), ",\"restore_of\":\"" + std::string(k_Uuid) + "\"")), s_On).ok);
            // unknown parameters are never execution
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", OpenParams(std::string(), ",\"mapping\":\"identity\"")), s_On).ok);

            // restore: the target and restore_of, no limit and no link generation; a no-op is refused
            const std::string s_Restore = "{\"mode\":\"restore\",\"session_id\":\"" + std::string(k_Uuid) + "\",\"expect_world\":\"" + k_World + "\",\"expect_local\":\"" + k_Local
                + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"target_world\":\"" + k_Preimage + "\",\"restore_of\":\"aaaaaaaa-aaaa-4aaa-8aaa-000000000009\"}";
            const auto s_Ok = DecodeCommand(CommandLine("candidate.motion.session", s_Restore), s_On);
            CHECK(s_Ok.ok && s_Ok.command.motion->mode == MotionSessionMode::Restore && s_Ok.command.motion->restore_of && *s_Ok.command.motion->restore_of == "aaaaaaaa-aaaa-4aaa-8aaa-000000000009");
            std::string s_WithLimit = s_Restore;
            s_WithLimit.insert(s_WithLimit.size() - 1, ",\"duration_ms\":1000");
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", s_WithLimit), s_On).ok);
            std::string s_WithGeneration = s_Restore;
            s_WithGeneration.insert(s_WithGeneration.size() - 1, ",\"motion_link_generation\":5");
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", s_WithGeneration), s_On).ok);
            std::string s_NoOp = s_Restore;
            s_NoOp.replace(s_NoOp.find(k_Preimage), k_Preimage.size(), k_World);
            const auto s_NoOpDecoded = DecodeCommand(CommandLine("candidate.motion.session", s_NoOp), s_On);
            CHECK(!s_NoOpDecoded.ok && Contains(s_NoOpDecoded.reason, "no-op"));
            std::string s_NoTarget = s_Restore;
            s_NoTarget.erase(s_NoTarget.find(",\"target_world\""), std::string(",\"target_world\":\"").size() + k_Preimage.size() + 1);
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", s_NoTarget), s_On).ok);
            CHECK(!DecodeCommand(CommandLine("candidate.motion.session", s_Restore, "{}"), s_On).ok);
        }

        // Neither entry exists at version 2.
        CHECK(Lookup("candidate.motion.session", 2) == Entry::Unknown && Lookup("player.pose.share", 2) == Entry::Unknown);
    }

    uint64_t MonoMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // The hello a command link sends with the given settings (read by a stand-in listener).
    std::string Hello(bool p_Motion, bool p_Effectful, bool p_Token)
    {
        CommandTest::Listener s_Beam;
        if (!s_Beam.Start(0))
            return "";
        CommandQueue s_Queue;
        CommandLink::Options s_Options;
        s_Options.port = s_Beam.Port;
        s_Options.connect_timeout_ms = 500;
        s_Options.welcome_timeout_ms = 1000;
        s_Options.backoff_initial_ms = 50;
        s_Options.backoff_max_ms = 200;
        s_Options.effectful = p_Effectful;
        s_Options.motion = p_Motion;
        s_Options.token = p_Token ? "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" : "";
        std::string s_Hello;
        {
            CommandLink s_Link(s_Options, s_Queue, "aaf540dd-c6df-4429-8fa5-84bc8933a13e", MonoMs);
            for (int s_Attempt = 0; s_Attempt < 20 && s_Hello.empty(); ++s_Attempt)
            {
                if (!s_Beam.Accept(3000))
                    break;
                s_Hello = s_Beam.ReadLine(1500);
                if (s_Hello.empty())
                    s_Beam.CloseClient();
            }
            s_Beam.CloseClient();
        }
        s_Beam.Stop();
        return s_Hello;
    }

    void Advertisement()
    {
        WSADATA s_Data;
        WSAStartup(MAKEWORD(2, 2), &s_Data);

        const std::string s_M6 = Hello(false, true, true);
        const std::string s_ShareOnly = Hello(true, false, true);
        const std::string s_Both = Hello(true, true, true);
        const std::string s_NoToken = Hello(true, true, false);

        CHECK(!s_M6.empty() && !Contains(s_M6, "player.pose.share") && !Contains(s_M6, "candidate.motion.session") && Contains(s_M6, "candidate.transform.set/2"));
        CHECK(Contains(s_ShareOnly, "\"player.pose.share/1\"") && !Contains(s_ShareOnly, "candidate.motion.session") && !Contains(s_ShareOnly, "candidate.transform.set"));
        CHECK(Contains(s_Both, "\"player.pose.share/1\"") && Contains(s_Both, "\"candidate.motion.session/1\""));
        CHECK(!s_NoToken.empty() && !Contains(s_NoToken, "player.pose.share") && !Contains(s_NoToken, "candidate.motion.session")); // version 1: never
        // with motion off the hello carries exactly M6's capability list, in M6's order
        CHECK(Contains(s_M6, "\"capabilities\":[\"relay.probe/1\",\"relay.inspect.outfit/1\",\"relay.inspect.players/1\",\"relay.inspect.entity/1\",\"relay.inspect.candidate/1\",\"relay.inspect.candidate_transform/1\",\"player.outfit.set/2\",\"candidate.transform.set/1\",\"candidate.transform.set/2\"]}"));

        WSACleanup();
    }
}

void RunMotionProtocolTests()
{
    Codec();
    Mac();
    Entries();
    Advertisement();
}
