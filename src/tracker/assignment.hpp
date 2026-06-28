#pragma once

#include "geometry.hpp"

#include <cstddef>
#include <vector>

namespace autoframing {

struct AssignmentMatch {
    size_t track_index = 0;
    size_t detection_index = 0;
    float iou = 0.0f;
};

std::vector<AssignmentMatch> greedy_iou_match(
    const std::vector<Rect>& track_boxes,
    const std::vector<Rect>& detection_boxes,
    float iou_threshold);

} // namespace autoframing

