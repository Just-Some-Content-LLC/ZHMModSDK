#include "Attestation.h"

#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t k_K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    inline uint32_t Rotr(uint32_t p_Value, unsigned p_Bits)
    {
        return (p_Value >> p_Bits) | (p_Value << (32 - p_Bits));
    }

    struct Sha256State
    {
        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint8_t block[64] = {};
        size_t block_used = 0;
        uint64_t total = 0;

        void Compress(const uint8_t* p_Block)
        {
            uint32_t w[64];

            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p_Block[4 * i]) << 24) | (uint32_t(p_Block[4 * i + 1]) << 16) | (uint32_t(p_Block[4 * i + 2]) << 8) | uint32_t(p_Block[4 * i + 3]);

            for (int i = 16; i < 64; ++i)
            {
                const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

            for (int i = 0; i < 64; ++i)
            {
                const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                const uint32_t ch = (e & f) ^ (~e & g);
                const uint32_t t1 = hh + S1 + ch + k_K[i] + w[i];
                const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }

            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        void Update(const uint8_t* p_Data, size_t p_Length)
        {
            total += p_Length;

            while (p_Length > 0)
            {
                const size_t s_Take = std::min(p_Length, sizeof(block) - block_used);
                std::memcpy(block + block_used, p_Data, s_Take);
                block_used += s_Take;
                p_Data += s_Take;
                p_Length -= s_Take;

                if (block_used == sizeof(block))
                {
                    Compress(block);
                    block_used = 0;
                }
            }
        }

        Attestation::Digest Finish()
        {
            const uint64_t s_Bits = total * 8;
            const uint8_t s_One = 0x80;
            Update(&s_One, 1);

            const uint8_t s_Zero = 0;
            while (block_used != 56)
                Update(&s_Zero, 1);

            uint8_t s_Length[8];
            for (int i = 0; i < 8; ++i)
                s_Length[i] = static_cast<uint8_t>(s_Bits >> (56 - 8 * i));
            Update(s_Length, 8);

            Attestation::Digest s_Digest;
            for (int i = 0; i < 8; ++i)
            {
                s_Digest[4 * i] = static_cast<uint8_t>(h[i] >> 24);
                s_Digest[4 * i + 1] = static_cast<uint8_t>(h[i] >> 16);
                s_Digest[4 * i + 2] = static_cast<uint8_t>(h[i] >> 8);
                s_Digest[4 * i + 3] = static_cast<uint8_t>(h[i]);
            }
            return s_Digest;
        }
    };

    bool LowerHex(std::string_view p_Text, size_t p_Length)
    {
        if (p_Text.size() != p_Length)
            return false;
        for (const char c : p_Text)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        return true;
    }
}

namespace Attestation
{
    Digest Sha256(const uint8_t* p_Data, size_t p_Length)
    {
        Sha256State s_State;
        if (p_Length > 0)
            s_State.Update(p_Data, p_Length);
        return s_State.Finish();
    }

    Digest HmacSha256(const uint8_t* p_Key, size_t p_KeyLength, const uint8_t* p_Data, size_t p_DataLength)
    {
        uint8_t s_Key[64] = {};

        if (p_KeyLength > 64)
        {
            const Digest s_Hashed = Sha256(p_Key, p_KeyLength);
            std::memcpy(s_Key, s_Hashed.data(), s_Hashed.size());
        }
        else if (p_KeyLength > 0)
        {
            std::memcpy(s_Key, p_Key, p_KeyLength);
        }

        uint8_t s_Inner[64], s_Outer[64];
        for (int i = 0; i < 64; ++i)
        {
            s_Inner[i] = s_Key[i] ^ 0x36;
            s_Outer[i] = s_Key[i] ^ 0x5c;
        }

        Sha256State s_InnerState;
        s_InnerState.Update(s_Inner, 64);
        if (p_DataLength > 0)
            s_InnerState.Update(p_Data, p_DataLength);
        const Digest s_InnerDigest = s_InnerState.Finish();

        Sha256State s_OuterState;
        s_OuterState.Update(s_Outer, 64);
        s_OuterState.Update(s_InnerDigest.data(), s_InnerDigest.size());
        return s_OuterState.Finish();
    }

    std::string Hex(const Digest& p_Digest)
    {
        static const char* k_Digits = "0123456789abcdef";
        std::string s_Hex;
        s_Hex.reserve(64);
        for (const uint8_t s_Byte : p_Digest)
        {
            s_Hex.push_back(k_Digits[s_Byte >> 4]);
            s_Hex.push_back(k_Digits[s_Byte & 0x0f]);
        }
        return s_Hex;
    }

    std::string CanonicalInput(std::string_view p_Nonce, uint64_t p_LinkGeneration, std::string_view p_AdapterInstanceId)
    {
        std::string s_Input(p_Nonce);
        s_Input += '|';
        s_Input += std::to_string(p_LinkGeneration);
        s_Input += '|';
        s_Input += p_AdapterInstanceId;
        return s_Input;
    }

    std::string AttestMac(std::string_view p_Token, std::string_view p_Nonce, uint64_t p_LinkGeneration, std::string_view p_AdapterInstanceId)
    {
        const std::string s_Input = CanonicalInput(p_Nonce, p_LinkGeneration, p_AdapterInstanceId);
        return Hex(HmacSha256(
            reinterpret_cast<const uint8_t*>(p_Token.data()), p_Token.size(),
            reinterpret_cast<const uint8_t*>(s_Input.data()), s_Input.size()
        ));
    }

    std::string MotionCanonicalInput(std::string_view p_Nonce, uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId)
    {
        std::string s_Input("motion|");
        s_Input += p_Nonce;
        s_Input += '|';
        s_Input += std::to_string(p_MotionLinkGeneration);
        s_Input += '|';
        s_Input += std::to_string(p_CommandLinkGeneration);
        s_Input += '|';
        s_Input += p_AdapterInstanceId;
        return s_Input;
    }

    std::string MotionAttestMac(std::string_view p_Token, std::string_view p_Nonce, uint64_t p_MotionLinkGeneration, uint64_t p_CommandLinkGeneration, std::string_view p_AdapterInstanceId)
    {
        const std::string s_Input = MotionCanonicalInput(p_Nonce, p_MotionLinkGeneration, p_CommandLinkGeneration, p_AdapterInstanceId);
        return Hex(HmacSha256(
            reinterpret_cast<const uint8_t*>(p_Token.data()), p_Token.size(),
            reinterpret_cast<const uint8_t*>(s_Input.data()), s_Input.size()
        ));
    }

    bool ValidNonce(std::string_view p_Text)
    {
        return LowerHex(p_Text, 32);
    }

    bool ValidToken(std::string_view p_Text)
    {
        return LowerHex(p_Text, 64);
    }
}
