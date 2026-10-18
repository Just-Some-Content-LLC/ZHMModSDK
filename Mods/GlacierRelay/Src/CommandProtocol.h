#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FloatWords.h"

// The command protocol, versions 1 and 2, as BEAM's GlacierRelay.Command.Schema defines it
// (glacier-relay docs/design/M5_COMMAND_CHANNEL.md sections 5, 17.4, 26.2 and 27; ADR 0009). The
// native side decodes what BEAM writes (welcome, command, refused) and encodes what BEAM reads
// (hello, attest, command.result), strictly and within the same bounds: a 4,096-byte line, a
// validated UUID v4 command id, a positive link generation, a known command name and version
// from the compiled allowlist, a TTL within 100..10,000 ms, parameters typed per entry, a bounded
// precondition object, a closed status vocabulary, a reason of at most 200 bytes and a read-back
// of at most 48 entries (strings at most 128 bytes). Unknown kinds, names, versions, fields or
// parameters never reach execution.
//
// Versions (M5 C-2B): the native side chooses the version in its hello — 1 (the frozen C-1
// protocol, byte for byte) when no run token is configured, 2 when one is — and every later line
// of the link carries that version; a line of the other version is a violation. Version 2 adds the
// attestation exchange: the welcome carries a fresh nonce and the native answers with one attest
// whose MAC is HMAC-SHA256 with the run token over "nonce|link_generation|adapter_instance_id"
// (Attestation.h). Version 2 is also where player.outfit.set version 2 — the complete outfit tuple
// — may be decoded, and only when the effectful setting is on; otherwise the entry is rejected
// exactly as the reserved version 1 is. Nothing here is an engine type, a pointer, an offset, a
// function name or a script, and nothing here touches the engine.
namespace CommandProtocol
{
    constexpr int k_ProtocolVersion = 1;  // the frozen C-1 protocol
    constexpr int k_ProtocolVersion2 = 2; // M5 C-2B: adds the attestation exchange
    constexpr size_t k_MaxLineBytes = 4096;
    constexpr uint32_t k_MinTtlMs = 100;
    constexpr uint32_t k_MaxTtlMs = 10000;
    constexpr size_t k_MaxReasonBytes = 200;
    // M5 C-1 raised the read-back bound from 16 to 48 entries (BEAM's schema agrees) so an
    // inspection can report every field with its own status; the line bound is unchanged and the
    // scheduler reduces a result that would not fit rather than exceeding it.
    constexpr size_t k_MaxObservedEntries = 48;
    constexpr size_t k_MaxObservedTextBytes = 128;
    constexpr size_t k_MaxObservedReasonBytes = 40;
    constexpr size_t k_MaxMissionGenerationBytes = 64;
    // player.outfit.set v2 bounds (the same as BEAM's schema): charset 0..15, variation 0..63.
    constexpr int32_t k_MaxCharset = 15;
    constexpr int32_t k_MaxVariation = 63;

    // The per-field status of an inspection read-back (M5 C-1). A field with a status other than
    // Observed carries no value: nothing is defaulted, zeroed or assumed.
    enum class FieldStatus
    {
        Observed,
        Unavailable,
        Unsupported,
        Inconsistent,
        PreconditionFailed,
    };

    const char* FieldStatusText(FieldStatus p_Status);

    // The closed status vocabulary (the same nine words as BEAM's schema).
    enum class Status
    {
        Received,
        Rejected,
        Busy,
        Duplicate,
        Expired,
        PreconditionFailed,
        Unavailable,
        Applied,
        Failed,
    };

    const char* StatusText(Status p_Status);

    // The compiled allowlist. relay.probe v1 and relay.inspect.outfit v1 (M5 C-1; read-only, one
    // optional validated repository id) are executable; relay.inspect.players v1 and
    // relay.inspect.entity v1 (M6-B1; read-only: the player registry's four declared slots, the
    // loaded-brick inventory, the local hero's authored identity and the blueprint-factory lookup
    // round trip, with two optional 16-hex-digit parameters for the candidate brick and entity id)
    // are executable and inert; relay.inspect.candidate v1 (M6-D1a; read-only: the candidate
    // hero's and the local hero's stored spatial fields, member presence and m_bActive property,
    // the spawn entity's stored transform and properties, registry membership by identity, with
    // the same two optional 16-hex-digit parameters as relay.inspect.entity) is executable and
    // inert; player.outfit.set v1 is reserved (documented, decoded, never executed);
    // player.outfit.set v2 (M5 C-2B) is the effectful entry, decodable only on a version-2 link
    // with the effectful setting on; relay.inspect.candidate_transform v1 (M6-F; read-only, no
    // parameters: the compiled candidate's and the local hero's exact stored transform words) is
    // executable and inert; candidate.transform.set v1 (M6-F) is the second effectful entry,
    // decodable under the same setting: a "lift" (the compiled +0.25 world-z displacement of the
    // compiled candidate, computed natively) or a "restore" (the journaled preimage world group of
    // one earlier lift, bound to it), each with the exact expectation groups of a recent exact
    // inspection; candidate.transform.set v2 (M6-G1) is the third effectful entry, decodable under the
    // same setting: a "step_y" (the compiled +0.25 world-y displacement of the compiled candidate,
    // computed natively) or a "restore" (the journaled preimage of one earlier v2 step, bound to it);
    // the capability identity is the (name, version) pair — a v1 restore restores only a v1 lift and
    // a v2 restore only a v2 step; any other name/version is unknown.
    enum class Entry
    {
        Unknown,
        RelayProbe1,
        RelayInspectOutfit1,
        PlayerOutfitSet1Reserved,
        PlayerOutfitSet2,
        RelayInspectPlayers1,
        RelayInspectEntity1,
        RelayInspectCandidate1,
        RelayInspectCandidateTransform1,
        CandidateTransformSet1,
        CandidateTransformSet2,
    };

    // candidate.transform.set v1 (M6-F; design section 12.5) and v2 (M6-G1; glacier-relay
    // docs/design/M6G_VISIBLE_CONTROL.md section 6): the mode, the exact expectation of the current
    // state (the world and parent-relative groups and the parent id of a recent exact inspection),
    // and for a restore the journaled preimage world group and the original command it restores.
    // Nothing else: no location, direction, rotation, scale, distance, entity, pointer or function
    // name is a parameter. The allowed pairs: v1 lift | restore, v2 step_y | restore; every other
    // pair is refused at decode and again by the apply's gate.
    enum class TransformMode
    {
        Lift,    // v1 only: the compiled +0.25f world-z displacement
        Restore, // v1 and v2: restores a forward of the same entry version
        StepY,   // v2 only: the compiled +0.25f world-y displacement
    };

    const char* TransformModeText(TransformMode p_Mode);

    struct TransformRequest
    {
        TransformMode mode = TransformMode::Lift;
        uint32_t version = 1;                               // the entry version the request was decoded under (1 or 2)
        FloatWords::Words12 expect_world;
        FloatWords::Words7 expect_local;
        std::string expect_parent;                          // 16 lowercase hex, or "none"
        std::optional<FloatWords::Words12> target_world;    // restore only
        std::optional<std::string> restore_of;              // restore only: the original lift's command id
    };

    Entry Lookup(std::string_view p_Name, uint32_t p_Version);
    bool IsEffectful(Entry p_Entry);

    bool ValidUuid(std::string_view p_Text);
    bool ValidHex16(std::string_view p_Text); // exactly 16 lowercase hex digits (a runtime resource id or an authored entity id)

    // A complete outfit tuple: the kit definition's repository id (dashed lowercase), the charset
    // and the variation. The wire carries nothing else about an outfit.
    struct OutfitTuple
    {
        std::string outfit_id;
        int32_t charset = 0;
        int32_t variation = 0;

        bool operator==(const OutfitTuple& p_Other) const
        {
            return outfit_id == p_Other.outfit_id && charset == p_Other.charset && variation == p_Other.variation;
        }

        bool operator!=(const OutfitTuple& p_Other) const { return !(*this == p_Other); }
    };

    struct Command
    {
        std::string command_id;
        uint64_t link_generation = 0;
        std::string issued_at; // audit text only; never compared with a clock
        uint32_t ttl_ms = 0;
        std::string name;
        uint32_t version = 0;
        Entry entry = Entry::Unknown;
        std::vector<std::pair<std::string, std::string>> params; // validated per entry; string-valued
        std::optional<std::string> mission_generation_precondition;

        // player.outfit.set v2 only: the requested tuple and the mandatory expected-current tuple.
        std::optional<OutfitTuple> outfit_target;
        std::optional<OutfitTuple> outfit_expect_current;

        // candidate.transform.set v1 only (M6-F).
        std::optional<TransformRequest> transform;
    };

    struct DecodedCommand
    {
        bool ok = false;
        std::string reason; // when !ok: why the line was rejected (bounded, for the result/log)
        // When the line was well formed enough to carry an id and generation, they are kept so a
        // rejection can be answered; otherwise both are empty/zero and the line is a violation.
        std::string command_id;
        uint64_t link_generation = 0;
        Command command;
    };

    // What the link negotiated: the version every line must carry, and whether the effectful
    // entry may be decoded at all (the [command] effectful setting; false by default).
    struct DecodeOptions
    {
        uint32_t protocol_version = k_ProtocolVersion;
        bool effectful = false;
    };

    // Decodes one line (no trailing newline) BEAM sent. A command that decodes but names an
    // unknown or reserved entry, or carries unknown fields or parameters, comes back !ok with the
    // reason and (when present and valid) its id and generation, so the link can answer "rejected".
    // The one-argument form is version 1 without the effectful entry, unchanged since M5-B.
    DecodedCommand DecodeCommand(std::string_view p_Line);
    DecodedCommand DecodeCommand(std::string_view p_Line, const DecodeOptions& p_Options);

    struct Welcome
    {
        uint32_t protocol_version = k_ProtocolVersion;
        uint64_t link_generation = 0;
        std::string node;
        std::vector<std::pair<std::string, uint32_t>> allowlist;
        std::string nonce;            // version 2: 32 lowercase hex characters
        bool attest_required = false; // version 2: always true
    };

    // Decodes BEAM's welcome; false with a reason when it is not a valid welcome of the expected
    // version (a welcome of the other version is a mismatch, never a downgrade or an upgrade).
    bool DecodeWelcome(std::string_view p_Line, Welcome& p_Out, std::string& p_Reason); // version 1
    bool DecodeWelcome(std::string_view p_Line, uint32_t p_ExpectedVersion, Welcome& p_Out, std::string& p_Reason);

    // True when the line is BEAM's "refused" message (a second link while one is up).
    bool IsRefusal(std::string_view p_Line, std::string& p_Reason);

    struct Hello
    {
        uint32_t protocol_version = k_ProtocolVersion;
        std::string adapter_instance_id;
        std::string native_at;
        std::optional<std::string> mission_generation;
        std::vector<std::string> capabilities;
    };

    std::string EncodeHello(const Hello& p_Hello); // one line with its newline

    // Version 2: the one attest line after the welcome. The MAC is computed by the link
    // (Attestation::AttestMac); this only encodes it.
    struct Attest
    {
        uint64_t link_generation = 0;
        std::string adapter_instance_id;
        std::string mac; // 64 lowercase hex characters
    };

    std::string EncodeAttest(const Attest& p_Attest); // one line with its newline

    struct ObservedValue
    {
        enum class Kind { Null, Bool, Integer, Number, Text };
        Kind kind = Kind::Null;
        bool boolean = false;
        int64_t integer = 0;
        double number = 0.0;
        std::string text; // truncated to k_MaxObservedTextBytes on encoding

        // M5 C-1: when set, the entry encodes as an object {"status":..,"value":..,"reason":..}
        // (value only when the status is Observed and the kind is not Null; reason only when not
        // empty, truncated to k_MaxObservedReasonBytes). Unset: the plain scalar, as relay.probe
        // has always encoded it.
        std::optional<FieldStatus> field_status;
        std::string field_reason;
    };

    struct Result
    {
        uint32_t protocol_version = k_ProtocolVersion; // the link's negotiated version (set by the link)
        std::string command_id;
        uint64_t link_generation = 0;
        std::string adapter_instance_id;
        Status status = Status::Rejected;
        std::string reason;     // truncated to k_MaxReasonBytes on encoding; empty → null
        std::string native_at;  // empty → null
        std::optional<uint64_t> frame;
        std::vector<std::pair<std::string, ObservedValue>> observed; // empty → null; at most 48 kept
        std::optional<std::string> mission_generation;
    };

    std::string EncodeResult(const Result& p_Result); // one line with its newline, deterministic field order
}
