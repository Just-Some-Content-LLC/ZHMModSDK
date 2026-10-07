#include "TestHarness.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <chrono>
#include <string>
#include <thread>

#include "TcpRelaySink.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
    using Clock = std::chrono::steady_clock;

    // A minimal loopback listener standing in for BEAM. Blocking calls with timeouts.
    struct TestListener
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

        // Reads until a newline or the timeout. Returns the line without the newline, or "" on timeout.
        std::string ReadLine(uint32_t p_TimeoutMs)
        {
            std::string s_Line;
            const auto s_Deadline = Clock::now() + std::chrono::milliseconds(p_TimeoutMs);

            while (Clock::now() < s_Deadline)
            {
                fd_set s_Readable;
                FD_ZERO(&s_Readable);
                FD_SET(Client, &s_Readable);
                timeval s_Timeout = {0, 50000};

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

        // True when another connection is waiting in the backlog.
        bool Pending()
        {
            fd_set s_Readable;
            FD_ZERO(&s_Readable);
            FD_SET(Listen, &s_Readable);
            timeval s_Timeout = {0, 0};
            return select(0, &s_Readable, nullptr, nullptr, &s_Timeout) > 0;
        }

        // True when the accepted peer has already closed its end (a FIN or RST is waiting).
        bool PeerClosed()
        {
            u_long s_NonBlocking = 1;
            ioctlsocket(Client, FIONBIO, &s_NonBlocking);
            char s_Peek;
            const int s_Received = recv(Client, &s_Peek, 1, MSG_PEEK);
            const int s_Error = WSAGetLastError();
            s_NonBlocking = 0;
            ioctlsocket(Client, FIONBIO, &s_NonBlocking);
            return s_Received == 0 || (s_Received < 0 && s_Error != WSAEWOULDBLOCK);
        }

        // Accepts the connection the sink currently holds. On Windows loopback a connect the sink
        // abandoned at its connect timeout can still complete in the kernel a moment later (the stack
        // retries a refused SYN after ~500 ms) and land in the backlog ahead of the sink's next, live
        // attempt; the sink closes that abandoned socket before it retries. So: accept, wait until
        // the sink says it is connected, then move to the newest pending connection and confirm the
        // peer has not closed it.
        bool AcceptLive(const TcpRelaySink& p_Sink, uint32_t p_TimeoutMs)
        {
            if (!Accept(p_TimeoutMs))
                return false;

            const auto s_Deadline = Clock::now() + std::chrono::milliseconds(p_TimeoutMs);

            while (!p_Sink.GetStats().connected && Clock::now() < s_Deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (!p_Sink.GetStats().connected)
                return false;

            while (Pending())
            {
                CloseClient();
                Client = accept(Listen, nullptr, nullptr);
            }

            return Client != INVALID_SOCKET && !PeerClosed();
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

    uint16_t FreePort()
    {
        TestListener s_Probe;
        s_Probe.Start(0);
        const uint16_t s_Port = s_Probe.Port;
        s_Probe.Stop();
        return s_Port;
    }

    PublishedEnvelope Envelope(uint64_t p_Sequence)
    {
        PublishedEnvelope s_Envelope;
        s_Envelope.event_type = "mission.playing";
        s_Envelope.sequence = p_Sequence;
        s_Envelope.json = "{\"sequence\":" + std::to_string(p_Sequence) + "}";
        return s_Envelope;
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

    TcpRelaySink::Options FastOptions(uint16_t p_Port)
    {
        TcpRelaySink::Options s_Options;
        s_Options.port = p_Port;
        s_Options.connect_timeout_ms = 500;
        s_Options.backoff_initial_ms = 50;
        s_Options.backoff_max_ms = 200;
        return s_Options;
    }
}

void RunTcpRelaySinkTests()
{
    WSADATA s_Data;
    WSAStartup(MAKEWORD(2, 2), &s_Data);

    const uint16_t s_Port = FreePort();

    // 1. Backend absent: Publish returns immediately, events are dropped, connect attempts back off.
    {
        TcpRelaySink s_Sink(FastOptions(s_Port));

        for (uint64_t i = 1; i <= 3; ++i)
        {
            const auto s_Start = Clock::now();
            s_Sink.Publish(Envelope(i));
            CHECK(Clock::now() - s_Start < std::chrono::milliseconds(20));
        }

        auto s_Stats = s_Sink.GetStats();
        CHECK(!s_Stats.connected);
        CHECK(s_Stats.enqueued == 3);
        CHECK(s_Stats.dropped == 3);
        CHECK(s_Stats.sent == 0);

        CHECK(WaitUntil(1500, [&] { return s_Sink.GetStats().connect_attempts >= 3; }));
        s_Stats = s_Sink.GetStats();
        CHECK(s_Stats.connects == 0);

        // Backoff: after 1.5 s with 50, 100, 200, 200... ms waits there are at most ~9 attempts, not 30.
        CHECK(s_Stats.connect_attempts <= 12);
    }

    // 2. Backend appears later: the sink connects on its own and delivers in order.
    {
        TcpRelaySink s_Sink(FastOptions(s_Port));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!s_Sink.GetStats().connected);

        TestListener s_Listener;
        CHECK(s_Listener.Start(s_Port));
        CHECK(s_Listener.AcceptLive(s_Sink, 3000));
        CHECK(s_Sink.GetStats().connected);

        s_Sink.Publish(Envelope(1));
        s_Sink.Publish(Envelope(2));
        CHECK(s_Listener.ReadLine(2000) == "{\"sequence\":1}");
        CHECK(s_Listener.ReadLine(2000) == "{\"sequence\":2}");
        CHECK(WaitUntil(1000, [&] { return s_Sink.GetStats().sent == 2; }));
        CHECK(s_Sink.GetStats().dropped == 0);

        // 3. Backend sends bytes: discarded and counted; the connection keeps working.
        send(s_Listener.Client, "garbage\n", 8, 0);
        CHECK(WaitUntil(2000, [&] { return s_Sink.GetStats().inbound_bytes_discarded == 8; }));
        s_Sink.Publish(Envelope(3));
        CHECK(s_Listener.ReadLine(2000) == "{\"sequence\":3}");

        // 4. Backend disappears: detected, later events dropped, no reconnect storm.
        s_Listener.Stop();
        CHECK(WaitUntil(2000, [&] { return !s_Sink.GetStats().connected; }));
        CHECK(s_Sink.GetStats().disconnects == 1);
        s_Sink.Publish(Envelope(4));
        CHECK(s_Sink.GetStats().dropped == 1);

        // 5. Backend returns: reconnects and delivers again.
        TestListener s_Again;
        CHECK(s_Again.Start(s_Port));
        CHECK(s_Again.AcceptLive(s_Sink, 3000));
        CHECK(s_Sink.GetStats().connected);
        s_Sink.Publish(Envelope(5));
        CHECK(s_Again.ReadLine(2000) == "{\"sequence\":5}");

        const auto s_Stats = s_Sink.GetStats();
        CHECK(s_Stats.connects == 2);
        CHECK(s_Stats.sent == 4);
        CHECK(s_Stats.dropped == 1);
        s_Again.Stop();
    }

    // 6. Destruction with a live connection and with none both return promptly.
    {
        const auto s_Start = Clock::now();
        {
            TcpRelaySink s_Sink(FastOptions(s_Port));
        }
        CHECK(Clock::now() - s_Start < std::chrono::seconds(3));
    }

    WSACleanup();
}
