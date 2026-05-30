#pragma once
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <functional>
#include <mutex>

// Windows Graphics Capture (WGC) of a single monitor into D3D11 BGRA textures.
//
// Design notes (architecture diagram stage ①):
//  * The whole pipeline lives on the GPU. WGC hands us an ID3D11Texture2D that
//    already sits in VRAM -- no CPU copy. Downstream stages (NV12 convert,
//    encode) consume that texture directly.
//  * We use a free-threaded frame pool so frames are delivered on a worker
//    thread without needing a DispatcherQueue / message pump.
class WgcCapture {
public:
    // Called on a WGC worker thread for every captured frame. `tex` is owned by
    // the frame pool and only valid for the duration of the callback -- copy or
    // process it synchronously.
    using FrameCallback = std::function<void(ID3D11Texture2D* tex, int64_t timestamp_us)>;

    WgcCapture() = default;
    ~WgcCapture();

    // Initialise D3D11 + start capturing `mon`. If `mon` is null, the primary
    // monitor is captured (back-compat with the --serve / --demo modes).
    void Start(const FrameCallback& cb, HMONITOR mon = nullptr);
    void Stop();

    ID3D11Device* Device() const { return device_.get(); }
    ID3D11DeviceContext* Context() const { return context_.get(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    void OnFrameArrived(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool,
        const winrt::Windows::Foundation::IInspectable&);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rt_device_{ nullptr };

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_revoker_;

    FrameCallback callback_;
    uint32_t width_ = 0, height_ = 0;
    std::mutex mutex_;
};
