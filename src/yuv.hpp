#pragma once

#include <algorithm>
#include <cstdint>

namespace autoframing {

enum class PackedYuvFormat {
    Yuy2,
    Uyvy,
    Yvyu,
};

struct PackedYuvGroup {
    uint8_t y0 = 0;
    uint8_t u = 128;
    uint8_t v = 128;
    uint8_t y1 = 0;
    bool has_second_pixel = false;
};

inline uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

inline void yuv_to_rgba(uint8_t y, uint8_t u, uint8_t v, uint8_t* rgba)
{
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    rgba[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
    rgba[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgba[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
    rgba[3] = 255;
}

inline PackedYuvGroup decode_packed_yuv_group(const uint8_t* src, PackedYuvFormat format, bool has_second_pixel)
{
    PackedYuvGroup group;
    group.has_second_pixel = has_second_pixel;

    // Packed YUV formats are normally four bytes for two pixels. Odd source widths can leave a
    // trailing one-pixel group, so bytes for the second pixel/chroma pair must only be read when present.
    switch (format) {
    case PackedYuvFormat::Yuy2:
        group.y0 = src[0];
        group.u = src[1];
        if (has_second_pixel) {
            group.y1 = src[2];
            group.v = src[3];
        }
        break;
    case PackedYuvFormat::Uyvy:
        group.u = src[0];
        group.y0 = src[1];
        if (has_second_pixel) {
            group.v = src[2];
            group.y1 = src[3];
        }
        break;
    case PackedYuvFormat::Yvyu:
        group.y0 = src[0];
        group.v = src[1];
        if (has_second_pixel) {
            group.y1 = src[2];
            group.u = src[3];
        }
        break;
    }

    return group;
}

} // namespace autoframing
