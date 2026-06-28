#pragma once

#include "detector.hpp"
#include "letterbox.hpp"

#include <cstddef>
#include <vector>

namespace autoframing {

struct YoloXPostprocessConfig {
    float score_floor = 0.30f;
    float nms_threshold = 0.45f;
    float min_person_class_score = 0.25f;
    float min_person_class_margin = 0.10f;
    bool require_person_best_class = false;
};

struct YoloXClassFilterResult {
    bool accepted = false;
    int best_class_id = -1;
    float best_class_score = 0.0f;
    float person_class_score = 0.0f;
    float objectness = 0.0f;
    float combined_score = 0.0f;
};

YoloXClassFilterResult evaluate_yolox_person_class_scores(
    const float* row,
    size_t features,
    const YoloXPostprocessConfig& config);

std::vector<Detection> postprocess_yolox_person_detections(
    const float* output,
    size_t rows,
    size_t features,
    int input_width,
    int input_height,
    const LetterboxInfo& letterbox,
    const Size& source_size,
    const YoloXPostprocessConfig& config);

} // namespace autoframing
