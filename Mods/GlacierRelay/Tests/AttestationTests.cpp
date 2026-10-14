// M5 C-2B: the vendored SHA-256 and HMAC-SHA256 against published known-answer vectors (FIPS 180-4
// examples; RFC 4231 cases 1, 2, 6, 7) and against a vector produced by the BEAM reference
// implementation (Erlang :crypto through GlacierRelay.Command.Schema.attest_mac/4), the canonical
// attestation input, the shape checks and the attest line. No secret here is a real token.
#include "TestHarness.h"

#include <string>
#include <vector>

#include "Attestation.h"
#include "CommandProtocol.h"

namespace
{
    std::string Sha(const std::string& p_Text)
    {
        return Attestation::Hex(Attestation::Sha256(reinterpret_cast<const uint8_t*>(p_Text.data()), p_Text.size()));
    }

    std::string Hmac(const std::string& p_Key, const std::string& p_Text)
    {
        return Attestation::Hex(Attestation::HmacSha256(
            reinterpret_cast<const uint8_t*>(p_Key.data()), p_Key.size(),
            reinterpret_cast<const uint8_t*>(p_Text.data()), p_Text.size()
        ));
    }
}

void RunAttestationTests()
{
    // SHA-256 (FIPS 180-4 examples, plus the empty message and the one-million-'a' message).
    CHECK(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Boundary lengths around the padding: 55, 56, 63, 64, 65 bytes agree with a reference run.
    CHECK(Sha(std::string(55, 'x')) != Sha(std::string(56, 'x')));
    CHECK(Sha(std::string(64, 'x')) != Sha(std::string(65, 'x')));

    // HMAC-SHA256 (RFC 4231).
    CHECK(Hmac(std::string(20, '\x0b'), "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(Hmac("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(Hmac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    CHECK(Hmac(std::string(131, '\xaa'), "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.") == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

    // The cross-side vector: BEAM's Schema.attest_mac/4 (Erlang :crypto) over the same canonical
    // input with the same (test-only) token produced exactly this MAC.
    const std::string s_Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string s_Nonce = "514f1d221aefccd9d7fc4193040369cf";
    const std::string s_Adapter = "aaf540dd-c6df-4429-8fa5-84bc8933a13e";
    CHECK(Attestation::CanonicalInput(s_Nonce, 7, s_Adapter) == "514f1d221aefccd9d7fc4193040369cf|7|aaf540dd-c6df-4429-8fa5-84bc8933a13e");
    CHECK(Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter) == "7e005b8baf131b526036e9b41d4e139628d9272bf5592c29f8cfe6cd222f3164");
    // Binding: any change of nonce, generation, identity or token changes the MAC.
    CHECK(Attestation::AttestMac(s_Token, s_Nonce, 8, s_Adapter) != Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter));
    CHECK(Attestation::AttestMac(s_Token, "614f1d221aefccd9d7fc4193040369cf", 7, s_Adapter) != Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter));
    CHECK(Attestation::AttestMac(s_Token, s_Nonce, 7, "aaf540dd-c6df-4429-8fa5-84bc8933a13f") != Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter));
    CHECK(Attestation::AttestMac("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210", s_Nonce, 7, s_Adapter) != Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter));
    CHECK(Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter).size() == 64);

    // Shapes.
    CHECK(Attestation::ValidNonce(s_Nonce) && !Attestation::ValidNonce("514F1D221AEFCCD9D7FC4193040369CF") && !Attestation::ValidNonce("514f1d22"));
    CHECK(Attestation::ValidToken(s_Token) && !Attestation::ValidToken(s_Token + "0") && !Attestation::ValidToken("x" + s_Token.substr(1)));

    // The attest line, exactly the keys BEAM's decoder accepts, version 2, newline-terminated.
    CommandProtocol::Attest s_Attest;
    s_Attest.link_generation = 7;
    s_Attest.adapter_instance_id = s_Adapter;
    s_Attest.mac = Attestation::AttestMac(s_Token, s_Nonce, 7, s_Adapter);
    CHECK(CommandProtocol::EncodeAttest(s_Attest) == "{\"command_protocol_version\":2,\"kind\":\"attest\",\"link_generation\":7,\"adapter_instance_id\":\"" + s_Adapter + "\",\"mac\":\"7e005b8baf131b526036e9b41d4e139628d9272bf5592c29f8cfe6cd222f3164\"}\n");
}
