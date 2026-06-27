#include "detector.hpp"

#include <cmath>

namespace autoframing {
namespace {
constexpr double ns_to_seconds = 1.0 / 1000000000.0;
}

std::vector<Detection> MockPersonDetector::detect(const Frame& frame)
{
    if (!enabled_ || frame.width == 0 || frame.height == 0) {
        return {};
    }

    const float width = static_cast<float>(frame.width);
    const float height = static_cast<float>(frame.height);
    const double seconds = static_cast<double>(frame.timestamp_ns) * ns_to_seconds;

    const float box_width = std::max(80.0f, width * 0.18f);
    const float box_height = std::max(160.0f, height * 0.48f);
    const float center_x = width * (0.5f + 0.28f * static_cast<float>(std::sin(seconds * 0.85)));
    const float center_y = height * (0.52f + 0.05f * static_cast<float>(std::sin(seconds * 0.31)));

    Detection detection;
    detection.box = make_centered_rect(center_x, center_y, box_width, box_height);
    detection.box = clamp_rect_to_size(detection.box, {width, height});
    detection.confidence = 0.95f;
    detection.class_id = 0;
    return {detection};
}

} // namespace autoframing

