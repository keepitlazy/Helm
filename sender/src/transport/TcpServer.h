#pragma once
// Single-client TCP frame server (architecture diagram stage ④).
//
// Owns a background network thread that accepts one viewer at a time and streams
// framed payloads to it. The capture loop hands frames in via PushFrame, which is
// a depth-1 "latest frame wins" mailbox: if the network can't keep up, older
// undelivered frames are dropped rather than queued, keeping latency bounded.
#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "transport/Protocol.h"

class TcpServer {
public:
    ~TcpServer();

    // Bind + listen on the given TCP port and start the network thread.
    bool Start(uint16_t port);
    void Stop();

    bool HasClient() const { return has_client_.load(); }

    // Overwrite the pending frame (drop-old). `payload` is moved in.
    void PushFrame(const proto::FrameHeader& header, std::vector<uint8_t>&& payload);

private:
    void NetThread();
    bool SendAll(uintptr_t sock, const void* data, int len);

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> has_client_{ false };
    uintptr_t listen_sock_ = static_cast<uintptr_t>(~0ull);  // INVALID_SOCKET

    // Depth-1 frame mailbox.
    std::mutex mtx_;
    std::condition_variable cv_;
    bool have_frame_ = false;
    proto::FrameHeader pending_header_{};
    std::vector<uint8_t> pending_payload_;
};
