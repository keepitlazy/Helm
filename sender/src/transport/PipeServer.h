#pragma once
// Single-client Windows named-pipe frame server.
//
// Same role and depth-1 "latest frame wins" design as TcpServer, but over a
// kernel named pipe (\\.\pipe\...) instead of TCP. Used by the Electron
// utilityProcess bridge, which connects as the pipe client. Wire framing is the
// shared proto::FrameHeader, so the client parses pipe and TCP identically.
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "transport/Protocol.h"

class PipeServer {
public:
    ~PipeServer();

    // Create the named pipe (e.g. L"\\\\.\\pipe\\stream_demo") and start serving.
    bool Start(const std::wstring& pipe_name);
    void Stop();

    bool HasClient() const { return has_client_.load(); }

    // Overwrite the pending frame (drop-old). `payload` is moved in.
    void PushFrame(const proto::FrameHeader& header, std::vector<uint8_t>&& payload);

private:
    void NetThread();
    bool WriteAll(void* handle, const void* data, int len);

    std::wstring pipe_name_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> has_client_{ false };
    void* pipe_ = nullptr;  // HANDLE; INVALID_HANDLE_VALUE when unset

    std::mutex mtx_;
    std::condition_variable cv_;
    bool have_frame_ = false;
    proto::FrameHeader pending_header_{};
    std::vector<uint8_t> pending_payload_;
};
