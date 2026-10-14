#include "TestHarness.h"

#include <string>

#include "CommandJson.h"
#include "CommandProtocol.h"
#include "TestListener.h"

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
        CHECK(Lookup("game.time_scale.set", 1) == Entry::Unknown);
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
}
