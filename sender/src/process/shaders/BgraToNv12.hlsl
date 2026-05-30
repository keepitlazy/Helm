// BGRA -> NV12 (YUV 4:2:0), BT.709 limited range, on the GPU.
//
// One thread handles one *chroma* sample = a 2x2 luma block. It writes four Y
// values (full res) and one averaged UV pair (half res). Dispatched over the
// chroma grid (W/2 x H/2). Keeping this on the GPU is the whole point of the
// architecture: the pixels never touch the CPU between capture and encode.

Texture2D<float4>      g_src  : register(t0); // BGRA, .rgb = logical RGB
RWTexture2D<float>     g_outY : register(u0); // NV12 luma plane  (R8_UNORM view)
RWTexture2D<float2>    g_outUV: register(u1); // NV12 chroma plane (R8G8_UNORM view)

static const float Kr = 0.2126;
static const float Kb = 0.0722;
static const float Kg = 1.0 - Kr - Kb;

float Luma(float3 c) { return Kr * c.r + Kg * c.g + Kb * c.b; }

// Full-range RGB -> limited-range Y' (normalised 0..1 so R8_UNORM stores it).
float EncodeY(float3 c)  { return (16.0 + 219.0 * Luma(c)) / 255.0; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    g_src.GetDimensions(w, h);

    uint2 luma = id.xy * 2;
    if (luma.x >= w || luma.y >= h) return;

    uint2 p00 = luma;
    uint2 p10 = uint2(min(luma.x + 1, w - 1), luma.y);
    uint2 p01 = uint2(luma.x, min(luma.y + 1, h - 1));
    uint2 p11 = uint2(min(luma.x + 1, w - 1), min(luma.y + 1, h - 1));

    float3 c00 = g_src[p00].rgb;
    float3 c10 = g_src[p10].rgb;
    float3 c01 = g_src[p01].rgb;
    float3 c11 = g_src[p11].rgb;

    g_outY[p00] = EncodeY(c00);
    g_outY[p10] = EncodeY(c10);
    g_outY[p01] = EncodeY(c01);
    g_outY[p11] = EncodeY(c11);

    // Average the 2x2 block for the shared chroma sample.
    float3 avg = (c00 + c10 + c01 + c11) * 0.25;
    float y  = Luma(avg);
    float cb = (avg.b - y) / (2.0 * (1.0 - Kb)); // -0.5..0.5
    float cr = (avg.r - y) / (2.0 * (1.0 - Kr));
    float cbq = (128.0 + 224.0 * cb) / 255.0;     // limited range, normalised
    float crq = (128.0 + 224.0 * cr) / 255.0;
    g_outUV[id.xy] = float2(cbq, crq);            // NV12 order: U(Cb) then V(Cr)
}
