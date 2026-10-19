#include "MotionProtocol.h"

#include <fmt/format.h>

#include "CommandJson.h"
#include "CommandProtocol.h"
#include "FloatWords.h"
#include "Json.h"

namespace MotionProtocol
{
    using CommandJson::Value;

    const char* KindText(Kind p_Kind)
    {
        switch (p_Kind)
        {
            case Kind::Welcome: return "motion.welcome";
            case Kind::Refused: return "motion.refused";
            case Kind::Target: return "ghost.target";
            case Kind::Close: return "ghost.close";
            case Kind::ShareClose: return "share.close";
        }
        return "motion.refused";
    }

    const char* OutcomeText(Outcome p_Outcome)
    {
        switch (p_Outcome)
        {
            case Outcome::Applied: return "applied";
            case Outcome::SkippedIdentical: return "skipped_identical";
            case Outcome::Refused: return "refused";
            case Outcome::Uncertain: return "uncertain";
        }
        return "refused";
    }

    std::string ReasonText(std::string_view p_Reason)
    {
        if (p_Reason.size() <= k_MaxReasonBytes)
            return std::string(p_Reason);

        // Cut on a UTF-8 boundary so the text stays valid (BEAM bounds the decoded bytes).
        size_t s_Cut = k_MaxReasonBytes;
        while (s_Cut > 0 && (static_cast<unsigned char>(p_Reason[s_Cut]) & 0xC0) == 0x80)
            --s_Cut;
        return std::string(p_Reason.substr(0, s_Cut));
    }

    namespace
    {
        Decoded Fail(std::string p_Why)
        {
            Decoded s_Out;
            s_Out.error = std::move(p_Why);
            return s_Out;
        }

        bool KnownKeys(const Value& p_Object, std::initializer_list<const char*> p_Allowed, std::string& p_Unknown)
        {
            for (const auto& s_Field : p_Object.fields)
            {
                bool s_Known = false;
                for (const char* s_Key : p_Allowed)
                    if (s_Field.first == s_Key)
                        s_Known = true;
                if (!s_Known)
                {
                    p_Unknown = s_Field.first.substr(0, 64);
                    return false;
                }
            }
            return true;
        }

        bool Text(const Value* p_Value, size_t p_Max, std::string& p_Out)
        {
            if (!p_Value || p_Value->kind != Value::Kind::String || p_Value->text.size() > p_Max)
                return false;
            p_Out = p_Value->text;
            return true;
        }

        bool Integer(const Value* p_Value, uint64_t p_Min, uint64_t p_Max, uint64_t& p_Out)
        {
            if (!p_Value || p_Value->kind != Value::Kind::Number || !p_Value->is_integer || p_Value->integer < 0)
                return false;
            const uint64_t s_Value = static_cast<uint64_t>(p_Value->integer);
            if (s_Value < p_Min || s_Value > p_Max)
                return false;
            p_Out = s_Value;
            return true;
        }

        bool Uuid(const Value* p_Value, std::string& p_Out)
        {
            return Text(p_Value, 36, p_Out) && CommandProtocol::ValidUuid(p_Out);
        }

        bool LowerHex32(std::string_view p_Text)
        {
            if (p_Text.size() != 32)
                return false;
            for (const char c : p_Text)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            return true;
        }
    }

    Decoded DecodeFromBeam(std::string_view p_Line)
    {
        if (p_Line.size() > k_MaxLineBytes)
            return Fail("line too long");

        const auto s_Parsed = CommandJson::Parse(p_Line);
        if (!s_Parsed.ok)
            return Fail("invalid json: " + s_Parsed.error);

        const Value& s_Root = s_Parsed.value;
        if (s_Root.kind != Value::Kind::Object)
            return Fail("not an object");

        uint64_t s_Version = 0;
        if (!Integer(s_Root.Find("motion_protocol_version"), 0, UINT64_MAX >> 1, s_Version) || s_Version != static_cast<uint64_t>(k_Version))
            return Fail("unsupported or missing motion_protocol_version");

        std::string s_Kind;
        if (!Text(s_Root.Find("kind"), 32, s_Kind))
            return Fail("missing kind");

        Decoded s_Out;
        Message& s_Message = s_Out.message;
        std::string s_Unknown;

        if (s_Kind == "motion.welcome")
        {
            if (!KnownKeys(s_Root, {"motion_protocol_version", "kind", "motion_link_generation", "nonce"}, s_Unknown))
                return Fail("unknown field: " + s_Unknown);
            s_Message.kind = Kind::Welcome;
            if (!Integer(s_Root.Find("motion_link_generation"), 1, k_MaxGeneration, s_Message.motion_link_generation))
                return Fail("invalid field: motion_link_generation");
            if (!Text(s_Root.Find("nonce"), 32, s_Message.nonce) || !LowerHex32(s_Message.nonce))
                return Fail("invalid field: nonce");
        }
        else if (s_Kind == "motion.refused")
        {
            if (!KnownKeys(s_Root, {"motion_protocol_version", "kind", "reason"}, s_Unknown))
                return Fail("unknown field: " + s_Unknown);
            s_Message.kind = Kind::Refused;
            if (!Text(s_Root.Find("reason"), k_MaxReasonBytes, s_Message.reason))
                return Fail("invalid field: reason");
        }
        else if (s_Kind == "ghost.target")
        {
            if (!KnownKeys(s_Root, {"motion_protocol_version", "kind", "session_id", "seq", "pos"}, s_Unknown))
                return Fail("unknown field: " + s_Unknown);
            s_Message.kind = Kind::Target;
            if (!Uuid(s_Root.Find("session_id"), s_Message.id))
                return Fail("invalid field: session_id");
            if (!Integer(s_Root.Find("seq"), 1, UINT64_MAX >> 1, s_Message.seq))
                return Fail("invalid field: seq");
            std::string s_Pos;
            std::string s_Why;
            uint32_t s_Words[2] = {};
            if (!Text(s_Root.Find("pos"), 17, s_Pos) || !FloatWords::Decode(s_Pos, s_Words, 2, s_Why))
                return Fail("invalid field: pos" + (s_Why.empty() ? std::string() : " (" + s_Why + ")"));
            s_Message.x = s_Words[0];
            s_Message.y = s_Words[1];
        }
        else if (s_Kind == "ghost.close")
        {
            if (!KnownKeys(s_Root, {"motion_protocol_version", "kind", "session_id", "reason"}, s_Unknown))
                return Fail("unknown field: " + s_Unknown);
            s_Message.kind = Kind::Close;
            if (!Uuid(s_Root.Find("session_id"), s_Message.id))
                return Fail("invalid field: session_id");
            if (!Text(s_Root.Find("reason"), k_MaxReasonBytes, s_Message.reason))
                return Fail("invalid field: reason");
        }
        else if (s_Kind == "share.close")
        {
            if (!KnownKeys(s_Root, {"motion_protocol_version", "kind", "share_id", "reason"}, s_Unknown))
                return Fail("unknown field: " + s_Unknown);
            s_Message.kind = Kind::ShareClose;
            if (!Uuid(s_Root.Find("share_id"), s_Message.id))
                return Fail("invalid field: share_id");
            if (!Text(s_Root.Find("reason"), k_MaxReasonBytes, s_Message.reason))
                return Fail("invalid field: reason");
        }
        else
        {
            return Fail("unknown kind: " + s_Kind.substr(0, 32));
        }

        s_Out.ok = true;
        return s_Out;
    }

    namespace
    {
        std::string Words(const uint32_t* p_Words, size_t p_Count)
        {
            return Json::Quote(FloatWords::Encode(p_Words, p_Count));
        }

        std::string OptionalInt(const std::optional<uint64_t>& p_Value)
        {
            return p_Value ? std::to_string(*p_Value) : "null";
        }
    }

    std::string EncodeHello(std::string_view p_AdapterInstanceId, uint64_t p_CommandLinkGeneration)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"motion.hello\",\"adapter_instance_id\":{},\"command_link_generation\":{}}}\n",
            k_Version, Json::Quote(p_AdapterInstanceId), p_CommandLinkGeneration
        );
    }

    std::string EncodeAttest(uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId, std::string_view p_Mac)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"motion.attest\",\"motion_link_generation\":{},\"command_link_generation\":{},\"adapter_instance_id\":{},\"mac\":{}}}\n",
            k_Version, p_MotionLinkGeneration, p_CommandLinkGeneration, Json::Quote(p_AdapterInstanceId), Json::Quote(p_Mac.substr(0, 64))
        );
    }

    std::string EncodeHeroSample(std::string_view p_ShareId, uint64_t p_Seq, uint64_t p_Frame, uint64_t p_NativeMonoMs, const uint32_t p_Pos[3], bool p_Dirty)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"hero.sample\",\"share_id\":{},\"seq\":{},\"frame\":{},\"native_mono_ms\":{},\"pos\":{},\"dirty\":{}}}\n",
            k_Version, Json::Quote(p_ShareId), p_Seq, p_Frame, p_NativeMonoMs, Words(p_Pos, 3), p_Dirty ? "true" : "false"
        );
    }

    std::string EncodeShareClosed(std::string_view p_ShareId, std::string_view p_Reason, std::optional<uint64_t> p_LastSeq, uint64_t p_Sent)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"share.closed\",\"share_id\":{},\"reason\":{},\"last_seq\":{},\"sent\":{}}}\n",
            k_Version, Json::Quote(p_ShareId), Json::Quote(ReasonText(p_Reason)), OptionalInt(p_LastSeq), p_Sent
        );
    }

    std::string EncodeApplied(const Applied& p_Applied)
    {
        std::string s_Post = "null";
        if (p_Applied.post_x && p_Applied.post_y)
        {
            const uint32_t s_Words[2] = {*p_Applied.post_x, *p_Applied.post_y};
            s_Post = Words(s_Words, 2);
        }

        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"ghost.applied\",\"session_id\":{},\"seq\":{},\"frame\":{},\"native_mono_ms\":{},\"outcome\":\"{}\",\"reason\":{},\"post\":{},\"exact\":{}}}\n",
            k_Version, Json::Quote(p_Applied.session_id), p_Applied.seq, p_Applied.frame, p_Applied.native_mono_ms, OutcomeText(p_Applied.outcome),
            p_Applied.reason ? Json::Quote(ReasonText(*p_Applied.reason)) : "null", s_Post,
            p_Applied.exact ? (*p_Applied.exact ? "true" : "false") : "null"
        );
    }

    std::string EncodeReport(const Report& p_Report)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"ghost.report\",\"session_id\":{},\"report_seq\":{},\"native_mono_ms\":{},\"frame\":{},\"received\":{},\"applied\":{},\"skipped\":{},\"refused\":{},\"uncertain\":{},\"superseded\":{},\"last_applied_seq\":{}}}\n",
            k_Version, Json::Quote(p_Report.session_id), p_Report.report_seq, p_Report.native_mono_ms, p_Report.frame,
            p_Report.counts.received, p_Report.counts.applied, p_Report.counts.skipped, p_Report.counts.refused, p_Report.counts.uncertain,
            p_Report.counts.superseded, OptionalInt(p_Report.last_applied_seq)
        );
    }

    std::string EncodeClosed(const Closed& p_Closed)
    {
        return fmt::format(
            "{{\"motion_protocol_version\":{},\"kind\":\"ghost.closed\",\"session_id\":{},\"reason\":{},\"native_mono_ms\":{},\"clean\":{},\"final_world\":{},\"last_applied_seq\":{},\"applied\":{}}}\n",
            k_Version, Json::Quote(p_Closed.session_id), Json::Quote(ReasonText(p_Closed.reason)), p_Closed.native_mono_ms,
            p_Closed.clean ? "true" : "false", p_Closed.final_world ? Json::Quote(*p_Closed.final_world) : "null",
            OptionalInt(p_Closed.last_applied_seq), p_Closed.applied
        );
    }

    size_t WorstCaseLineBytes(std::string_view p_Kind)
    {
        const std::string s_Id = "ffffffff-ffff-4fff-bfff-ffffffffffff";
        const std::string s_Reason(k_MaxReasonBytes, '\x01'); // each escapes to six bytes
        const uint64_t s_Max = UINT64_MAX;
        const uint32_t s_Pos[3] = {0xff7fffffu, 0xff7fffffu, 0xff7fffffu};

        if (p_Kind == "motion.hello")
            return EncodeHello(s_Id, s_Max).size() - 1;
        if (p_Kind == "motion.attest")
            return EncodeAttest(s_Max, s_Max, s_Id, std::string(64, 'f')).size() - 1;
        if (p_Kind == "hero.sample")
            return EncodeHeroSample(s_Id, s_Max, s_Max, s_Max, s_Pos, true).size() - 1;
        if (p_Kind == "share.closed")
            return EncodeShareClosed(s_Id, s_Reason, s_Max, s_Max).size() - 1;
        if (p_Kind == "ghost.applied")
        {
            Applied s_Applied;
            s_Applied.session_id = s_Id;
            s_Applied.seq = s_Max;
            s_Applied.frame = s_Max;
            s_Applied.native_mono_ms = s_Max;
            s_Applied.outcome = Outcome::SkippedIdentical;
            s_Applied.reason = s_Reason;
            s_Applied.post_x = 0xff7fffffu;
            s_Applied.post_y = 0xff7fffffu;
            s_Applied.exact = false;
            return EncodeApplied(s_Applied).size() - 1;
        }
        if (p_Kind == "ghost.report")
        {
            Report s_Report;
            s_Report.session_id = s_Id;
            s_Report.report_seq = s_Report.native_mono_ms = s_Report.frame = s_Max;
            s_Report.counts = Counts{s_Max, s_Max, s_Max, s_Max, s_Max, s_Max};
            s_Report.last_applied_seq = s_Max;
            return EncodeReport(s_Report).size() - 1;
        }
        if (p_Kind == "ghost.closed")
        {
            Closed s_Closed;
            s_Closed.session_id = s_Id;
            s_Closed.reason = s_Reason;
            s_Closed.native_mono_ms = s_Max;
            s_Closed.final_world = std::string(FloatWords::k_WorldTextBytes, 'f');
            s_Closed.last_applied_seq = s_Max;
            s_Closed.applied = s_Max;
            return EncodeClosed(s_Closed).size() - 1;
        }
        return 0;
    }
}
