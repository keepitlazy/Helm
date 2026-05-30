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
//   stream_sender.exe --snapshot-serve              resident warm capture: serve region
//                                                   screenshots on demand (no spawn latency)
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
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Capture a single (non-warm-up) frame and write it to `bmpPath`. When `mon`
// is non-null, that specific monitor is captured (per-display screenshot);
// otherwise the primary monitor is used.
static bool CaptureOneFrameToBmp(const char* bmpPath, HMONITOR mon) {
    WgcCapture capture;
    std::atomic<bool> started{ false };
    std::atomic<bool> done{ false };
    std::atomic<int>  frames{ 0 };

    capture.Start([&](ID3D11Texture2D* tex, int64_t /*ts*/) {
        int n = ++frames;
        if (n < 2 || started.exchange(true)) return; // skip first warm-up frame
        DumpTextureToBmp(capture.Device(), capture.Context(), tex, bmpPath);
        done.store(true); // set only after the write fully completes
    }, mon);

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

// ---- Resident warm capture engine (--snapshot-serve) -----------------------
// Keeps a live WGC capture running per monitor so a region-screenshot request
// can be served from the most recent frame with no spawn/warm-up latency. This
// is what makes the Electron overlay feel instantaneous: by the time the user
// triggers a screenshot, a fresh frame is already sitting in VRAM.
namespace {

// One always-on capture for a single monitor. The newest frame is copied into
// a private DEFAULT texture (`latest_`) under `gpu_`; Dump() snapshots that.
struct MonitorCap {
    WgcCapture cap;
    std::mutex gpu;                              // guards latest_ vs. Dump()
    winrt::com_ptr<ID3D11Texture2D> latest;      // lazily created on first frame
    std::atomic<bool> hasFrame{ false };

    void Start(HMONITOR mon) {
        cap.Start([this](ID3D11Texture2D* tex, int64_t /*ts*/) { OnFrame(tex); }, mon);
    }

    void OnFrame(ID3D11Texture2D* tex) {
        if (!tex) return;
        std::lock_guard<std::mutex> lk(gpu);
        if (!latest) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            // Private copy we fully own: no bind/cpu flags, plain DEFAULT.
            desc.BindFlags = 0;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(cap.Device()->CreateTexture2D(&desc, nullptr, latest.put())))
                return;
        }
        cap.Context()->CopyResource(latest.get(), tex);
        hasFrame.store(true);
    }

    // Wait briefly for a frame (WGC is change-driven, may lag), then dump the
    // latest to a BMP. Returns false if no frame ever arrived.
    bool Dump(const char* path) {
        for (int i = 0; i < 200 && !hasFrame.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lk(gpu);
        if (!hasFrame.load() || !latest) return false;
        DumpTextureToBmp(cap.Device(), cap.Context(), latest.get(), path);
        return true;
    }

    uint32_t Width() const { return cap.Width(); }
    uint32_t Height() const { return cap.Height(); }
};

} // namespace

static int RunSnapshotServe() {
    // Per-monitor captures live for the whole process: once created they are
    // never destroyed, so the WGC worker-thread callbacks never outlive their
    // MonitorCap (no use-after-free), and a second request for the same monitor
    // reuses the already-warm capture.
    std::map<HMONITOR, std::unique_ptr<MonitorCap>> caps;
    std::mutex caps_mutex;

    auto getCap = [&](HMONITOR mon) -> MonitorCap* {
        std::lock_guard<std::mutex> lk(caps_mutex);
        auto it = caps.find(mon);
        if (it != caps.end()) return it->second.get();
        auto mc = std::make_unique<MonitorCap>();
        mc->Start(mon);
        MonitorCap* raw = mc.get();
        caps.emplace(mon, std::move(mc));
        return raw;
    };

    // Warm the primary monitor immediately so the very first request is fast.
    HMONITOR primary = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    getCap(primary);

    std::printf("SNAPSHOT_READY\n");            // readiness line for the parent
    std::fflush(stdout);
    LOGI("snapshot engine ready (resident warm capture)");

    // Request protocol (tab-delimited so paths may contain spaces):
    //   snap\t<x>\t<y>\t<path>\n   x<0 => primary monitor
    char line[1024];
    while (std::fgets(line, sizeof(line), stdin)) {
        if (std::strncmp(line, "quit", 4) == 0) break;
        if (std::strncmp(line, "snap", 4) != 0) continue;

        // Parse the three tab-separated fields after "snap".
        char* save = nullptr;
        strtok_s(line, "\t", &save);                      // "snap"
        const char* sx = strtok_s(nullptr, "\t", &save);
        const char* sy = strtok_s(nullptr, "\t", &save);
        char* path = strtok_s(nullptr, "\t\r\n", &save);
        if (!sx || !sy || !path) {
            std::printf("SNAPSHOT_FAIL bad-request\n");
            std::fflush(stdout);
            continue;
        }

        int x = std::atoi(sx);
        int y = std::atoi(sy);
        HMONITOR mon = (x < 0) ? primary
                               : MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST);

        MonitorCap* mc = getCap(mon);
        if (mc->Dump(path)) {
            std::printf("SNAPSHOT_OK %s %ux%u\n", path, mc->Width(), mc->Height());
        } else {
            std::printf("SNAPSHOT_FAIL no-frame\n");
        }
        std::fflush(stdout);
    }

    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // Per-Monitor-V2 DPI awareness so monitor rects / MonitorFromPoint use the
    // same physical-pixel coordinate space the Electron side passes us. WGC
    // itself always captures at physical resolution regardless, but the monitor
    // *selection* below depends on this.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    check_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

    std::wstring mode = (argc > 1) ? argv[1] : L"--demo";

    if (mode == L"--screenshot") {
        // Convert the wide path arg to UTF-8 for fopen.
        std::wstring wpath = (argc > 2) ? argv[2] : L"capture_test.bmp";
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), len, nullptr, nullptr);

        // Optional "<x> <y>" physical-pixel point selects the monitor under it
        // (per-display capture). Absent -> primary monitor.
        HMONITOR mon = nullptr;
        if (argc > 4) {
            POINT pt{ _wtoi(argv[3]), _wtoi(argv[4]) };
            mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        }
        return CaptureOneFrameToBmp(path.c_str(), mon) ? 0 : 1;
    }

    if (mode == L"--serve") {
        uint16_t port = (argc > 2) ? static_cast<uint16_t>(_wtoi(argv[2])) : 8787;
        int fps = (argc > 3) ? _wtoi(argv[3]) : 15;
        float quality = (argc > 4) ? static_cast<float>(_wtof(argv[4])) : 0.7f;
        return RunServeTcp(port, fps, quality);
    }

    if (mode == L"--snapshot-serve") {
        return RunSnapshotServe();
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
