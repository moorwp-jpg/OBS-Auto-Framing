#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace autoframing {

struct Size {
    float width = 0.0f;
    float height = 0.0f;

    bool valid() const { return width > 0.0f && height > 0.0f; }
    float aspect() const { return valid() ? width / height : 1.0f; }
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool valid() const { return width > 0.0f && height > 0.0f; }
    float left() const { return x; }
    float top() const { return y; }
    float right() const { return x + width; }
    float bottom() const { return y + height; }
    float center_x() const { return x + width * 0.5f; }
    float center_y() const { return y + height * 0.5f; }
    float area() const { return valid() ? width * height : 0.0f; }
};

inline Rect make_centered_rect(float center_x, float center_y, float width, float height)
{
    return {center_x - width * 0.5f, center_y - height * 0.5f, width, height};
}

inline float intersection_over_union(const Rect& a, const Rect& b)
{
    const float left = std::max(a.left(), b.left());
    const float top = std::max(a.top(), b.top());
    const float right = std::min(a.right(), b.right());
    const float bottom = std::min(a.bottom(), b.bottom());
    const float width = std::max(0.0f, right - left);
    const float height = std::max(0.0f, bottom - top);
    const float intersection = width * height;
    const float union_area = a.area() + b.area() - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

inline Rect union_rects(const std::vector<Rect>& rects)
{
    if (rects.empty()) {
        return {};
    }

    float left = rects.front().left();
    float top = rects.front().top();
    float right = rects.front().right();
    float bottom = rects.front().bottom();

    for (const Rect& rect : rects) {
        left = std::min(left, rect.left());
        top = std::min(top, rect.top());
        right = std::max(right, rect.right());
        bottom = std::max(bottom, rect.bottom());
    }

    return {left, top, right - left, bottom - top};
}

inline Rect clamp_rect_to_size(Rect rect, const Size& size)
{
    if (!size.valid() || !rect.valid()) {
        return {};
    }

    rect.width = std::min(rect.width, size.width);
    rect.height = std::min(rect.height, size.height);
    rect.x = std::clamp(rect.x, 0.0f, size.width - rect.width);
    rect.y = std::clamp(rect.y, 0.0f, size.height - rect.height);
    return rect;
}

} // namespace autoframing

