#pragma once
// Self-rolled wire framing (architecture diagram stage ④).
//
// Every message on the wire is: [24-byte FrameHeader][payload bytes].
// The header is fixed-size and little-endian (x86/x64 native), so the JS
// client can parse it with a DataView without any codec library.
//
// The payload codec is carried in the header (`codec`). Today the engine emits
// interim MJPEG (CODEC_JPEG); milestone ③ swaps the payload to H.264 NAL units
// (CODEC_H264) without touching this framing or the client's transport code.
#include <cstdint>

namespace proto {

// 'S' 'D' 'M' '1'  -- Stream Demo, framing v1.
inline constexpr unsigned char kMagic[4] = { 'S', 'D', 'M', '1' };

enum MsgType : uint8_t {
    MSG_VIDEO = 1,
};

enum Codec : uint8_t {
    CODEC_JPEG = 0,   // interim: each frame is a standalone JPEG (always a keyframe)
    CODEC_H264 = 1,   // milestone ③: Annex-B H.264, keyframe flagged below
};

enum Flags : uint8_t {
    FLAG_KEYFRAME = 1 << 0,
};

#pragma pack(push, 1)
struct FrameHeader {
    unsigned char magic[4];   // 0  : kMagic
    uint8_t  type;            // 4  : MsgType
    uint8_t  codec;           // 5  : Codec
    uint8_t  flags;           // 6  : Flags
    uint8_t  reserved;        // 7  : 0
    uint16_t width;           // 8  : frame width  (px)
    uint16_t height;          // 10 : frame height (px)
    int64_t  timestamp_us;    // 12 : capture timestamp (us since process start)
    uint32_t payload_size;    // 20 : payload byte count following this header
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 24, "FrameHeader must stay 24 bytes / wire-stable");

} // namespace proto
