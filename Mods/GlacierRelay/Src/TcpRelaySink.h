#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "IRelaySink.h"

// Stage 2 sink: an outbound TCP client that writes one envelope per line (NDJSON) to the BEAM
// listener on loopback. Native never listens.
//
// Delivery is ordered and best-effort while a connection is live. Publish() only appends to a
// bounded queue and returns; a sender thread owns the socket, connects with a timeout, retries
// with exponential backoff, and drops envelopes while disconnected (logging each drop). Nothing
// is ever read as protocol: bytes the peer sends are discarded and counted. A missing backend
// therefore costs the game nothing but log lines.
class TcpRelaySink : public IRelaySink
{
public:
    struct Options
    {
        std::string host = "127.0.0.1"; // loopback only; not configurable to anything else in M1
        uint16_t port = 4747;
        size_t max_queued = 256;
        uint32_t connect_timeout_ms = 2000;
        uint32_t send_timeout_ms = 2000;
        uint32_t backoff_initial_ms = 1000;
        uint32_t backoff_max_ms = 30000;
    };

    struct Stats
    {
        bool connected = false;
        uint64_t enqueued = 0;
        uint64_t sent = 0;
        uint64_t dropped = 0;       // not delivered: disconnected, queue full, or send failed
        uint64_t connect_attempts = 0;
        uint64_t connects = 0;
        uint64_t disconnects = 0;
        uint64_t inbound_bytes_discarded = 0;
    };

    explicit TcpRelaySink(Options p_Options);
    ~TcpRelaySink() override;

    TcpRelaySink(const TcpRelaySink&) = delete;
    TcpRelaySink& operator=(const TcpRelaySink&) = delete;

    // Frame thread. Appends to the queue (dropping the oldest entry if full) and returns.
    void Publish(const PublishedEnvelope& p_Envelope) override;

    Stats GetStats() const;

private:
    void SenderLoop();
    bool TryConnect();
    void Disconnect(const char* p_Reason);
    bool SendLine(const std::string& p_Line);
    void DiscardInbound();
    bool WaitForWork(uint32_t p_TimeoutMs);

private:
    Options m_Options;

    mutable std::mutex m_Mutex;
    std::condition_variable m_Wake;
    std::deque<std::string> m_Queue; // lines, each already ending in '\n'
    bool m_Stop = false;
    Stats m_Stats;

    std::thread m_Thread;
    uintptr_t m_Socket = ~uintptr_t(0); // INVALID_SOCKET; stored untyped so the header needs no Winsock
    bool m_WinsockStarted = false;
};
