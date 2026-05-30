#include "capture/WgcCapture.h"
#include "common/Common.h"

#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>      // IGraphicsCaptureItemInterop
#include <windows.graphics.directx.direct3d11.interop.h> // CreateDirect3D11DeviceFromDXGIDevice

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

// Pull the native ID3D11Texture2D out of a WinRT surface.
com_ptr<ID3D11Texture2D> GetTexture(const IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex;
    check_hr(access->GetInterface(guid_of<ID3D11Texture2D>(), tex.put_void()), "GetInterface(texture)");
    return tex;
}

} // namespace

WgcCapture::~WgcCapture() { Stop(); }

void WgcCapture::Start(const FrameCallback& cb) {
    callback_ = cb;

    // --- Pick the adapter that actually owns the primary monitor's output.
    // This box has multiple adapters (Intel + an Oray virtual display). WGC
    // capture must run on the SAME adapter that drives the monitor, otherwise
    // we get all-black cross-adapter frames.
    HMONITOR primaryMon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    com_ptr<IDXGIFactory1> factory;
    check_hr(CreateDXGIFactory1(guid_of<IDXGIFactory1>(), factory.put_void()), "CreateDXGIFactory1");

    com_ptr<IDXGIAdapter1> chosen;
    for (UINT ai = 0; ; ++ai) {
        com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
        for (UINT oi = 0; ; ++oi) {
            com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, output.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{}; output->GetDesc(&od);
            LOGI("adapter[%u] '%ls' output '%ls' monitor=%p%s",
                 ai, ad.Description, od.DeviceName, (void*)od.Monitor,
                 od.Monitor == primaryMon ? "  <-- primary" : "");
            if (od.Monitor == primaryMon) chosen = adapter;
        }
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got{};
    // If we found the owning adapter use it explicitly; else fall back to default.
    D3D_DRIVER_TYPE driverType = chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    check_hr(D3D11CreateDevice(chosen.get(), driverType, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION,
                               device_.put(), &got, context_.put()),
             "D3D11CreateDevice");

    // Wrap the D3D11 device as a WinRT IDirect3DDevice for WGC.
    auto dxgi = device_.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    check_hr(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()),
             "CreateDirect3D11DeviceFromDXGIDevice");
    rt_device_ = inspectable.as<IDirect3DDevice>();

    // --- Build a GraphicsCaptureItem for the primary monitor.
    HMONITOR mon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    check_hr(interop->CreateForMonitor(mon, guid_of<GraphicsCaptureItem>(),
                                       put_abi(item_)), "CreateForMonitor");

    auto size = item_.Size();
    width_ = static_cast<uint32_t>(size.Width);
    height_ = static_cast<uint32_t>(size.Height);
    LOGI("WGC capture item: %ux%u", width_, height_);

    // --- Free-threaded frame pool: FrameArrived fires on a worker thread.
    pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
        rt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    frame_revoker_ = pool_.FrameArrived(auto_revoke,
        { this, &WgcCapture::OnFrameArrived });

    session_ = pool_.CreateCaptureSession(item_);
    // Hide the yellow capture border if the OS build supports it.
    try { session_.IsBorderRequired(false); } catch (...) {}
    try { session_.IsCursorCaptureEnabled(true); } catch (...) {}
    session_.StartCapture();
    LOGI("WGC capture started");
}

void WgcCapture::OnFrameArrived(const Direct3D11CaptureFramePool& pool,
                                const winrt::Windows::Foundation::IInspectable&) {
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;

    auto surface = frame.Surface();
    auto tex = GetTexture(surface);

    FrameCallback cb;
    { std::lock_guard<std::mutex> lk(mutex_); cb = callback_; }
    if (cb) cb(tex.get(), now_us());
}

void WgcCapture::Stop() {
    { std::lock_guard<std::mutex> lk(mutex_); callback_ = nullptr; }
    frame_revoker_.revoke();
    if (session_) { session_.Close(); session_ = nullptr; }
    if (pool_) { pool_.Close(); pool_ = nullptr; }
    item_ = nullptr;
}
