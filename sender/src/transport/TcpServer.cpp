// Self-rolled framing over TCP (architecture diagram stage ④).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "transport/TcpServer.h"
#include "common/Common.h"

#pragma comment(lib, "ws2_32.lib")

TcpServer::~TcpServer() { Stop(); }

bool TcpServer::Start(uint16_t port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("WSAStartup failed");
        return false;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { LOGE("socket() failed"); WSACleanup(); return false; }

    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOGE("bind(:%u) failed: %d", port, WSAGetLastError());
        closesocket(ls); WSACleanup(); return false;
    }
    if (listen(ls, 1) == SOCKET_ERROR) {
        LOGE("listen() failed: %d", WSAGetLastError());
        closesocket(ls); WSACleanup(); return false;
    }

    listen_sock_ = static_cast<uintptr_t>(ls);
    running_.store(true);
    thread_ = std::thread(&TcpServer::NetThread, this);
    return true;
}

void TcpServer::Stop() {
    if (!running_.exchange(false)) return;

    // Unblock a pending accept() by closing the listening socket, and wake the
    // mailbox wait so the inner send loop can observe shutdown.
    if (listen_sock_ != static_cast<uintptr_t>(~0ull)) {
        closesocket(static_cast<SOCKET>(listen_sock_));
        listen_sock_ = static_cast<uintptr_t>(~0ull);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    WSACleanup();
}

void TcpServer::PushFrame(const proto::FrameHeader& header, std::vector<uint8_t>&& payload) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_header_ = header;
        pending_payload_ = std::move(payload);  // overwrite (drop-old)
        have_frame_ = true;
    }
    cv_.notify_one();
}

bool TcpServer::SendAll(uintptr_t sock, const void* data, int len) {
    const char* p = static_cast<const char*>(data);
    int left = len;
    while (left > 0) {
        int n = send(static_cast<SOCKET>(sock), p, left, 0);
        if (n == SOCKET_ERROR || n == 0) return false;  // client gone
        p += n;
        left -= n;
    }
    return true;
}

void TcpServer::NetThread() {
    while (running_.load()) {
        SOCKET client = accept(static_cast<SOCKET>(listen_sock_), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;     // Stop() closed the listener
            continue;
        }

        // Low latency: disable Nagle so small JPEG frames go out immediately.
        BOOL nodelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

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
            if (!SendAll(static_cast<uintptr_t>(client), &header, sizeof(header)) ||
                !SendAll(static_cast<uintptr_t>(client),
                         payload.data(), static_cast<int>(payload.size()))) {
                alive = false;  // disconnect -> fall back to accept()
            }
        }

        has_client_.store(false);
        closesocket(client);
        LOGI("CLIENT_DISCONNECTED");
    }
}
