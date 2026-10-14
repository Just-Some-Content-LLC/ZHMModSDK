#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

// A minimal loopback listener standing in for BEAM's command listener in the command tests and
// the command probe's self-check: accept one client, read and write lines with timeouts. Test
// tooling only (the M2 sink tests keep their own copy untouched).
namespace CommandTest
{
    using Clock = std::chrono::steady_clock;

    struct Listener
    {
        SOCKET Listen = INVALID_SOCKET;
        SOCKET Client = INVALID_SOCKET;
        uint16_t Port = 0;

        bool Start(uint16_t p_Port)
        {
            Listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in s_Address = {};
            s_Address.sin_family = AF_INET;
            s_Address.sin_port = htons(p_Port);
            inet_pton(AF_INET, "127.0.0.1", &s_Address.sin_addr);
            const BOOL s_Reuse = TRUE;
            setsockopt(Listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&s_Reuse), sizeof(s_Reuse));

            if (bind(Listen, reinterpret_cast<sockaddr*>(&s_Address), sizeof(s_Address)) != 0 || listen(Listen, 4) != 0)
                return false;

            int s_Length = sizeof(s_Address);
            getsockname(Listen, reinterpret_cast<sockaddr*>(&s_Address), &s_Length);
            Port = ntohs(s_Address.sin_port);
            return true;
        }

        bool Accept(uint32_t p_TimeoutMs)
        {
            fd_set s_Readable;
            FD_ZERO(&s_Readable);
            FD_SET(Listen, &s_Readable);
            timeval s_Timeout = {static_cast<long>(p_TimeoutMs / 1000), static_cast<long>((p_TimeoutMs % 1000) * 1000)};

            if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
                return false;

            Client = accept(Listen, nullptr, nullptr);
            return Client != INVALID_SOCKET;
        }

        // Reads one line (without the newline) or "" on timeout / close.
        std::string ReadLine(uint32_t p_TimeoutMs)
        {
            std::string s_Line;
            const auto s_Deadline = Clock::now() + std::chrono::milliseconds(p_TimeoutMs);

            while (Clock::now() < s_Deadline)
            {
                fd_set s_Readable;
                FD_ZERO(&s_Readable);
                FD_SET(Client, &s_Readable);
                timeval s_Timeout = {0, 20000};

                if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
                    continue;

                char s_Char;
                const int s_Received = recv(Client, &s_Char, 1, 0);

                if (s_Received <= 0)
                    return "";

                if (s_Char == '\n')
                    return s_Line;

                s_Line.push_back(s_Char);
            }

            return "";
        }

        bool Send(const std::string& p_Text)
        {
            return send(Client, p_Text.data(), static_cast<int>(p_Text.size()), 0) == static_cast<int>(p_Text.size());
        }

        bool PeerClosed(uint32_t p_TimeoutMs)
        {
            const auto s_Deadline = Clock::now() + std::chrono::milliseconds(p_TimeoutMs);

            while (Clock::now() < s_Deadline)
            {
                fd_set s_Readable;
                FD_ZERO(&s_Readable);
                FD_SET(Client, &s_Readable);
                timeval s_Timeout = {0, 20000};

                if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) > 0)
                {
                    char s_Peek;
                    const int s_Received = recv(Client, &s_Peek, 1, MSG_PEEK);
                    if (s_Received <= 0)
                        return true;
                    // Data is waiting, not a close; drain one byte to make progress.
                    recv(Client, &s_Peek, 1, 0);
                }
            }

            return false;
        }

        void CloseClient()
        {
            if (Client != INVALID_SOCKET)
                closesocket(Client);
            Client = INVALID_SOCKET;
        }

        void Stop()
        {
            CloseClient();
            if (Listen != INVALID_SOCKET)
                closesocket(Listen);
            Listen = INVALID_SOCKET;
        }
    };

    inline uint16_t FreePort()
    {
        Listener s_Probe;
        s_Probe.Start(0);
        const uint16_t s_Port = s_Probe.Port;
        s_Probe.Stop();
        return s_Port;
    }

    template <typename TPredicate>
    bool WaitUntil(uint32_t p_TimeoutMs, const TPredicate& p_Predicate)
    {
        const auto s_Deadline = Clock::now() + std::chrono::milliseconds(p_TimeoutMs);

        while (Clock::now() < s_Deadline)
        {
            if (p_Predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return p_Predicate();
    }

    inline std::string Welcome(uint64_t p_Generation)
    {
        return "{\"command_protocol_version\":1,\"kind\":\"welcome\",\"link_generation\":" + std::to_string(p_Generation) +
               ",\"node\":\"test\",\"allowlist\":[{\"name\":\"relay.probe\",\"version\":1}]}\n";
    }

    inline std::string Command(const std::string& p_Id, uint64_t p_Generation, uint32_t p_Ttl = 5000, const std::string& p_Name = "relay.probe", uint32_t p_Version = 1, const std::string& p_Params = "{}", const std::string& p_Preconditions = "{}", uint32_t p_ProtocolVersion = 1)
    {
        return "{\"command_protocol_version\":" + std::to_string(p_ProtocolVersion) + ",\"kind\":\"command\",\"command_id\":\"" + p_Id + "\",\"link_generation\":" + std::to_string(p_Generation) +
               ",\"issued_at\":\"2026-10-14T00:00:00.000Z\",\"ttl_ms\":" + std::to_string(p_Ttl) + ",\"name\":\"" + p_Name + "\",\"version\":" + std::to_string(p_Version) +
               ",\"params\":" + p_Params + ",\"preconditions\":" + p_Preconditions + "}\n";
    }

    // M5 C-2B: the version-2 welcome BEAM sends (a nonce and attest_required), and the complete
    // player.outfit.set v2 parameter object.
    inline std::string WelcomeV2(uint64_t p_Generation, const std::string& p_Nonce, bool p_Effectful = true)
    {
        return "{\"command_protocol_version\":2,\"kind\":\"welcome\",\"link_generation\":" + std::to_string(p_Generation) +
               ",\"node\":\"test\",\"allowlist\":[" + std::string(p_Effectful ? "{\"name\":\"player.outfit.set\",\"version\":2}," : "") +
               "{\"name\":\"relay.inspect.outfit\",\"version\":1},{\"name\":\"relay.probe\",\"version\":1}],\"nonce\":\"" + p_Nonce + "\",\"attest_required\":true}\n";
    }

    inline std::string OutfitParams(const std::string& p_Target, int p_Charset, int p_Variation, const std::string& p_Current, int p_CurrentCharset = 0, int p_CurrentVariation = 0)
    {
        return "{\"disguise_repository_id\":\"" + p_Target + "\",\"charset\":" + std::to_string(p_Charset) + ",\"variation\":" + std::to_string(p_Variation) +
               ",\"expect_current\":{\"disguise_repository_id\":\"" + p_Current + "\",\"charset\":" + std::to_string(p_CurrentCharset) + ",\"variation\":" + std::to_string(p_CurrentVariation) + "}}";
    }

    // Deterministic UUID v4 forms for tests: aaaaaaaa-aaaa-4aaa-8aaa-00000000000N.
    inline std::string Uuid(unsigned p_N)
    {
        char s_Buffer[64];
        snprintf(s_Buffer, sizeof(s_Buffer), "aaaaaaaa-aaaa-4aaa-8aaa-%012u", p_N);
        return s_Buffer;
    }
}
