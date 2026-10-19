#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The attestation primitives of the command protocol version 2 (M5 C-2B; glacier-relay
// Command.Schema.attest_mac/4, M5 design sections 26.2 and 27.2): a vendored SHA-256 and
// HMAC-SHA256 (FIPS 180-4 / RFC 2104), so the DLL imports no cryptographic library — the import
// set stays explainable name by name. Both are checked against published known-answer vectors
// (FIPS 180-4 examples, RFC 4231 cases) and against a vector produced by the BEAM reference
// implementation (Erlang :crypto) in Tests/AttestationTests.cpp. Nothing here touches the engine,
// the socket or a file; nothing here logs. The run token never leaves the link's memory.
namespace Attestation
{
    using Digest = std::array<uint8_t, 32>;

    Digest Sha256(const uint8_t* p_Data, size_t p_Length);
    Digest HmacSha256(const uint8_t* p_Key, size_t p_KeyLength, const uint8_t* p_Data, size_t p_DataLength);

    // Lowercase hex of a digest (64 characters).
    std::string Hex(const Digest& p_Digest);

    // The canonical input is "<nonce>|<link_generation>|<adapter_instance_id>" (the welcome's nonce
    // as the lowercase hex BEAM sent it, the generation in decimal, the adapter id as dashed
    // lowercase text); the key is the run token's text bytes exactly as configured (64 lowercase
    // hex characters, never decoded). The MAC is the lowercase hex of HMAC-SHA256.
    std::string CanonicalInput(std::string_view p_Nonce, uint64_t p_LinkGeneration, std::string_view p_AdapterInstanceId);
    std::string AttestMac(std::string_view p_Token, std::string_view p_Nonce, uint64_t p_LinkGeneration, std::string_view p_AdapterInstanceId);

    // M7-B2: the motion link's attestation (glacier-relay Command.Schema.motion_attest_mac/5; M7
    // design section 15.5). The canonical input is
    // "motion|<nonce>|<motion_link_generation>|<command_link_generation>|<adapter_instance_id>": the
    // "motion|" prefix separates it from the command link's input, so neither MAC answers the other's
    // challenge. Same key, same HMAC-SHA256, same lowercase hex; never logged.
    std::string MotionCanonicalInput(std::string_view p_Nonce, uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId);
    std::string MotionAttestMac(std::string_view p_Token, std::string_view p_Nonce, uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId);

    // Shapes the protocol accepts: a nonce is 32 lowercase hex characters, a token 64.
    bool ValidNonce(std::string_view p_Text);
    bool ValidToken(std::string_view p_Text);
}
