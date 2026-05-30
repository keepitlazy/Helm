// Named-pipe framing transport (orthodox local IPC for the Electron bridge).
#include <windows.h>
#include "transport/PipeServer.h"
#include "common/Common.h"

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(const std::wstring& pipe_name) {
    pipe_name_ = pipe_name;

    // Byte-stream, blocking, single instance. 1 MiB buffers comfortably hold a
    // 1080p JPEG so WriteFile rarely blocks on a live reader.
    HANDLE h = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                                  // max instances
        1u << 20, 1u << 20,                 // out / in buffer bytes
        0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOGE("CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }
    pipe_ = h;
    running_.store(true);
    thread_ = std::thread(&PipeServer::NetThread, this);
    return true;
}

void PipeServer::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();

    // Unblock a thread parked in ConnectNamedPipe by briefly self-connecting.
    HANDLE c = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);

    if (thread_.joinable()) thread_.join();
    if (pipe_) { CloseHandle(static_cast<HANDLE>(pipe_)); pipe_ = nullptr; }
}

void PipeServer::PushFrame(const proto::FrameHeader& header, std::vector<uint8_t>&& payload) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_header_ = header;
        pending_payload_ = std::move(payload);  // overwrite (drop-old)
        have_frame_ = true;
    }
    cv_.notify_one();
}

bool PipeServer::WriteAll(void* handle, const void* data, int len) {
    const char* p = static_cast<const char*>(data);
    int left = len;
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile(static_cast<HANDLE>(handle), p, static_cast<DWORD>(left), &wrote, nullptr) ||
            wrote == 0) {
            return false;  // reader gone
        }
        p += wrote;
        left -= static_cast<int>(wrote);
    }
    return true;
}

void PipeServer::NetThread() {
    HANDLE h = static_cast<HANDLE>(pipe_);

    while (running_.load()) {
        // Wait for a client. ERROR_PIPE_CONNECTED = client beat us to it.
        BOOL ok = ConnectNamedPipe(h, nullptr);
        if (!running_.load()) break;
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            DisconnectNamedPipe(h);
            continue;
        }

        has_client_.store(true);
        LOGI("CLIENT_CONNECTED");

        bool alive = true;
        while (running_.load() && alive) {
            proto::FrameHeader header{};
            std::vector<uint8_t> payload;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [&] { return have_frame_ || !running_.load(); });
                if (!running_.load()) break;
                header = pending_header_;
                payload = std::move(pending_payload_);
                have_frame_ = false;
            }
            if (!WriteAll(h, &header, sizeof(header)) ||
                !WriteAll(h, payload.data(), static_cast<int>(payload.size()))) {
                alive = false;  // disconnect -> wait for the next client
            }
        }

        has_client_.store(false);
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
        LOGI("CLIENT_DISCONNECTED");
    }
}
