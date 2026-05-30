#pragma once
#include <windows.h>
#include <winrt/base.h>
#include <cstdio>
#include <cstdint>
#include <string>

// Throw winrt::hresult_error on failure -- gives a readable message + stack.
inline void check_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[FATAL] %s failed: 0x%08X\n", what, static_cast<unsigned>(hr));
        winrt::check_hresult(hr);
    }
}

#define LOGI(...) do { std::fprintf(stdout, "[INFO] " __VA_ARGS__); std::fprintf(stdout, "\n"); std::fflush(stdout); } while (0)
#define LOGW(...) do { std::fprintf(stderr, "[WARN] " __VA_ARGS__); std::fprintf(stderr, "\n"); std::fflush(stderr); } while (0)
#define LOGE(...) do { std::fprintf(stderr, "[ERR ] " __VA_ARGS__); std::fprintf(stderr, "\n"); std::fflush(stderr); } while (0)

// High-resolution timestamp in microseconds since process start.
inline int64_t now_us() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (c.QuadPart * 1'000'000) / freq.QuadPart;
}
