#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The local motion link's protocol, version 1, as BEAM's GlacierRelay.Motion.Codec defines it
// (glacier-relay docs/design/M7_NETWORK_GHOST.md sections 15.5 and 17; M7-B2): lines of at most
// 1,024 bytes on 127.0.0.1:4750, one UTF-8 JSON object per line carrying "motion_protocol_version"
// and a "kind". Decoding is strict, as BEAM's own: a kind is accepted only with exactly its
// declared fields, each present, of its type and within its bounds; an unknown kind, field or
// version is an error and never execution (the link closes on it). Positions are exact binary32
// words (FloatWords): "x,y" on a target or a postimage, "x,y,z" on a hero sample. Nothing here is
// an engine type, a pointer or an address, nothing here touches the engine or a socket, and the
// run token never appears here (the MAC is computed by Attestation and only encoded).
//
// BEAM -> native: motion.welcome, motion.refused, ghost.target, ghost.close, share.close.
// Native -> BEAM: motion.hello, motion.attest, hero.sample, share.closed, ghost.applied,
// ghost.report, ghost.closed.
namespace MotionProtocol
{
    constexpr int k_Version = 1;
    constexpr size_t k_MaxLineBytes = 1024;
    constexpr size_t k_MaxReasonBytes = 64;
    constexpr uint64_t k_MaxGeneration = 2147483647; // BEAM's {:integer, 1, 2_147_483_647}

    enum class Kind
    {
        Welcome,
        Refused,
        Target,
        Close,
        ShareClose,
    };

    const char* KindText(Kind p_Kind);

    // One decoded BEAM line, as plain values (only the fields of its kind are meaningful).
    struct Message
    {
        Kind kind = Kind::Refused;
        uint64_t motion_link_generation = 0; // welcome
        std::string nonce;                   // welcome: 32 lowercase hex
        std::string reason;                  // refused, close, share.close (<= 64 bytes)
        std::string id;                      // target / close: session_id; share.close: share_id
        uint64_t seq = 0;                    // target (> 0)
        uint32_t x = 0;                      // target: the world x word
        uint32_t y = 0;                      // target: the world y word
    };

    struct Decoded
    {
        bool ok = false;
        std::string error; // bounded, when !ok
        Message message;
    };

    // Strict decoding of one line BEAM sent (without its newline).
    Decoded DecodeFromBeam(std::string_view p_Line);

    // A reason as bounded text: at most 64 bytes, cut on a UTF-8 boundary.
    std::string ReasonText(std::string_view p_Reason);

    // ---- native -> BEAM (each one line with its newline, deterministic field order) ----

    std::string EncodeHello(std::string_view p_AdapterInstanceId, uint64_t p_CommandLinkGeneration);
    std::string EncodeAttest(uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId, std::string_view p_Mac);

    std::string EncodeHeroSample(std::string_view p_ShareId, uint64_t p_Seq, uint64_t p_Frame, uint64_t p_NativeMonoMs, const uint32_t p_Pos[3], bool p_Dirty);
    std::string EncodeShareClosed(std::string_view p_ShareId, std::string_view p_Reason, std::optional<uint64_t> p_LastSeq, uint64_t p_Sent);

    enum class Outcome
    {
        Applied,
        SkippedIdentical,
        Refused,
        Uncertain,
    };

    const char* OutcomeText(Outcome p_Outcome);

    struct Applied
    {
        std::string session_id;
        uint64_t seq = 0;
        uint64_t frame = 0;
        uint64_t native_mono_ms = 0;
        Outcome outcome = Outcome::Refused;
        std::optional<std::string> reason;
        std::optional<uint32_t> post_x; // both or neither
        std::optional<uint32_t> post_y;
        std::optional<bool> exact;
    };

    std::string EncodeApplied(const Applied& p_Applied);

    struct Counts
    {
        uint64_t received = 0;
        uint64_t applied = 0;
        uint64_t skipped = 0;
        uint64_t refused = 0;
        uint64_t uncertain = 0;
        uint64_t superseded = 0;
    };

    struct Report
    {
        std::string session_id;
        uint64_t report_seq = 0;
        uint64_t native_mono_ms = 0;
        uint64_t frame = 0;
        Counts counts;
        std::optional<uint64_t> last_applied_seq;
    };

    std::string EncodeReport(const Report& p_Report);

    struct Closed
    {
        std::string session_id;
        std::string reason;
        uint64_t native_mono_ms = 0;
        bool clean = false;
        std::optional<std::string> final_world; // twelve words (FloatWords::Encode), only when qualified
        std::optional<uint64_t> last_applied_seq;
        uint64_t applied = 0;
    };

    std::string EncodeClosed(const Closed& p_Closed);

    // The longest line each native kind can produce with the bounded inputs above (for the line-bound
    // test and the capacity statement): every id 36 bytes, every counter at UINT64_MAX, reasons at 64
    // bytes of the worst-case escaping BEAM accepts.
    size_t WorstCaseLineBytes(std::string_view p_Kind);
}
