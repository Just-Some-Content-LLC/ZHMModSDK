// M7-B2: the native motion link (MotionLink) against a stand-in BEAM listener on Windows loopback
// (glacier-relay docs/design/M7_NETWORK_GHOST.md sections 15.5 and 15.6): it connects only to an
// attested command link, binds with the domain-separated HMAC over BEAM's fresh welcome, admits only
// BEAM's kinds after the binding (any violation, flood or second welcome closes it), ends the binding
// with the command link it named, honours the frame thread's drop, bounds every queue, and drops the
// link when a write misses its send deadline against a socket nobody reads. Test tooling only; the
// run token is the tests' constant, never a real one.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "Attestation.h"
#include "CommandJson.h"
#include "MotionLink.h"
#include "TestHarness.h"
#include "TestListener.h"

using CommandTest::Listener;
using CommandTest::WaitUntil;

namespace
{
    constexpr const char* k_Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr const char* k_Adapter = "aaf540dd-c6df-4429-8fa5-84bc8933a13e";
    constexpr const char* k_Session = "6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f";

    uint64_t MonoMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    std::string Field(const std::string& p_Line, const char* p_Key)
    {
        const auto s_Parsed = CommandJson::Parse(p_Line);
        if (!s_Parsed.ok)
            return "<unparsed>";
        const auto* s_Value = s_Parsed.value.Find(p_Key);
        if (!s_Value)
            return "<absent>";
        if (s_Value->kind == CommandJson::Value::Kind::String)
            return s_Value->text;
        if (s_Value->kind == CommandJson::Value::Kind::Number)
            return std::to_string(s_Value->integer);
        return "<other>";
    }

    MotionLink::Options Fast(uint16_t p_Port)
    {
        MotionLink::Options s_Options;
        s_Options.port = p_Port;
        s_Options.connect_timeout_ms = 500;
        s_Options.welcome_timeout_ms = 800;
        s_Options.backoff_initial_ms = 50;
        s_Options.backoff_max_ms = 200;
        s_Options.token = k_Token;
        return s_Options;
    }

    std::string Welcome(uint64_t p_Generation, const std::string& p_Nonce)
    {
        return "{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":" + std::to_string(p_Generation) + ",\"nonce\":\"" + p_Nonce + "\"}\n";
    }

    std::string Target(uint64_t p_Seq)
    {
        return "{\"motion_protocol_version\":1,\"kind\":\"ghost.target\",\"session_id\":\"" + std::string(k_Session) + "\",\"seq\":" + std::to_string(p_Seq) + ",\"pos\":\"c2b2fae1,416d47ae\"}\n";
    }

    std::string Close()
    {
        return "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"" + std::string(k_Session) + "\",\"reason\":\"operator\"}\n";
    }

    // Accepts the link's live connection (skipping abandoned ones in the loopback backlog) and reads its hello.
    std::string AcceptHello(Listener& p_Beam)
    {
        for (int s_Attempt = 0; s_Attempt < 20; ++s_Attempt)
        {
            if (!p_Beam.Accept(3000))
                return "";
            const std::string s_Hello = p_Beam.ReadLine(1500);
            if (!s_Hello.empty())
                return s_Hello;
            p_Beam.CloseClient();
        }
        return "";
    }

    // The whole binding as BEAM performs it; returns the attest line ("" on failure).
    std::string Bind(Listener& p_Beam, MotionLink& p_Link, uint64_t p_MotionGeneration, const std::string& p_Nonce, uint64_t p_Serial)
    {
        if (AcceptHello(p_Beam).empty())
            return "";
        p_Beam.Send(Welcome(p_MotionGeneration, p_Nonce));
        const std::string s_Attest = p_Beam.ReadLine(2000);
        if (!WaitUntil(2000, [&] { const auto b = p_Link.Current(); return b.bound && b.serial == p_Serial; }))
            return "";
        return s_Attest;
    }

    void Binding()
    {
        Listener s_Beam;
        CHECK(s_Beam.Start(0));
        std::atomic<uint64_t> s_Command{0};
        MotionLink s_Link(Fast(s_Beam.Port), k_Adapter, MonoMs, [&] { return s_Command.load(); });

        // No attested command link: no connection at all.
        CHECK(!s_Beam.Accept(400) && s_Link.GetStats().connect_attempts == 0 && !s_Link.Current().bound);

        // The binding: hello naming the command link; one attest over the welcome's nonce.
        s_Command = 9;
        const std::string s_Hello = AcceptHello(s_Beam);
        CHECK(Field(s_Hello, "kind") == "motion.hello" && Field(s_Hello, "adapter_instance_id") == k_Adapter && Field(s_Hello, "command_link_generation") == "9" && Field(s_Hello, "motion_protocol_version") == "1");
        CHECK(s_Hello.find(k_Token) == std::string::npos);
        s_Beam.Send(Welcome(4, "514f1d221aefccd9d7fc4193040369cf"));
        const std::string s_Attest = s_Beam.ReadLine(2000);
        CHECK(Field(s_Attest, "kind") == "motion.attest" && Field(s_Attest, "motion_link_generation") == "4" && Field(s_Attest, "command_link_generation") == "9" && Field(s_Attest, "adapter_instance_id") == k_Adapter);
        CHECK(Field(s_Attest, "mac") == Attestation::MotionAttestMac(k_Token, "514f1d221aefccd9d7fc4193040369cf", 4, 9, k_Adapter));
        CHECK(Field(s_Attest, "mac") != Attestation::AttestMac(k_Token, "514f1d221aefccd9d7fc4193040369cf", 9, k_Adapter));
        CHECK(WaitUntil(2000, [&] { const auto b = s_Link.Current(); return b.bound && b.serial == 1 && b.motion_generation == 4 && b.command_generation == 9; }));

        // Messages after the binding: copied values with their receipt time and serial; a close (not a
        // target) advances the cancellation signal; a line split across writes is one message.
        const uint64_t s_Signals = s_Link.Signals();
        const uint64_t s_Before = MonoMs();
        s_Beam.Send(Target(1));
        const std::string s_Split = Target(2);
        s_Beam.Send(s_Split.substr(0, 40));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        s_Beam.Send(s_Split.substr(40));
        std::vector<IMotionLink::Inbound> s_In;
        CHECK(WaitUntil(2000, [&] { auto d = s_Link.Drain(); s_In.insert(s_In.end(), d.begin(), d.end()); return s_In.size() >= 2; }));
        CHECK(s_In.size() == 2 && s_In[0].message.seq == 1 && s_In[1].message.seq == 2 && s_In[0].serial == 1 && s_In[0].received_mono_ms >= s_Before && s_Link.Signals() == s_Signals);
        s_Beam.Send(Close());
        CHECK(WaitUntil(2000, [&] { return s_Link.Signals() == s_Signals + 1; }));
        CHECK(s_Link.Drain().size() == 1);

        // Lines the frame thread queues reach BEAM; a stale serial is not the bound one.
        CHECK(s_Link.SendControl(1, "{\"control\":1}\n") == IMotionLink::Queued::Queued);
        CHECK(s_Beam.ReadLine(2000) == "{\"control\":1}");
        CHECK(s_Link.SendSample(1, "{\"sample\":1}\n") == IMotionLink::Queued::Queued);
        CHECK(s_Beam.ReadLine(2000) == "{\"sample\":1}");
        CHECK(s_Link.SendControl(2, "x\n") == IMotionLink::Queued::NotBound && s_Link.SendSample(0, "x\n") == IMotionLink::Queued::NotBound);

        // The command link it named changes: the binding ends at once (signal), the link reconnects
        // and binds afresh to the new command link with a new serial; the old serial is dead.
        const uint64_t s_SignalsBefore = s_Link.Signals();
        s_Command = 10;
        CHECK(s_Beam.PeerClosed(2000));
        CHECK(WaitUntil(2000, [&] { return !s_Link.Current().bound && s_Link.Signals() > s_SignalsBefore && s_Link.GetStats().command_link_changes == 1; }));
        s_Beam.CloseClient();
        const std::string s_Hello2 = AcceptHello(s_Beam);
        CHECK(Field(s_Hello2, "command_link_generation") == "10");
        s_Beam.Send(Welcome(5, "614f1d221aefccd9d7fc4193040369cf"));
        const std::string s_Attest2 = s_Beam.ReadLine(2000);
        CHECK(Field(s_Attest2, "mac") == Attestation::MotionAttestMac(k_Token, "614f1d221aefccd9d7fc4193040369cf", 5, 10, k_Adapter) && Field(s_Attest2, "mac") != Field(s_Attest, "mac"));
        CHECK(WaitUntil(2000, [&] { const auto b = s_Link.Current(); return b.bound && b.serial == 2 && b.motion_generation == 5; }));
        CHECK(s_Link.SendControl(1, "x\n") == IMotionLink::Queued::NotBound);

        // The frame thread drops the binding (required evidence undeliverable); a stale drop is ignored.
        s_Link.Drop(1, "stale");
        CHECK(!s_Beam.PeerClosed(300) && s_Link.Current().bound);
        s_Link.Drop(2, "evidence undeliverable");
        CHECK(s_Beam.PeerClosed(2000));
        CHECK(WaitUntil(2000, [&] { return !s_Link.Current().bound && s_Link.GetStats().last_disconnect == "evidence undeliverable"; }));
        s_Beam.CloseClient();

        // A refusal during the binding: no binding; the next attempt waits the backoff.
        CHECK(!AcceptHello(s_Beam).empty());
        s_Beam.Send("{\"motion_protocol_version\":1,\"kind\":\"motion.refused\",\"reason\":\"command_link_not_token_verified\"}\n");
        CHECK(s_Beam.PeerClosed(2000));
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().refusals == 1 && s_Link.GetStats().last_refusal == "command_link_not_token_verified" && !s_Link.Current().bound; }));
        s_Beam.CloseClient();

        // No welcome within the binding deadline: the link gives up the connection on its own.
        CHECK(!AcceptHello(s_Beam).empty());
        CHECK(s_Beam.PeerClosed(3000));
        CHECK(!s_Link.Current().bound && s_Link.GetStats().attests_sent == 2);
        s_Beam.CloseClient();

        // An invalid welcome (generation 0) is a violation: no attest.
        CHECK(!AcceptHello(s_Beam).empty());
        s_Beam.Send("{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":0,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\"}\n");
        CHECK(s_Beam.PeerClosed(2000) && s_Link.GetStats().attests_sent == 2);
        s_Beam.CloseClient();

        // A target before the binding is a violation.
        CHECK(!AcceptHello(s_Beam).empty());
        s_Beam.Send(Target(1));
        CHECK(s_Beam.PeerClosed(2000) && s_Link.GetStats().attests_sent == 2);
        s_Beam.CloseClient();
        s_Beam.Stop();
    }

    void Violations()
    {
        struct Case
        {
            const char* label;
            std::string bytes;
        };
        const Case s_Cases[] = {
            {"oversized line", "{\"motion_protocol_version\":1,\"kind\":\"ghost.close\",\"session_id\":\"6b9c8f0e-3d1a-4c2b-9e7f-0a1b2c3d4e5f\",\"reason\":\"" + std::string(1000, 'r') + "\"}\n"},
            {"partial line beyond the bound", std::string(1100, ' ')},
            {"malformed json", "{\"motion_protocol_version\":1,\"kind\":\n"},
            {"unknown kind", "{\"motion_protocol_version\":1,\"kind\":\"ghost.teleport\"}\n"},
            {"a second welcome", Welcome(9, "714f1d221aefccd9d7fc4193040369cf")},
            {"a refusal after the binding", "{\"motion_protocol_version\":1,\"kind\":\"motion.refused\",\"reason\":\"late\"}\n"},
            {"a flood of targets", [] { std::string s; for (int i = 1; i <= 17; ++i) s += Target(i); return s; }()},
        };
        for (const auto& s_Case : s_Cases)
        {
            Listener s_Beam;
            CHECK(s_Beam.Start(0));
            MotionLink s_Link(Fast(s_Beam.Port), k_Adapter, MonoMs, [] { return uint64_t(9); });
            const bool s_Bound = !Bind(s_Beam, s_Link, 4, "514f1d221aefccd9d7fc4193040369cf", 1).empty();
            const uint64_t s_Signals = s_Link.Signals();
            s_Beam.Send(s_Case.bytes);
            const bool s_Closed = s_Beam.PeerClosed(3000);
            const bool s_Ok = s_Bound && s_Closed && WaitUntil(2000, [&] { return !s_Link.Current().bound && s_Link.Signals() > s_Signals && s_Link.GetStats().violations >= 1; });
            if (!s_Ok)
                std::printf("  motion link violation '%s': bound %d closed %d violations %llu\n", s_Case.label, s_Bound, s_Closed, static_cast<unsigned long long>(s_Link.GetStats().violations));
            CHECK(s_Ok);
            s_Beam.CloseClient();
            s_Beam.Stop();
        }
    }

    // A raw listener whose accepted sockets have a small receive buffer and which never reads.
    struct DeafBeam
    {
        SOCKET listen_socket = INVALID_SOCKET;
        SOCKET client = INVALID_SOCKET;
        uint16_t port = 0;

        bool Start()
        {
            listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            const int s_Small = 1024;
            setsockopt(listen_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&s_Small), sizeof(s_Small));
            sockaddr_in s_Address = {};
            s_Address.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &s_Address.sin_addr);
            if (bind(listen_socket, reinterpret_cast<sockaddr*>(&s_Address), sizeof(s_Address)) != 0 || listen(listen_socket, 4) != 0)
                return false;
            int s_Length = sizeof(s_Address);
            getsockname(listen_socket, reinterpret_cast<sockaddr*>(&s_Address), &s_Length);
            port = ntohs(s_Address.sin_port);
            return true;
        }

        bool Accept(uint32_t p_TimeoutMs)
        {
            fd_set s_Readable;
            FD_ZERO(&s_Readable);
            FD_SET(listen_socket, &s_Readable);
            timeval s_Timeout = {static_cast<long>(p_TimeoutMs / 1000), static_cast<long>((p_TimeoutMs % 1000) * 1000)};
            if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
                return false;
            client = accept(listen_socket, nullptr, nullptr);
            return client != INVALID_SOCKET;
        }

        std::string ReadLine(uint32_t p_TimeoutMs)
        {
            std::string s_Line;
            const auto s_Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);
            while (std::chrono::steady_clock::now() < s_Deadline)
            {
                fd_set s_Readable;
                FD_ZERO(&s_Readable);
                FD_SET(client, &s_Readable);
                timeval s_Timeout = {0, 20000};
                if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
                    continue;
                char c;
                if (recv(client, &c, 1, 0) != 1)
                    return "";
                if (c == '\n')
                    return s_Line;
                s_Line.push_back(c);
            }
            return "";
        }

        void Stop()
        {
            if (client != INVALID_SOCKET)
                closesocket(client);
            if (listen_socket != INVALID_SOCKET)
                closesocket(listen_socket);
        }
    };

    void SendDeadline()
    {
        DeafBeam s_Beam;
        CHECK(s_Beam.Start());
        auto s_Options = Fast(s_Beam.port);
        s_Options.send_buffer_bytes = 1024;
        s_Options.send_timeout_ms = 200;
        MotionLink s_Link(s_Options, k_Adapter, MonoMs, [] { return uint64_t(9); });

        bool s_Bound = false;
        for (int s_Attempt = 0; s_Attempt < 10 && !s_Bound; ++s_Attempt)
        {
            if (!s_Beam.Accept(3000))
                break;
            if (s_Beam.ReadLine(1500).empty())
            {
                closesocket(s_Beam.client);
                continue;
            }
            const std::string s_Welcome = Welcome(4, "514f1d221aefccd9d7fc4193040369cf");
            send(s_Beam.client, s_Welcome.data(), static_cast<int>(s_Welcome.size()), 0);
            s_Beam.ReadLine(2000); // the attest; nothing is read after it
            s_Bound = WaitUntil(2000, [&] { return s_Link.Current().bound; });
        }
        CHECK(s_Bound);

        // BEAM stops reading: the queues fill (bounded: Full), a write blocks past its deadline, and the
        // link drops itself within a bounded time; the frame thread then sees NotBound and the signal.
        const uint64_t s_Signals = s_Link.Signals();
        const std::string s_Line = "{\"sample\":\"" + std::string(400, 's') + "\"}\n";
        bool s_SawFull = false;
        const uint64_t s_Start = MonoMs();
        while (MonoMs() - s_Start < 20000 && s_Link.Current().bound)
        {
            // bursts larger than both queues (the network thread writes a few lines, then blocks)
            for (int i = 0; i < 2 * static_cast<int>(MotionLink::k_ControlCapacity); ++i)
            {
                const auto s_Sample = s_Link.SendSample(1, s_Line);
                const auto s_Control = s_Link.SendControl(1, s_Line);
                s_SawFull = s_SawFull || s_Sample == IMotionLink::Queued::Full || s_Control == IMotionLink::Queued::Full;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const uint64_t s_Elapsed = MonoMs() - s_Start;
        const auto s_Stats = s_Link.GetStats();
        std::printf("motion link send deadline: dropped after %llu ms; %llu line(s) sent, samples full %llu, control full %llu, send failures %llu, discarded %llu, last disconnect '%s'\n",
            static_cast<unsigned long long>(s_Elapsed), static_cast<unsigned long long>(s_Stats.lines_sent), static_cast<unsigned long long>(s_Stats.samples_full), static_cast<unsigned long long>(s_Stats.control_full),
            static_cast<unsigned long long>(s_Stats.send_failures), static_cast<unsigned long long>(s_Stats.discarded_on_disconnect), s_Stats.last_disconnect.c_str());
        CHECK(!s_Link.Current().bound && s_Stats.send_failures == 1 && s_Stats.last_disconnect == "send deadline passed" && s_Link.Signals() > s_Signals);
        CHECK(s_SawFull && s_Stats.samples_full > 0 && s_Stats.control_full > 0);
        CHECK(s_Link.SendControl(1, s_Line) == IMotionLink::Queued::NotBound);
        s_Beam.Stop();
    }
}

void RunMotionLinkTests()
{
    WSADATA s_Data;
    WSAStartup(MAKEWORD(2, 2), &s_Data);
    Binding();
    Violations();
    SendDeadline();
    WSACleanup();
}
