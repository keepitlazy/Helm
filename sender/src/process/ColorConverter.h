#pragma once
#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>

// Stage ②: BGRA -> NV12 colour-space conversion on the GPU via a compute shader.
//
// Output is a single DXGI_FORMAT_NV12 texture (luma + interleaved chroma planes)
// kept in VRAM, ready to hand straight to the hardware encoder. Reused across
// frames -- the caller must consume / encode the result before the next ToNv12.
class ColorConverter {
public:
    void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);

    // Convert a BGRA texture to NV12. Returns the internal NV12 texture (valid
    // until the next call). `srcBgra` only needs to live for this call.
    ID3D11Texture2D* ToNv12(ID3D11Texture2D* srcBgra);

    uint32_t Width() const { return w_; }
    uint32_t Height() const { return h_; }

private:
    void EnsureResources(uint32_t w, uint32_t h);
    ID3D11ShaderResourceView* SrvForSource(ID3D11Texture2D* src);

    winrt::com_ptr<ID3D11Device> dev_;
    winrt::com_ptr<ID3D11DeviceContext> ctx_;
    winrt::com_ptr<ID3D11ComputeShader> cs_;

    winrt::com_ptr<ID3D11Texture2D> nv12_;
    winrt::com_ptr<ID3D11UnorderedAccessView> uavY_, uavUV_;

    // Fallback when the capture texture can't be bound as a shader resource.
    winrt::com_ptr<ID3D11Texture2D> srcCopy_;
    winrt::com_ptr<ID3D11ShaderResourceView> srcCopySrv_;

    uint32_t w_ = 0, h_ = 0;
};
