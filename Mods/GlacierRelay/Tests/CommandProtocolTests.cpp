#include "TestHarness.h"

#include <string>

#include "CommandJson.h"
#include "CommandProtocol.h"
#include "TestListener.h"

#include "Attestation.h"

using namespace CommandProtocol;

namespace
{
    bool Contains(const std::string& p_Text, const std::string& p_Needle)
    {
        return p_Text.find(p_Needle) != std::string::npos;
    }
}

void RunCommandProtocolTests()
{
    const std::string s_Id = CommandTest::Uuid(1);

    // --- the JSON reader: strict and bounded ------------------------------------------------------
    {
        CHECK(CommandJson::Parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5e3}}").ok);
        CHECK(!CommandJson::Parse("{\"a\":1,}").ok);
        CHECK(!CommandJson::Parse("{\"a\":1,\"a\":2}").ok);          // duplicate key
        CHECK(!CommandJson::Parse("{\"a\":01}").ok);                 // leading zero
        CHECK(!CommandJson::Parse("{\"a\":\"\x01\"}").ok);           // control character
        CHECK(!CommandJson::Parse("{\"a\":\"\\x\"}").ok);            // bad escape
        CHECK(!CommandJson::Parse("{\"a\":1} x").ok);                // trailing characters
        CHECK(!CommandJson::Parse("[[[[[1]]]]]").ok);                // too deep (5 > 4)
        CHECK(CommandJson::Parse("[[[[1]]]]").ok);
        CHECK(!CommandJson::Parse(std::string("\xff\xfe{}")).ok);    // invalid utf-8
        CHECK(!CommandJson::Parse("{\"a\":\"\\ud800\"}").ok);         // lone surrogate
        CHECK(CommandJson::Parse("{\"a\":\"\\ud83d\\ude00\"}").ok);   // a valid pair
        CHECK(CommandJson::Parse("\"" + std::string(1024, 'x') + "\"").ok);
        CHECK(!CommandJson::Parse("\"" + std::string(1025, 'x') + "\"").ok);

        std::string s_Many = "[";
        for (int i = 0; i < 300; ++i) s_Many += (i ? ",1" : "1");
        s_Many += "]";
        CHECK(!CommandJson::Parse(s_Many).ok);                       // too many values

        const auto s_Number = CommandJson::Parse("[12, 12.0, -3, 1e2]");
        CHECK(s_Number.ok && s_Number.value.items[0].is_integer && s_Number.value.items[0].integer == 12);
        CHECK(!s_Number.value.items[1].is_integer && !s_Number.value.items[3].is_integer);
        CHECK(s_Number.value.items[2].is_integer && s_Number.value.items[2].integer == -3);
    }

    // --- UUID and lookup ----------------------------------------------------------------------------
    {
        CHECK(ValidUuid("9c8f3a75-7054-457c-919d-ee87e77f57d9"));
        CHECK(!ValidUuid("9C8F3A75-7054-457C-919D-EE87E77F57D9")); // uppercase
        CHECK(!ValidUuid("9c8f3a75-7054-057c-919d-ee87e77f57d9")); // version 0
        CHECK(!ValidUuid("9c8f3a75-7054-457c-719d-ee87e77f57d9")); // variant 7
        CHECK(!ValidUuid("not-a-uuid"));
        CHECK(Lookup("relay.probe", 1) == Entry::RelayProbe1);
        CHECK(Lookup("relay.probe", 2) == Entry::Unknown);
        CHECK(Lookup("player.outfit.set", 1) == Entry::PlayerOutfitSet1Reserved);
        CHECK(Lookup("player.outfit.set", 2) == Entry::PlayerOutfitSet2 && IsEffectful(Entry::PlayerOutfitSet2) && !IsEffectful(Entry::PlayerOutfitSet1Reserved));
        CHECK(Lookup("game.time_scale.set", 1) == Entry::Unknown);

        // M6-B1: the two read-only entries, inert, and the 16-hex-digit id rule.
        CHECK(Lookup("relay.inspect.players", 1) == Entry::RelayInspectPlayers1 && !IsEffectful(Entry::RelayInspectPlayers1));
        CHECK(Lookup("relay.inspect.entity", 1) == Entry::RelayInspectEntity1 && !IsEffectful(Entry::RelayInspectEntity1));
        CHECK(Lookup("relay.inspect.players", 2) == Entry::Unknown && Lookup("relay.inspect.entity", 2) == Entry::Unknown);
        // M6-D1a
        CHECK(Lookup("relay.inspect.candidate", 1) == Entry::RelayInspectCandidate1 && !IsEffectful(Entry::RelayInspectCandidate1));
        CHECK(Lookup("relay.inspect.candidate", 2) == Entry::Unknown && Lookup("relay.inspect.candidates", 1) == Entry::Unknown);
        CHECK(ValidHex16("007c57a6ea581f8c") && ValidHex16("feede715906f747f"));
        CHECK(!ValidHex16("FEEDE715906F747F") && !ValidHex16("feede715906f747") && !ValidHex16("feede715906f747fa") && !ValidHex16("0xfeede715906f74") && !ValidHex16(""));
    }

    // --- M6-B1: relay.inspect.players takes no parameter; relay.inspect.entity two optional ids ---
    {
        auto s_Decode = [&](const std::string& p_LineWithNewline) {
            std::string s_Line = p_LineWithNewline;
            s_Line.pop_back();
            return DecodeCommand(s_Line);
        };

        const auto s_Players = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.players", 1));
        CHECK(s_Players.ok && s_Players.command.entry == Entry::RelayInspectPlayers1 && s_Players.command.params.empty());
        const auto s_PlayersParam = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.players", 1, "{\"slot\":0}"));
        CHECK(!s_PlayersParam.ok && Contains(s_PlayersParam.reason, "unknown parameter") && s_PlayersParam.command_id == s_Id);

        const auto s_Entity = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.entity", 1));
        CHECK(s_Entity.ok && s_Entity.command.entry == Entry::RelayInspectEntity1 && s_Entity.command.params.empty());
        const auto s_Both = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.entity", 1, "{\"brick\":\"007c57a6ea581f8c\",\"entity_id\":\"feede715906f747f\"}"));
        CHECK(s_Both.ok && s_Both.command.params.size() == 2 && s_Both.command.params[0].first == "brick" && s_Both.command.params[0].second == "007c57a6ea581f8c" && s_Both.command.params[1].second == "feede715906f747f");
        const auto s_Upper = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.entity", 1, "{\"entity_id\":\"FEEDE715906F747F\"}"));
        CHECK(!s_Upper.ok && Contains(s_Upper.reason, "invalid parameter: entity_id"));
        const auto s_Address = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.entity", 1, "{\"address\":\"0x14313dac0\"}"));
        CHECK(!s_Address.ok && Contains(s_Address.reason, "unknown parameter: address"));
        const auto s_Number = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.entity", 1, "{\"brick\":1}"));
        CHECK(!s_Number.ok && Contains(s_Number.reason, "invalid parameter: brick"));
        // the entries decode on a version-2 link too, with the effectful setting off
        DecodeOptions s_V2;
        s_V2.protocol_version = 2;
        std::string s_V2Line = CommandTest::Command(s_Id, 3, 500, "relay.inspect.players", 1, "{}", "{}", 2);
        s_V2Line.pop_back();
        CHECK(DecodeCommand(s_V2Line, s_V2).ok);

        // M6-D1a: relay.inspect.candidate takes exactly the same two optional ids and nothing else —
        // no spawn id, no address, no offset, no name of anything.
        const auto s_Candidate = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1));
        CHECK(s_Candidate.ok && s_Candidate.command.entry == Entry::RelayInspectCandidate1 && s_Candidate.command.params.empty());
        const auto s_CandidateBoth = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{\"brick\":\"007c57a6ea581f8c\",\"entity_id\":\"feede715906f747f\"}"));
        CHECK(s_CandidateBoth.ok && s_CandidateBoth.command.params.size() == 2 && s_CandidateBoth.command.params[1].first == "entity_id");
        const auto s_CandidateSpawn = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{\"spawn_id\":\"feed755acdd56780\"}"));
        CHECK(!s_CandidateSpawn.ok && Contains(s_CandidateSpawn.reason, "unknown parameter: spawn_id"));
        const auto s_CandidateOffset = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{\"offset\":1816}"));
        CHECK(!s_CandidateOffset.ok && Contains(s_CandidateOffset.reason, "unknown parameter: offset"));
        const auto s_CandidateMethod = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{\"entity_id\":\"GetObjectToWorld\"}"));
        CHECK(!s_CandidateMethod.ok && Contains(s_CandidateMethod.reason, "invalid parameter: entity_id"));
        const auto s_CandidateUpper = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{\"brick\":\"007C57A6EA581F8C\"}"));
        CHECK(!s_CandidateUpper.ok && Contains(s_CandidateUpper.reason, "invalid parameter: brick"));
        std::string s_CandidateV2 = CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 1, "{}", "{}", 2);
        s_CandidateV2.pop_back();
        CHECK(DecodeCommand(s_CandidateV2, s_V2).ok && DecodeCommand(s_CandidateV2, s_V2).command.entry == Entry::RelayInspectCandidate1);
        // version 2 of the entry does not exist
        const auto s_CandidateV2Entry = s_Decode(CommandTest::Command(s_Id, 3, 500, "relay.inspect.candidate", 2));
        CHECK(!s_CandidateV2Entry.ok && Contains(s_CandidateV2Entry.reason, "unknown command"));
    }

    // --- a valid relay.probe command ----------------------------------------------------------------
    {
        std::string s_Line = CommandTest::Command(s_Id, 3, 500);
        s_Line.pop_back();
        const auto s_Decoded = DecodeCommand(s_Line);
        CHECK(s_Decoded.ok);
        CHECK(s_Decoded.command.command_id == s_Id);
        CHECK(s_Decoded.command.link_generation == 3);
        CHECK(s_Decoded.command.ttl_ms == 500);
        CHECK(s_Decoded.command.entry == Entry::RelayProbe1);
        CHECK(s_Decoded.command.params.empty());
        CHECK(!s_Decoded.command.mission_generation_precondition);
    }

    // --- rejections that carry an id (answerable) ---------------------------------------------------
    {
        auto s_Reject = [&](const std::string& p_LineWithNewline, const char* p_Needle) {
            std::string s_Line = p_LineWithNewline;
            s_Line.pop_back();
            const auto s_Decoded = DecodeCommand(s_Line);
            CHECK(!s_Decoded.ok);
            CHECK(s_Decoded.command_id == s_Id && s_Decoded.link_generation == 3);
            CHECK(Contains(s_Decoded.reason, p_Needle));
        };

        s_Reject(CommandTest::Command(s_Id, 3, 500, "relay.nope", 1), "unknown command");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "relay.probe", 2), "unknown command");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 1, "{\"disguise_repository_id\":\"9c8f3a75-7054-457c-919d-ee87e77f57d9\"}"), "reserved");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 1, "{\"kit_pointer\":\"0x1\"}"), "unknown parameter");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "relay.probe", 1, "{\"x\":1}"), "unknown parameter");
        s_Reject(CommandTest::Command(s_Id, 3, 50), "ttl_ms");
        s_Reject(CommandTest::Command(s_Id, 3, 20000), "ttl_ms");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "relay.probe", 1, "{}", "{\"scene_resource\":\"x\"}"), "unknown precondition");
        s_Reject(CommandTest::Command(s_Id, 3, 500, "relay.probe", 1, "{}", "{\"mission_generation\":1}"), "invalid precondition");

        // An unknown top-level field with a valid id: rejected (answerable), never executed.
        std::string s_Extra = CommandTest::Command(s_Id, 3, 500);
        s_Extra.pop_back();
        s_Extra.pop_back();
        s_Extra += ",\"function\":\"ZHitman5_SetOutfit\"}";
        const auto s_Decoded = DecodeCommand(s_Extra);
        CHECK(!s_Decoded.ok && Contains(s_Decoded.reason, "unknown field") && s_Decoded.command_id == s_Id);
    }

    // --- violations without an answerable identity --------------------------------------------------
    {
        auto s_Violation = [](const std::string& p_Line, const char* p_Needle) {
            const auto s_Decoded = DecodeCommand(p_Line);
            CHECK(!s_Decoded.ok);
            CHECK(s_Decoded.command_id.empty() || s_Decoded.link_generation == 0);
            CHECK(Contains(s_Decoded.reason, p_Needle));
        };

        s_Violation("{nope", "invalid json");
        s_Violation("[1,2]", "not an object");
        s_Violation(std::string("\xff\xfe{}"), "invalid json");
        s_Violation("{\"command_protocol_version\":2,\"kind\":\"command\"}", "protocol version");
        s_Violation("{\"command_protocol_version\":1,\"kind\":\"exec\"}", "unknown kind");
        s_Violation("{\"command_protocol_version\":1,\"kind\":\"command\",\"command_id\":\"x\"}", "command_id");
        s_Violation("{\"command_protocol_version\":1,\"kind\":\"command\",\"command_id\":\"" + s_Id + "\",\"link_generation\":0}", "link_generation");
        s_Violation(std::string(4097, 'x'), "line too long");
    }

    // --- welcome and refusal ------------------------------------------------------------------------
    {
        Welcome s_Welcome;
        std::string s_Reason;
        std::string s_Line = CommandTest::Welcome(7);
        s_Line.pop_back();
        CHECK(DecodeWelcome(s_Line, s_Welcome, s_Reason));
        CHECK(s_Welcome.link_generation == 7 && s_Welcome.node == "test" && s_Welcome.allowlist.size() == 1);
        CHECK(!DecodeWelcome("{\"command_protocol_version\":2,\"kind\":\"welcome\",\"link_generation\":1}", s_Welcome, s_Reason));
        CHECK(!DecodeWelcome("{\"command_protocol_version\":1,\"kind\":\"welcome\",\"link_generation\":0,\"node\":\"n\",\"allowlist\":[]}", s_Welcome, s_Reason));
        CHECK(!DecodeWelcome("{\"command_protocol_version\":1,\"kind\":\"welcome\",\"link_generation\":1,\"node\":\"n\",\"allowlist\":[],\"extra\":1}", s_Welcome, s_Reason));
        CHECK(IsRefusal("{\"command_protocol_version\":1,\"kind\":\"refused\",\"reason\":\"busy\"}", s_Reason) && s_Reason == "busy");
        CHECK(!IsRefusal(s_Line, s_Reason));
    }

    // --- encoding: hello and result, deterministic and bounded ---------------------------------------
    {
        Hello s_Hello;
        s_Hello.adapter_instance_id = s_Id;
        s_Hello.native_at = "2026-10-14T00:00:00.000Z";
        s_Hello.capabilities = {"relay.probe/1"};
        const std::string s_HelloLine = EncodeHello(s_Hello);
        CHECK(s_HelloLine == "{\"command_protocol_version\":1,\"kind\":\"hello\",\"adapter_instance_id\":\"" + s_Id + "\",\"native_at\":\"2026-10-14T00:00:00.000Z\",\"mission_generation\":null,\"capabilities\":[\"relay.probe/1\"]}\n");
        s_Hello.mission_generation = "gen-a";
        CHECK(Contains(EncodeHello(s_Hello), "\"mission_generation\":\"gen-a\""));

        Result s_Result;
        s_Result.command_id = s_Id;
        s_Result.link_generation = 3;
        s_Result.adapter_instance_id = CommandTest::Uuid(2);
        s_Result.status = Status::Applied;
        s_Result.native_at = "t";
        s_Result.frame = 42;
        ObservedValue s_Bool; s_Bool.kind = ObservedValue::Kind::Bool; s_Bool.boolean = true;
        ObservedValue s_Int; s_Int.kind = ObservedValue::Kind::Integer; s_Int.integer = 7;
        ObservedValue s_Text; s_Text.kind = ObservedValue::Kind::Text; s_Text.text = std::string(300, 'z');
        s_Result.observed = {{"scene_loaded", s_Bool}, {"frame", s_Int}, {"long", s_Text}};
        s_Result.reason = std::string(300, 'r');
        const std::string s_Line = EncodeResult(s_Result);
        CHECK(s_Line == EncodeResult(s_Result));
        CHECK(Contains(s_Line, "\"kind\":\"command.result\"") && Contains(s_Line, "\"status\":\"applied\"") && Contains(s_Line, "\"frame\":42"));
        CHECK(Contains(s_Line, "\"scene_loaded\":true") && Contains(s_Line, "\"frame\":7"));
        CHECK(Contains(s_Line, "\"reason\":\"" + std::string(200, 'r') + "\""));
        CHECK(Contains(s_Line, "\"long\":\"" + std::string(128, 'z') + "\""));
        CHECK(s_Line.size() <= k_MaxLineBytes);
        CHECK(s_Line.back() == '\n');

        // The encoded result decodes as a JSON object with exactly the schema's fields.
        std::string s_Body = s_Line;
        s_Body.pop_back();
        const auto s_Parsed = CommandJson::Parse(s_Body);
        CHECK(s_Parsed.ok && s_Parsed.value.fields.size() == 11);
        CHECK(s_Parsed.value.Find("mission_generation") && s_Parsed.value.Find("mission_generation")->kind == CommandJson::Value::Kind::Null);

        // Every status word is in the closed vocabulary BEAM accepts.
        const char* s_Expected[] = {"received", "rejected", "busy", "duplicate", "expired", "precondition_failed", "unavailable", "applied", "failed"};
        const Status s_Statuses[] = {Status::Received, Status::Rejected, Status::Busy, Status::Duplicate, Status::Expired, Status::PreconditionFailed, Status::Unavailable, Status::Applied, Status::Failed};
        for (int i = 0; i < 9; ++i)
            CHECK(std::string(StatusText(s_Statuses[i])) == s_Expected[i]);
    }

    // --- M5 C-2B: protocol version 2 and player.outfit.set v2 -------------------------------------------
    {
        const std::string s_Suit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
        const std::string s_Target = "4fc9396e-2619-4e66-a51e-2bd366230da7";
        const std::string s_Params = CommandTest::OutfitParams(s_Target, 0, 0, s_Suit);
        const std::string s_Pre = "{\"mission_generation\":\"gen-a\"}";
        DecodeOptions s_V2;
        s_V2.protocol_version = 2;
        s_V2.effectful = true;
        DecodeOptions s_V2Inert;
        s_V2Inert.protocol_version = 2;
        DecodeOptions s_V1;

        auto s_Line = [&](const std::string& p_WithNewline) { std::string l = p_WithNewline; l.pop_back(); return l; };

        // A complete, valid v2 command on an effectful v2 link.
        const auto s_Ok = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 2, s_Params, s_Pre, 2)), s_V2);
        CHECK(s_Ok.ok && s_Ok.command.entry == Entry::PlayerOutfitSet2);
        CHECK(s_Ok.command.outfit_target && s_Ok.command.outfit_target->outfit_id == s_Target && s_Ok.command.outfit_target->charset == 0 && s_Ok.command.outfit_target->variation == 0);
        CHECK(s_Ok.command.outfit_expect_current && s_Ok.command.outfit_expect_current->outfit_id == s_Suit);
        CHECK(s_Ok.command.mission_generation_precondition && *s_Ok.command.mission_generation_precondition == "gen-a");
        CHECK(DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 2, CommandTest::OutfitParams(s_Target, 15, 63, s_Suit, 15, 63), s_Pre, 2)), s_V2).ok);

        // Rejected (answerable) before scheduling: effectful off; incomplete or out-of-bounds tuples;
        // missing expect_current or generation; the same tuple; unknown parameters; v1 reserved.
        auto s_RejectV2 = [&](const std::string& p_Params, const std::string& p_Preconditions, const DecodeOptions& p_Options, const char* p_Needle) {
            const auto d = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 2, p_Params, p_Preconditions, 2)), p_Options);
            CHECK(!d.ok && d.command_id == s_Id && d.link_generation == 3);
            const bool s_Has = Contains(d.reason, p_Needle);
            CHECK(s_Has);
            if (!s_Has) std::printf("  v2 reject: expected '%s' got '%s'\n", p_Needle, d.reason.c_str());
        };
        s_RejectV2(s_Params, s_Pre, s_V2Inert, "effectful setting disabled");
        s_RejectV2(CommandTest::OutfitParams(s_Target, 16, 0, s_Suit), s_Pre, s_V2, "charset");
        s_RejectV2(CommandTest::OutfitParams(s_Target, 0, 64, s_Suit), s_Pre, s_V2, "variation");
        s_RejectV2(CommandTest::OutfitParams(s_Target, -1, 0, s_Suit), s_Pre, s_V2, "charset");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"charset\":0,\"variation\":0}", s_Pre, s_V2, "missing parameter: expect_current");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"variation\":0,\"expect_current\":{\"disguise_repository_id\":\"" + s_Suit + "\",\"charset\":0,\"variation\":0}}", s_Pre, s_V2, "charset");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"charset\":0,\"variation\":0,\"expect_current\":{\"disguise_repository_id\":\"" + s_Suit + "\",\"charset\":0}}", s_Pre, s_V2, "expect_current");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"charset\":0,\"variation\":0,\"expect_current\":{\"disguise_repository_id\":\"" + s_Suit + "\",\"charset\":0,\"variation\":0,\"kit_pointer\":\"0x1\"}}", s_Pre, s_V2, "expect_current");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"charset\":0,\"variation\":0,\"expect_current\":\"" + s_Suit + "\"}", s_Pre, s_V2, "expect_current");
        s_RejectV2(s_Params, "{}", s_V2, "missing precondition: mission_generation");
        s_RejectV2(CommandTest::OutfitParams(s_Suit, 0, 0, s_Suit), s_Pre, s_V2, "no-op");
        s_RejectV2("{\"disguise_repository_id\":\"" + s_Target + "\",\"charset\":0,\"variation\":0,\"expect_current\":{\"disguise_repository_id\":\"" + s_Suit + "\",\"charset\":0,\"variation\":0},\"function\":\"x\"}", s_Pre, s_V2, "unknown parameter");
        s_RejectV2("{\"disguise_repository_id\":\"not-a-uuid\",\"charset\":0,\"variation\":0,\"expect_current\":{\"disguise_repository_id\":\"" + s_Suit + "\",\"charset\":0,\"variation\":0}}", s_Pre, s_V2, "disguise_repository_id");
        const auto s_Reserved = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 1, "{\"disguise_repository_id\":\"" + s_Target + "\"}", "{}", 2)), s_V2);
        CHECK(!s_Reserved.ok && Contains(s_Reserved.reason, "reserved"));

        // A v1 line on a v2 link (and a v2 line on a v1 link) is a violation: no identity is kept.
        const auto s_Wrong1 = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3)), s_V2);
        CHECK(!s_Wrong1.ok && s_Wrong1.command_id.empty() && Contains(s_Wrong1.reason, "protocol version"));
        const auto s_Wrong2 = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "relay.probe", 1, "{}", "{}", 2)), s_V1);
        CHECK(!s_Wrong2.ok && s_Wrong2.command_id.empty() && Contains(s_Wrong2.reason, "protocol version"));
        // The one-argument decoder is version 1 without the effectful entry, as before.
        CHECK(DecodeCommand(s_Line(CommandTest::Command(s_Id, 3))).ok);
        const auto s_V1Outfit2 = DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "player.outfit.set", 2, s_Params, s_Pre)));
        CHECK(!s_V1Outfit2.ok && Contains(s_V1Outfit2.reason, "effectful setting disabled"));
        // Inert v2 commands decode on a v2 link.
        CHECK(DecodeCommand(s_Line(CommandTest::Command(s_Id, 3, 500, "relay.probe", 1, "{}", "{}", 2)), s_V2Inert).ok);

        // The version-2 welcome: nonce and attest_required required and bounded; the other
        // version is a mismatch in both directions.
        const std::string s_Nonce = "514f1d221aefccd9d7fc4193040369cf";
        Welcome s_Welcome;
        std::string s_Reason;
        CHECK(DecodeWelcome(s_Line(CommandTest::WelcomeV2(9, s_Nonce)), 2, s_Welcome, s_Reason));
        CHECK(s_Welcome.protocol_version == 2 && s_Welcome.link_generation == 9 && s_Welcome.nonce == s_Nonce && s_Welcome.attest_required && s_Welcome.allowlist.size() == 3);
        CHECK(!DecodeWelcome(s_Line(CommandTest::WelcomeV2(9, s_Nonce)), 1, s_Welcome, s_Reason) && Contains(s_Reason, "mismatch"));
        CHECK(!DecodeWelcome(s_Line(CommandTest::Welcome(9)), 2, s_Welcome, s_Reason) && Contains(s_Reason, "mismatch"));
        CHECK(DecodeWelcome(s_Line(CommandTest::Welcome(9)), 1, s_Welcome, s_Reason) && s_Welcome.protocol_version == 1 && s_Welcome.nonce.empty());
        CHECK(!DecodeWelcome(s_Line(CommandTest::WelcomeV2(9, "514F1D221AEFCCD9D7FC4193040369CF")), 2, s_Welcome, s_Reason) && Contains(s_Reason, "nonce"));
        CHECK(!DecodeWelcome(s_Line(CommandTest::WelcomeV2(9, "514f1d22")), 2, s_Welcome, s_Reason));
        CHECK(!DecodeWelcome("{\"command_protocol_version\":2,\"kind\":\"welcome\",\"link_generation\":9,\"node\":\"test\",\"allowlist\":[],\"nonce\":\"" + s_Nonce + "\",\"attest_required\":false}", 2, s_Welcome, s_Reason) && Contains(s_Reason, "attest_required"));
        CHECK(!DecodeWelcome("{\"command_protocol_version\":2,\"kind\":\"welcome\",\"link_generation\":9,\"node\":\"test\",\"allowlist\":[],\"attest_required\":true}", 2, s_Welcome, s_Reason) && Contains(s_Reason, "nonce"));
        CHECK(!DecodeWelcome("{\"command_protocol_version\":2,\"kind\":\"welcome\",\"link_generation\":9,\"node\":\"test\",\"allowlist\":[],\"nonce\":\"" + s_Nonce + "\",\"attest_required\":true,\"token\":\"x\"}", 2, s_Welcome, s_Reason) && Contains(s_Reason, "unknown field"));

        // Hello and result carry the negotiated version.
        Hello s_Hello;
        s_Hello.protocol_version = 2;
        s_Hello.adapter_instance_id = s_Id;
        s_Hello.native_at = "t";
        s_Hello.capabilities = {"relay.probe/1", "relay.inspect.outfit/1", "player.outfit.set/2"};
        const std::string s_HelloLine = EncodeHello(s_Hello);
        CHECK(s_HelloLine.rfind("{\"command_protocol_version\":2,\"kind\":\"hello\"", 0) == 0 && Contains(s_HelloLine, "\"player.outfit.set/2\""));
        Result s_Result;
        s_Result.protocol_version = 2;
        s_Result.command_id = s_Id;
        s_Result.link_generation = 3;
        s_Result.status = Status::Applied;
        CHECK(EncodeResult(s_Result).rfind("{\"command_protocol_version\":2,\"kind\":\"command.result\"", 0) == 0);
        s_Result.protocol_version = 1;
        CHECK(EncodeResult(s_Result).rfind("{\"command_protocol_version\":1,", 0) == 0);
    }
}
