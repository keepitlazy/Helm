#pragma once
// Interim payload codec for the streaming skeleton (replaced by H.264 at ③).
//
// Reads a GPU BGRA capture texture back to the CPU and WIC-encodes it to a
// JPEG byte buffer. JPEG keeps the transport/framing path exercisable end-to-end
// today; the Electron client decodes it with createImageBitmap / WebCodecs.
//
// Cost note: ~5-8 ms for 1080p. We throttle the capture loop well below that, so
// encoding inline on the WGC worker thread is fine for the demo.
#include <d3d11.h>
#include <winrt/base.h>
#include <wincodec.h>
#include <objbase.h>
#include <vector>
#include <cstdint>
#include "common/Common.h"

inline bool EncodeTextureToJpeg(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                ID3D11Texture2D* src, float quality,
                                std::vector<uint8_t>& out) {
    // WGC delivers frames on a thread-pool worker; COM must be live on it for
    // CoCreateInstance. Init once per thread (process-lifetime, never released).
    thread_local bool com_ready = false;
    if (!com_ready) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ready = true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging.put()))) return false;
    ctx->CopyResource(staging.get(), src);

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map))) return false;

    const uint32_t W = desc.Width, H = desc.Height;

    // Pack BGRA -> tightly-packed 24bpp BGR (top-down): the JPEG encoder's native
    // input format, so WIC needs no extra IWICFormatConverter step.
    const uint32_t stride = W * 3;
    std::vector<uint8_t> bgr(static_cast<size_t>(stride) * H);
    const uint8_t* base = static_cast<const uint8_t*>(map.pData);
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* s = base + static_cast<size_t>(y) * map.RowPitch;
        uint8_t* d = bgr.data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < W; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    ctx->Unmap(staging.get(), 0);

    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put())))) return false;

    winrt::com_ptr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return false;

    winrt::com_ptr<IWICBitmapEncoder> enc;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put()))) return false;
    if (FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(frame.put(), props.put()))) return false;

    if (props) {  // JPEG quality, 0..1
        PROPBAG2 opt{};
        WCHAR name[] = L"ImageQuality";
        opt.pstrName = name;
        VARIANT v{};
        v.vt = VT_R4;
        v.fltVal = quality;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.get()))) return false;
    if (FAILED(frame->SetSize(W, H))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (fmt != GUID_WICPixelFormat24bppBGR) return false;  // encoder rejected our format

    if (FAILED(frame->WritePixels(H, stride, static_cast<UINT>(bgr.size()), bgr.data()))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(enc->Commit())) return false;

    // Pull the encoded bytes back out of the growable HGLOBAL stream.
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
    const size_t size = static_cast<size_t>(stat.cbSize.QuadPart);

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.get(), &hg))) return false;
    void* p = GlobalLock(hg);
    if (!p) return false;
    out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
    GlobalUnlock(hg);

    return !out.empty();
}
