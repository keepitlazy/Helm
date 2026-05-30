#pragma once
// Debug helper: copy a GPU BGRA texture to the CPU and write it as a BMP.
// Used only to eyeball that stage-① capture works. Not on the hot path.
// (Plain BMP avoids any WIC encoder quirks -- this is throwaway debug code.)
#include <d3d11.h>
#include <winrt/base.h>
#include <vector>
#include <cstdio>
#include "common/Common.h"

inline void DumpTextureToBmp(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                             ID3D11Texture2D* src, const char* path) {
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    check_hr(dev->CreateTexture2D(&stagingDesc, nullptr, staging.put()), "CreateTexture2D(staging)");
    ctx->CopyResource(staging.get(), src);

    D3D11_MAPPED_SUBRESOURCE map{};
    check_hr(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map), "Map(staging)");

    const uint32_t W = desc.Width, H = desc.Height;

    // Sanity: how much of the frame is non-zero? (detect black/empty capture)
    {
        size_t nonzero = 0, sampled = 0;
        const uint8_t* p = static_cast<const uint8_t*>(map.pData);
        for (uint32_t y = 0; y < H; y += 16)
            for (uint32_t x = 0; x < W; x += 16) {
                if (p[y * map.RowPitch + x * 4] | p[y * map.RowPitch + x * 4 + 1] |
                    p[y * map.RowPitch + x * 4 + 2]) ++nonzero;
                ++sampled;
            }
        LOGI("frame non-zero sampled pixels: %zu / %zu", nonzero, sampled);
    }

    // 24-bit BMP (BGR, bottom-up).
    const uint32_t rowSize = ((W * 3 + 3) / 4) * 4;
    const uint32_t imgSize = rowSize * H;
#pragma pack(push, 1)
    struct { uint16_t bfType; uint32_t bfSize; uint16_t r1, r2; uint32_t bfOffBits;
             uint32_t biSize; int32_t biW, biH; uint16_t biPlanes, biBpp;
             uint32_t biComp, biImg; int32_t biXppm, biYppm; uint32_t biClr, biImp; } hdr{};
#pragma pack(pop)
    hdr.bfType = 0x4D42;
    hdr.bfOffBits = sizeof(hdr);
    hdr.bfSize = sizeof(hdr) + imgSize;
    hdr.biSize = 40; hdr.biW = (int32_t)W; hdr.biH = (int32_t)H;
    hdr.biPlanes = 1; hdr.biBpp = 24; hdr.biImg = imgSize;

    std::vector<uint8_t> row(rowSize, 0);
    FILE* f = nullptr; fopen_s(&f, path, "wb");
    if (!f) { ctx->Unmap(staging.get(), 0); LOGE("cannot open %s", path); return; }
    fwrite(&hdr, sizeof(hdr), 1, f);
    const uint8_t* base = static_cast<const uint8_t*>(map.pData);
    for (int y = (int)H - 1; y >= 0; --y) {           // bottom-up
        const uint8_t* srcRow = base + (size_t)y * map.RowPitch;
        for (uint32_t x = 0; x < W; ++x) {            // BGRA -> BGR
            row[x * 3 + 0] = srcRow[x * 4 + 0];
            row[x * 3 + 1] = srcRow[x * 4 + 1];
            row[x * 3 + 2] = srcRow[x * 4 + 2];
        }
        fwrite(row.data(), rowSize, 1, f);
    }
    fclose(f);
    ctx->Unmap(staging.get(), 0);
    LOGI("dumped frame to %s (%ux%u)", path, W, H);
}
