#pragma once

#include "geometry.hpp"

#include <algorithm>

namespace autoframing {

struct LetterboxInfo {
    int input_width = 0;
    int input_height = 0;
    int resized_width = 0;
    int resized_height = 0;
    float scale = 1.0f;
    float pad_x = 0.0f;
    float pad_y = 0.0f;
};

inline Rect map_letterboxed_model_box_to_source(Rect box, const LetterboxInfo& info, const Size& source_size)
{
    if (!source_size.valid() || info.scale <= 0.0f) {
        return {};
    }

    box.x = (box.x - info.pad_x) / info.scale;
    box.y = (box.y - info.pad_y) / info.scale;
    box.width /= info.scale;
    box.height /= info.scale;

    const float left = std::clamp(box.left(), 0.0f, source_size.width);
    const float top = std::clamp(box.top(), 0.0f, source_size.height);
    const float right = std::clamp(box.right(), 0.0f, source_size.width);
    const float bottom = std::clamp(box.bottom(), 0.0f, source_size.height);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

} // namespace autoframing
