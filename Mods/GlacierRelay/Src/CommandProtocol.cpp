#include "CommandProtocol.h"

#include <fmt/format.h>

#include "Attestation.h"
#include "CommandJson.h"
#include "Json.h"

namespace CommandProtocol
{
    const char* StatusText(Status p_Status)
    {
        switch (p_Status)
        {
            case Status::Received: return "received";
            case Status::Rejected: return "rejected";
            case Status::Busy: return "busy";
            case Status::Duplicate: return "duplicate";
            case Status::Expired: return "expired";
            case Status::PreconditionFailed: return "precondition_failed";
            case Status::Unavailable: return "unavailable";
            case Status::Applied: return "applied";
            case Status::Failed: return "failed";
        }

        return "failed";
    }

    const char* FieldStatusText(FieldStatus p_Status)
    {
        switch (p_Status)
        {
            case FieldStatus::Observed: return "observed";
            case FieldStatus::Unavailable: return "unavailable";
            case FieldStatus::Unsupported: return "unsupported";
            case FieldStatus::Inconsistent: return "inconsistent";
            case FieldStatus::PreconditionFailed: return "precondition_failed";
        }

        return "unavailable";
    }

    Entry Lookup(std::string_view p_Name, uint32_t p_Version)
    {
        if (p_Name == "relay.probe" && p_Version == 1)
            return Entry::RelayProbe1;
        if (p_Name == "relay.inspect.outfit" && p_Version == 1)
            return Entry::RelayInspectOutfit1;
        if (p_Name == "player.outfit.set" && p_Version == 1)
            return Entry::PlayerOutfitSet1Reserved;
        if (p_Name == "player.outfit.set" && p_Version == 2)
            return Entry::PlayerOutfitSet2;
        if (p_Name == "relay.inspect.players" && p_Version == 1)
            return Entry::RelayInspectPlayers1;
        if (p_Name == "relay.inspect.entity" && p_Version == 1)
            return Entry::RelayInspectEntity1;
        if (p_Name == "relay.inspect.candidate" && p_Version == 1)
            return Entry::RelayInspectCandidate1;
        if (p_Name == "relay.inspect.candidate_transform" && p_Version == 1)
            return Entry::RelayInspectCandidateTransform1;
        if (p_Name == "candidate.transform.set" && p_Version == 1)
            return Entry::CandidateTransformSet1;
        return Entry::Unknown;
    }

    bool IsEffectful(Entry p_Entry)
    {
        return p_Entry == Entry::PlayerOutfitSet2 || p_Entry == Entry::CandidateTransformSet1;
    }

    const char* TransformModeText(TransformMode p_Mode)
    {
        return p_Mode == TransformMode::Restore ? "restore" : "lift";
    }

    bool ValidHex16(std::string_view p_Text)
    {
        if (p_Text.size() != 16)
            return false;

        for (const char c : p_Text)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;

        return true;
    }

    bool ValidUuid(std::string_view p_Text)
    {
        // 8-4-4-4-12 lowercase hex, version nibble 1..5, variant nibble 8..b.
        if (p_Text.size() != 36)
            return false;

        for (size_t i = 0; i < 36; ++i)
        {
            const char c = p_Text[i];

            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != '-')
                    return false;
                continue;
            }

            const bool s_Hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!s_Hex)
                return false;
        }

        const char s_Version = p_Text[14];
        const char s_Variant = p_Text[19];
        return s_Version >= '1' && s_Version <= '5' && (s_Variant == '8' || s_Variant == '9' || s_Variant == 'a' || s_Variant == 'b');
    }

    namespace
    {
        using CommandJson::Value;

        bool IsText(const Value* p_Value, size_t p_MaxBytes)
        {
            return p_Value && p_Value->kind == Value::Kind::String && p_Value->text.size() <= p_MaxBytes;
        }

        bool IsUnsigned(const Value* p_Value, uint64_t& p_Out)
        {
            if (!p_Value || p_Value->kind != Value::Kind::Number || !p_Value->is_integer || p_Value->integer < 0)
                return false;
            p_Out = static_cast<uint64_t>(p_Value->integer);
            return true;
        }

        bool IsBoundedInt(const Value* p_Value, int32_t p_Min, int32_t p_Max, int32_t& p_Out)
        {
            if (!p_Value || p_Value->kind != Value::Kind::Number || !p_Value->is_integer || p_Value->integer < p_Min || p_Value->integer > p_Max)
                return false;
            p_Out = static_cast<int32_t>(p_Value->integer);
            return true;
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
                    p_Unknown = s_Field.first;
                    return false;
                }
            }

            return true;
        }

        DecodedCommand Reject(DecodedCommand p_Decoded, std::string p_Reason)
        {
            p_Decoded.ok = false;
            p_Decoded.reason = std::move(p_Reason);
            return p_Decoded;
        }

        std::string Truncate(std::string_view p_Text, size_t p_Max)
        {
            if (p_Text.size() <= p_Max)
                return std::string(p_Text);

            // Cut on a UTF-8 boundary so the output stays valid.
            size_t s_Cut = p_Max;
            while (s_Cut > 0 && (static_cast<unsigned char>(p_Text[s_Cut]) & 0xC0) == 0x80)
                --s_Cut;
            return std::string(p_Text.substr(0, s_Cut));
        }

        // An outfit tuple object with exactly the three fields, each within its bounds.
        bool ReadTuple(const Value& p_Object, OutfitTuple& p_Out, std::string& p_Reason)
        {
            if (p_Object.kind != Value::Kind::Object)
            {
                p_Reason = "not an object";
                return false;
            }

            std::string s_Unknown;
            if (!KnownKeys(p_Object, {"disguise_repository_id", "charset", "variation"}, s_Unknown))
            {
                p_Reason = "unknown field " + s_Unknown;
                return false;
            }

            const Value* s_Id = p_Object.Find("disguise_repository_id");
            if (!IsText(s_Id, 36) || !ValidUuid(s_Id->text))
            {
                p_Reason = "disguise_repository_id missing or not a uuid";
                return false;
            }

            if (!IsBoundedInt(p_Object.Find("charset"), 0, k_MaxCharset, p_Out.charset))
            {
                p_Reason = "charset missing or out of 0..15";
                return false;
            }

            if (!IsBoundedInt(p_Object.Find("variation"), 0, k_MaxVariation, p_Out.variation))
            {
                p_Reason = "variation missing or out of 0..63";
                return false;
            }

            p_Out.outfit_id = s_Id->text;
            return true;
        }
    }

    DecodedCommand DecodeCommand(std::string_view p_Line)
    {
        return DecodeCommand(p_Line, DecodeOptions{});
    }

    DecodedCommand DecodeCommand(std::string_view p_Line, const DecodeOptions& p_Options)
    {
        DecodedCommand s_Out;

        if (p_Line.size() > k_MaxLineBytes)
            return Reject(s_Out, "line too long");

        const auto s_Parsed = CommandJson::Parse(p_Line);

        if (!s_Parsed.ok)
            return Reject(s_Out, "invalid json: " + s_Parsed.error);

        const Value& s_Root = s_Parsed.value;

        if (s_Root.kind != Value::Kind::Object)
            return Reject(s_Out, "not an object");

        // The negotiated version is pinned for the link's life: a line of another version is a
        // violation (no identity is kept for it, so the link closes), never a downgrade.
        uint64_t s_Version = 0;
        if (!IsUnsigned(s_Root.Find("command_protocol_version"), s_Version) || s_Version != static_cast<uint64_t>(p_Options.protocol_version))
            return Reject(s_Out, "unsupported protocol version");

        const Value* s_Kind = s_Root.Find("kind");
        if (!IsText(s_Kind, 32) || s_Kind->text != "command")
            return Reject(s_Out, "unknown kind");

        // Identity first, so a rejection can be answered when they are valid.
        const Value* s_Id = s_Root.Find("command_id");
        if (!IsText(s_Id, 36) || !ValidUuid(s_Id->text))
            return Reject(s_Out, "invalid command_id");
        s_Out.command_id = s_Id->text;

        uint64_t s_Generation = 0;
        if (!IsUnsigned(s_Root.Find("link_generation"), s_Generation) || s_Generation == 0)
            return Reject(s_Out, "invalid link_generation");
        s_Out.link_generation = s_Generation;

        std::string s_Unknown;
        if (!KnownKeys(s_Root, {"command_protocol_version", "kind", "command_id", "link_generation", "issued_at", "ttl_ms", "name", "version", "params", "preconditions"}, s_Unknown))
            return Reject(s_Out, "unknown field: " + s_Unknown);

        const Value* s_IssuedAt = s_Root.Find("issued_at");
        if (!IsText(s_IssuedAt, 64))
            return Reject(s_Out, "invalid issued_at");

        uint64_t s_Ttl = 0;
        if (!IsUnsigned(s_Root.Find("ttl_ms"), s_Ttl) || s_Ttl < k_MinTtlMs || s_Ttl > k_MaxTtlMs)
            return Reject(s_Out, "ttl_ms out of range");

        const Value* s_Name = s_Root.Find("name");
        if (!IsText(s_Name, 64))
            return Reject(s_Out, "invalid name");

        uint64_t s_CommandVersion = 0;
        if (!IsUnsigned(s_Root.Find("version"), s_CommandVersion) || s_CommandVersion == 0 || s_CommandVersion > 1000)
            return Reject(s_Out, "invalid version");

        const Value* s_Params = s_Root.Find("params");
        if (!s_Params || s_Params->kind != Value::Kind::Object)
            return Reject(s_Out, "params not an object");

        const Value* s_Preconditions = s_Root.Find("preconditions");
        if (!s_Preconditions || s_Preconditions->kind != Value::Kind::Object)
            return Reject(s_Out, "preconditions not an object");

        Command& s_Command = s_Out.command;
        s_Command.command_id = s_Id->text;
        s_Command.link_generation = s_Generation;
        s_Command.issued_at = s_IssuedAt->text;
        s_Command.ttl_ms = static_cast<uint32_t>(s_Ttl);
        s_Command.name = s_Name->text;
        s_Command.version = static_cast<uint32_t>(s_CommandVersion);
        s_Command.entry = Lookup(s_Command.name, s_Command.version);

        // Preconditions: only mission_generation is defined; anything else is unknown.
        for (const auto& s_Field : s_Preconditions->fields)
        {
            if (s_Field.first != "mission_generation")
                return Reject(s_Out, "unknown precondition: " + s_Field.first);
            if (!IsText(&s_Field.second, k_MaxMissionGenerationBytes) || s_Field.second.text.empty())
                return Reject(s_Out, "invalid precondition mission_generation");
            s_Command.mission_generation_precondition = s_Field.second.text;
        }

        switch (s_Command.entry)
        {
            case Entry::RelayProbe1:
                if (!s_Params->fields.empty())
                    return Reject(s_Out, "unknown parameter: " + s_Params->fields.front().first);
                break;

            case Entry::RelayInspectOutfit1:
            {
                // M5 C-1: one optional, strictly validated repository id (UUID text); nothing else.
                for (const auto& s_Field : s_Params->fields)
                {
                    if (s_Field.first != "disguise_repository_id")
                        return Reject(s_Out, "unknown parameter: " + s_Field.first);
                    if (!IsText(&s_Field.second, 36) || !ValidUuid(s_Field.second.text))
                        return Reject(s_Out, "invalid parameter: " + s_Field.first);
                    s_Command.params.emplace_back(s_Field.first, s_Field.second.text);
                }
                break;
            }

            case Entry::RelayInspectPlayers1:
                // M6-B1: no parameters; the registry reads are fixed.
                if (!s_Params->fields.empty())
                    return Reject(s_Out, "unknown parameter: " + s_Params->fields.front().first);
                break;

            case Entry::RelayInspectCandidateTransform1:
                // M6-F: no parameters; the candidate is compiled, nothing is addressable by a peer.
                if (!s_Params->fields.empty())
                    return Reject(s_Out, "unknown parameter: " + s_Params->fields.front().first);
                break;

            case Entry::CandidateTransformSet1:
            {
                // M6-F (design section 12.5): the mode, the exact expectation groups and parent of a
                // recent exact inspection, and for a restore the journaled preimage world group and
                // the original command id; nothing else; the mandatory mission_generation
                // precondition; a restore already at its target is refused as a no-op; and none of
                // it unless the effectful setting is on (otherwise rejected like the reserved entry).
                if (!p_Options.effectful)
                    return Reject(s_Out, "effectful setting disabled: candidate.transform.set/1 not executable");

                if (!KnownKeys(*s_Params, {"mode", "expect_world", "expect_local", "expect_parent", "target_world", "restore_of"}, s_Unknown))
                    return Reject(s_Out, "unknown parameter: " + s_Unknown);

                TransformRequest s_Request;
                std::string s_Why;
                const Value* s_Mode = s_Params->Find("mode");
                if (!IsText(s_Mode, 16) || (s_Mode->text != "lift" && s_Mode->text != "restore"))
                    return Reject(s_Out, "invalid parameter: mode (missing or not lift|restore)");
                s_Request.mode = s_Mode->text == "restore" ? TransformMode::Restore : TransformMode::Lift;

                const Value* s_ExpectWorld = s_Params->Find("expect_world");
                if (!IsText(s_ExpectWorld, k_MaxObservedTextBytes) || !FloatWords::Decode(s_ExpectWorld->text, s_Request.expect_world, s_Why))
                    return Reject(s_Out, "invalid parameter: expect_world (" + (s_Why.empty() ? std::string("missing") : s_Why) + ")");

                const Value* s_ExpectLocal = s_Params->Find("expect_local");
                if (!IsText(s_ExpectLocal, k_MaxObservedTextBytes) || !FloatWords::Decode(s_ExpectLocal->text, s_Request.expect_local, s_Why))
                    return Reject(s_Out, "invalid parameter: expect_local (" + (s_Why.empty() ? std::string("missing") : s_Why) + ")");

                const Value* s_ExpectParent = s_Params->Find("expect_parent");
                if (!IsText(s_ExpectParent, 16) || !(ValidHex16(s_ExpectParent->text) || s_ExpectParent->text == "none"))
                    return Reject(s_Out, "invalid parameter: expect_parent (missing, or not 16 lowercase hex digits or none)");
                s_Request.expect_parent = s_ExpectParent->text;

                const Value* s_TargetWorld = s_Params->Find("target_world");
                const Value* s_RestoreOf = s_Params->Find("restore_of");

                if (s_Request.mode == TransformMode::Lift)
                {
                    if (s_TargetWorld || s_RestoreOf)
                        return Reject(s_Out, "invalid parameter: a lift carries no target_world or restore_of (the displacement is compiled)");
                }
                else
                {
                    FloatWords::Words12 s_Target;
                    if (!IsText(s_TargetWorld, k_MaxObservedTextBytes) || !FloatWords::Decode(s_TargetWorld->text, s_Target, s_Why))
                        return Reject(s_Out, "invalid parameter: target_world (" + (s_Why.empty() ? std::string("missing") : s_Why) + ")");
                    if (!IsText(s_RestoreOf, 36) || !ValidUuid(s_RestoreOf->text))
                        return Reject(s_Out, "invalid parameter: restore_of (missing or not a uuid)");
                    if (s_Target == s_Request.expect_world)
                        return Reject(s_Out, "target_world equals expect_world: already at the restore target; a no-op is not an effectful command");
                    s_Request.target_world = s_Target;
                    s_Request.restore_of = s_RestoreOf->text;
                }

                if (!s_Command.mission_generation_precondition)
                    return Reject(s_Out, "missing precondition: mission_generation");

                s_Command.transform = s_Request;
                s_Command.params.emplace_back("mode", s_Mode->text);
                break;
            }

            case Entry::RelayInspectEntity1:
            case Entry::RelayInspectCandidate1:
            {
                // M6-B1: two optional, strictly validated 16-hex-digit ids (a brick's runtime resource
                // id and an authored entity id); nothing else. No offset, no address, no pointer: the
                // ids name content, and the reader resolves them through the SDK's declared lookups.
                // M6-D1a: relay.inspect.candidate accepts exactly the same two parameters and nothing
                // else; the spawn target is the compiled historical default, never a peer-supplied id.
                for (const auto& s_Field : s_Params->fields)
                {
                    if (s_Field.first != "brick" && s_Field.first != "entity_id")
                        return Reject(s_Out, "unknown parameter: " + s_Field.first);
                    if (!IsText(&s_Field.second, 16) || !ValidHex16(s_Field.second.text))
                        return Reject(s_Out, "invalid parameter: " + s_Field.first + " (not 16 lowercase hex digits)");
                    s_Command.params.emplace_back(s_Field.first, s_Field.second.text);
                }
                break;
            }

            case Entry::PlayerOutfitSet1Reserved:
            {
                // Decoded for the record, never executed: the entry is reserved (M5 design 2.1).
                for (const auto& s_Field : s_Params->fields)
                {
                    if (s_Field.first != "disguise_repository_id" && s_Field.first != "expect_current")
                        return Reject(s_Out, "unknown parameter: " + s_Field.first);
                    if (!IsText(&s_Field.second, 36) || !ValidUuid(s_Field.second.text))
                        return Reject(s_Out, "invalid parameter: " + s_Field.first);
                    s_Command.params.emplace_back(s_Field.first, s_Field.second.text);
                }
                return Reject(s_Out, "reserved command: not issuable");
            }

            case Entry::PlayerOutfitSet2:
            {
                // M5 C-2B: the complete tuple, exactly these parameters, the mandatory
                // expected-current tuple and the mandatory mission_generation precondition; the
                // same tuple as the current one is refused as a no-op; and none of it unless the
                // effectful setting is on (otherwise rejected exactly like the reserved entry).
                if (!p_Options.effectful)
                    return Reject(s_Out, "effectful setting disabled: player.outfit.set/2 not executable");

                if (!KnownKeys(*s_Params, {"disguise_repository_id", "charset", "variation", "expect_current"}, s_Unknown))
                    return Reject(s_Out, "unknown parameter: " + s_Unknown);

                OutfitTuple s_Target;
                std::string s_Why;
                const Value* s_IdValue = s_Params->Find("disguise_repository_id");
                if (!IsText(s_IdValue, 36) || !ValidUuid(s_IdValue->text))
                    return Reject(s_Out, "invalid parameter: disguise_repository_id (missing or not a uuid)");
                if (!IsBoundedInt(s_Params->Find("charset"), 0, k_MaxCharset, s_Target.charset))
                    return Reject(s_Out, "invalid parameter: charset (missing or out of 0..15)");
                if (!IsBoundedInt(s_Params->Find("variation"), 0, k_MaxVariation, s_Target.variation))
                    return Reject(s_Out, "invalid parameter: variation (missing or out of 0..63)");
                s_Target.outfit_id = s_IdValue->text;

                const Value* s_Expect = s_Params->Find("expect_current");
                if (!s_Expect)
                    return Reject(s_Out, "missing parameter: expect_current");

                OutfitTuple s_Current;
                if (!ReadTuple(*s_Expect, s_Current, s_Why))
                    return Reject(s_Out, "invalid parameter: expect_current (" + s_Why + ")");

                if (!s_Command.mission_generation_precondition)
                    return Reject(s_Out, "missing precondition: mission_generation");

                if (s_Target == s_Current)
                    return Reject(s_Out, "target equals expect_current: a no-op is not an effectful command");

                s_Command.outfit_target = s_Target;
                s_Command.outfit_expect_current = s_Current;
                s_Command.params.emplace_back("disguise_repository_id", s_Target.outfit_id);
                break;
            }

            case Entry::Unknown:
                return Reject(s_Out, "unknown command: " + s_Command.name + "/" + std::to_string(s_Command.version));
        }

        s_Out.ok = true;
        return s_Out;
    }

    bool DecodeWelcome(std::string_view p_Line, Welcome& p_Out, std::string& p_Reason)
    {
        return DecodeWelcome(p_Line, k_ProtocolVersion, p_Out, p_Reason);
    }

    bool DecodeWelcome(std::string_view p_Line, uint32_t p_ExpectedVersion, Welcome& p_Out, std::string& p_Reason)
    {
        if (p_Line.size() > k_MaxLineBytes)
        {
            p_Reason = "line too long";
            return false;
        }

        const auto s_Parsed = CommandJson::Parse(p_Line);
        if (!s_Parsed.ok)
        {
            p_Reason = "invalid json: " + s_Parsed.error;
            return false;
        }

        const Value& s_Root = s_Parsed.value;
        uint64_t s_Version = 0;

        if (s_Root.kind != Value::Kind::Object || !IsUnsigned(s_Root.Find("command_protocol_version"), s_Version) || s_Version != static_cast<uint64_t>(p_ExpectedVersion))
        {
            p_Reason = "protocol version mismatch (expected " + std::to_string(p_ExpectedVersion) + ")";
            return false;
        }

        const Value* s_Kind = s_Root.Find("kind");
        if (!IsText(s_Kind, 32) || s_Kind->text != "welcome")
        {
            p_Reason = "not a welcome";
            return false;
        }

        std::string s_Unknown;
        const bool s_Known = p_ExpectedVersion == k_ProtocolVersion2
            ? KnownKeys(s_Root, {"command_protocol_version", "kind", "link_generation", "node", "allowlist", "nonce", "attest_required"}, s_Unknown)
            : KnownKeys(s_Root, {"command_protocol_version", "kind", "link_generation", "node", "allowlist"}, s_Unknown);
        if (!s_Known)
        {
            p_Reason = "unknown field: " + s_Unknown;
            return false;
        }

        uint64_t s_Generation = 0;
        if (!IsUnsigned(s_Root.Find("link_generation"), s_Generation) || s_Generation == 0)
        {
            p_Reason = "invalid link_generation";
            return false;
        }

        const Value* s_Node = s_Root.Find("node");
        if (!IsText(s_Node, 128))
        {
            p_Reason = "invalid node";
            return false;
        }

        const Value* s_Allowlist = s_Root.Find("allowlist");
        if (!s_Allowlist || s_Allowlist->kind != Value::Kind::Array || s_Allowlist->items.size() > 32)
        {
            p_Reason = "invalid allowlist";
            return false;
        }

        p_Out.protocol_version = p_ExpectedVersion;
        p_Out.link_generation = s_Generation;
        p_Out.node = s_Node->text;
        p_Out.allowlist.clear();
        p_Out.nonce.clear();
        p_Out.attest_required = false;

        for (const auto& s_Item : s_Allowlist->items)
        {
            uint64_t s_EntryVersion = 0;
            const Value* s_Name = s_Item.Find("name");
            if (s_Item.kind != Value::Kind::Object || !IsText(s_Name, 64) || !IsUnsigned(s_Item.Find("version"), s_EntryVersion))
            {
                p_Reason = "invalid allowlist entry";
                return false;
            }
            p_Out.allowlist.emplace_back(s_Name->text, static_cast<uint32_t>(s_EntryVersion));
        }

        if (p_ExpectedVersion == k_ProtocolVersion2)
        {
            const Value* s_Nonce = s_Root.Find("nonce");
            if (!IsText(s_Nonce, 32) || !Attestation::ValidNonce(s_Nonce->text))
            {
                p_Reason = "invalid nonce";
                return false;
            }

            const Value* s_Required = s_Root.Find("attest_required");
            if (!s_Required || s_Required->kind != Value::Kind::Bool || !s_Required->boolean)
            {
                p_Reason = "attest_required missing or false";
                return false;
            }

            p_Out.nonce = s_Nonce->text;
            p_Out.attest_required = true;
        }

        return true;
    }

    bool IsRefusal(std::string_view p_Line, std::string& p_Reason)
    {
        if (p_Line.size() > k_MaxLineBytes)
            return false;

        const auto s_Parsed = CommandJson::Parse(p_Line);
        if (!s_Parsed.ok || s_Parsed.value.kind != Value::Kind::Object)
            return false;

        const Value* s_Kind = s_Parsed.value.Find("kind");
        if (!IsText(s_Kind, 32) || s_Kind->text != "refused")
            return false;

        const Value* s_Why = s_Parsed.value.Find("reason");
        p_Reason = IsText(s_Why, 64) ? s_Why->text : "unspecified";
        return true;
    }

    std::string EncodeHello(const Hello& p_Hello)
    {
        std::string s_Capabilities = "[";
        bool s_First = true;
        size_t s_Count = 0;

        for (const auto& s_Capability : p_Hello.capabilities)
        {
            if (++s_Count > 16)
                break;
            if (!s_First)
                s_Capabilities += ",";
            s_First = false;
            s_Capabilities += Json::Quote(Truncate(s_Capability, 64));
        }
        s_Capabilities += "]";

        return fmt::format(
            "{{\"command_protocol_version\":{},\"kind\":\"hello\",\"adapter_instance_id\":{},\"native_at\":{},\"mission_generation\":{},\"capabilities\":{}}}\n",
            p_Hello.protocol_version, Json::Quote(p_Hello.adapter_instance_id), Json::Quote(Truncate(p_Hello.native_at, 64)),
            p_Hello.mission_generation ? Json::Quote(Truncate(*p_Hello.mission_generation, k_MaxMissionGenerationBytes)) : "null",
            s_Capabilities
        );
    }

    std::string EncodeAttest(const Attest& p_Attest)
    {
        return fmt::format(
            "{{\"command_protocol_version\":{},\"kind\":\"attest\",\"link_generation\":{},\"adapter_instance_id\":{},\"mac\":{}}}\n",
            k_ProtocolVersion2, p_Attest.link_generation, Json::Quote(p_Attest.adapter_instance_id), Json::Quote(Truncate(p_Attest.mac, 64))
        );
    }

    namespace
    {
        std::string ScalarJson(const ObservedValue& p_Value)
        {
            switch (p_Value.kind)
            {
                case ObservedValue::Kind::Null: return "null";
                case ObservedValue::Kind::Bool: return p_Value.boolean ? "true" : "false";
                case ObservedValue::Kind::Integer: return std::to_string(p_Value.integer);
                case ObservedValue::Kind::Number: return fmt::format("{}", p_Value.number);
                case ObservedValue::Kind::Text: return Json::Quote(Truncate(p_Value.text, k_MaxObservedTextBytes));
            }

            return "null";
        }

        std::string ObservedJson(const ObservedValue& p_Value)
        {
            if (!p_Value.field_status)
                return ScalarJson(p_Value);

            // M5 C-1 field object: the value only when observed, the reason only when given.
            std::string s_Object = "{\"status\":\"";
            s_Object += FieldStatusText(*p_Value.field_status);
            s_Object += "\"";

            if (*p_Value.field_status == FieldStatus::Observed && p_Value.kind != ObservedValue::Kind::Null)
            {
                s_Object += ",\"value\":";
                s_Object += ScalarJson(p_Value);
            }

            if (!p_Value.field_reason.empty())
            {
                s_Object += ",\"reason\":";
                s_Object += Json::Quote(Truncate(p_Value.field_reason, k_MaxObservedReasonBytes));
            }

            s_Object += "}";
            return s_Object;
        }
    }

    std::string EncodeResult(const Result& p_Result)
    {
        std::string s_Observed;

        if (p_Result.observed.empty())
        {
            s_Observed = "null";
        }
        else
        {
            s_Observed = "{";
            size_t s_Count = 0;
            for (const auto& s_Entry : p_Result.observed)
            {
                if (++s_Count > k_MaxObservedEntries)
                    break;
                if (s_Count > 1)
                    s_Observed += ",";
                s_Observed += Json::Quote(Truncate(s_Entry.first, 64));
                s_Observed += ":";
                s_Observed += ObservedJson(s_Entry.second);
            }
            s_Observed += "}";
        }

        return fmt::format(
            "{{\"command_protocol_version\":{},\"kind\":\"command.result\",\"command_id\":{},\"link_generation\":{},\"adapter_instance_id\":{},\"status\":\"{}\",\"reason\":{},\"native_at\":{},\"frame\":{},\"observed\":{},\"mission_generation\":{}}}\n",
            p_Result.protocol_version, Json::Quote(p_Result.command_id), p_Result.link_generation,
            p_Result.adapter_instance_id.empty() ? "null" : Json::Quote(p_Result.adapter_instance_id),
            StatusText(p_Result.status),
            p_Result.reason.empty() ? "null" : Json::Quote(Truncate(p_Result.reason, k_MaxReasonBytes)),
            p_Result.native_at.empty() ? "null" : Json::Quote(Truncate(p_Result.native_at, 64)),
            p_Result.frame ? std::to_string(*p_Result.frame) : "null",
            s_Observed,
            p_Result.mission_generation ? Json::Quote(Truncate(*p_Result.mission_generation, k_MaxMissionGenerationBytes)) : "null"
        );
    }
}
