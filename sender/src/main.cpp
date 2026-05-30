// stream_sender -- low-latency screen streaming sender.
//
// Pipeline:
//   ① WGC capture -> ② D3D11 BGRA->NV12 -> ③ HW encode (QSV/MF | NVENC)
//   -> ④ self-rolled framing over TCP -> client (Electron + WebCodecs)
//
// Modes:
//   stream_sender.exe --screenshot <path.bmp>       capture one frame, write BMP, exit
//   stream_sender.exe --demo [seconds]              continuous capture FPS test
//   stream_sender.exe --serve [port] [fps] [q]      resident engine: framed JPEG over TCP
//   stream_sender.exe --serve-pipe [name] [fps] [q] resident engine: framed JPEG over a
//                                                   named pipe (Electron utilityProcess bridge)
//
// Both serve modes run the same capture engine continuously and share the exact
// wire framing (proto::FrameHeader); milestone ③ swaps the JPEG payload for H.264
// without touching the transport. They stop on stdin EOF / "quit".

#include "common/Common.h"
#include "common/WicPng.h"
#include "capture/WgcCapture.h"
#include "encode/JpegEncoder.h"
#include "transport/TcpServer.h"
#include "transport/PipeServer.h"
#include "transport/Protocol.h"

#include <winrt/Windows.Foundation.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <string>

// Capture a single (non-warm-up) frame and write it to `bmpPath`.
static bool CaptureOneFrameToBmp(const char* bmpPath) {
    WgcCapture capture;
    std::atomic<bool> started{ false };
    std::atomic<bool> done{ false };
    std::atomic<int>  frames{ 0 };

    capture.Start([&](ID3D11Texture2D* tex, int64_t /*ts*/) {
        int n = ++frames;
        if (n < 2 || started.exchange(true)) return; // skip first warm-up frame
        DumpTextureToBmp(capture.Device(), capture.Context(), tex, bmpPath);
        done.store(true); // set only after the write fully completes
    });

    // WGC is change-driven; wait up to 6s for a usable frame.
    for (int i = 0; i < 600 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    uint32_t w = capture.Width(), h = capture.Height();
    capture.Stop();

    if (done.load()) {
        // Machine-parseable success line for the Electron parent process.
        std::printf("SCREENSHOT_OK %s %ux%u\n", bmpPath, w, h);
        std::fflush(stdout);
        return true;
    }
    std::printf("SCREENSHOT_FAIL no-frame\n");
    std::fflush(stdout);
    return false;
}

static int RunCaptureDemo(int run_seconds) {
    WgcCapture capture;
    std::atomic<int> total{ 0 }, window{ 0 };
    capture.Start([&](ID3D11Texture2D*, int64_t) { ++total; ++window; });
    LOGI("capturing for %d s ... (WGC is change-driven)", run_seconds);
    for (int s = 0; s < run_seconds; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOGI("t=%ds fps=%d total=%d %ux%u", s + 1, window.exchange(0),
             total.load(), capture.Width(), capture.Height());
    }
    capture.Stop();
    return total.load() > 0 ? 0 : 1;
}

// Drive the capture engine into any transport exposing HasClient()/PushFrame()/
// Stop(). Frames are throttled to `fps` and only encoded while a viewer is
// connected. Blocks on stdin until the parent closes it or sends "quit".
template <class Server>
static void RunServeLoop(Server& server, int fps, float quality) {
    const int64_t min_interval_us = fps > 0 ? (1'000'000 / fps) : 0;
    std::atomic<int64_t> last_sent_us{ 0 };  // 0 = nothing sent yet (ts is always > 0)

    WgcCapture capture;
    capture.Start([&](ID3D11Texture2D* tex, int64_t ts) {
        if (!server.HasClient()) return;                       // nobody watching
        const int64_t last = last_sent_us.load();
        if (last != 0 && ts - last < min_interval_us) return;  // throttle to target fps
        last_sent_us.store(ts);

        std::vector<uint8_t> jpeg;
        if (!EncodeTextureToJpeg(capture.Device(), capture.Context(), tex, quality, jpeg))
            return;

        proto::FrameHeader h{};
        std::memcpy(h.magic, proto::kMagic, sizeof(h.magic));
        h.type = proto::MSG_VIDEO;
        h.codec = proto::CODEC_JPEG;
        h.flags = proto::FLAG_KEYFRAME;          // every JPEG is a full frame
        h.width = static_cast<uint16_t>(capture.Width());
        h.height = static_cast<uint16_t>(capture.Height());
        h.timestamp_us = ts;
        h.payload_size = static_cast<uint32_t>(jpeg.size());
        server.PushFrame(h, std::move(jpeg));
    });

    // Control channel: fgets returns null when the parent closes stdin (process
    // teardown); "quit" stops on request.
    char line[256];
    while (std::fgets(line, sizeof(line), stdin)) {
        if (std::strncmp(line, "quit", 4) == 0) break;
    }

    capture.Stop();
    server.Stop();
}

static int RunServeTcp(uint16_t port, int fps, float quality) {
    TcpServer server;
    if (!server.Start(port)) return 1;
    std::printf("SERVE_LISTENING %u\n", port);  // machine-parseable readiness line
    std::fflush(stdout);
    LOGI("serving JPEG over tcp/%u  fps<=%d  q=%.2f", port, fps, quality);
    RunServeLoop(server, fps, quality);
    LOGI("serve stopped");
    return 0;
}

static int RunServePipe(const std::wstring& name, int fps, float quality) {
    PipeServer server;
    if (!server.Start(name)) return 1;
    std::printf("SERVE_PIPE_READY\n");          // readiness line for the bridge
    std::fflush(stdout);
    LOGI("serving JPEG over named pipe  fps<=%d  q=%.2f", fps, quality);
    RunServeLoop(server, fps, quality);
    LOGI("serve stopped");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    check_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

    std::wstring mode = (argc > 1) ? argv[1] : L"--demo";

    if (mode == L"--screenshot") {
        // Convert the wide path arg to UTF-8 for fopen.
        std::wstring wpath = (argc > 2) ? argv[2] : L"capture_test.bmp";
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), len, nullptr, nullptr);
        return CaptureOneFrameToBmp(path.c_str()) ? 0 : 1;
    }

    if (mode == L"--serve") {
        uint16_t port = (argc > 2) ? static_cast<uint16_t>(_wtoi(argv[2])) : 8787;
        int fps = (argc > 3) ? _wtoi(argv[3]) : 15;
        float quality = (argc > 4) ? static_cast<float>(_wtof(argv[4])) : 0.7f;
        return RunServeTcp(port, fps, quality);
    }

    if (mode == L"--serve-pipe") {
        std::wstring name = (argc > 2) ? argv[2] : L"\\\\.\\pipe\\stream_demo";
        int fps = (argc > 3) ? _wtoi(argv[3]) : 15;
        float quality = (argc > 4) ? static_cast<float>(_wtof(argv[4])) : 0.7f;
        return RunServePipe(name, fps, quality);
    }

    int seconds = (argc > 2) ? _wtoi(argv[2]) : 5;
    return RunCaptureDemo(seconds);
}
