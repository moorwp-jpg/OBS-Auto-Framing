#include "yolox_postprocess.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace autoframing {
namespace {

constexpr int person_class_id = 0;

struct Candidate {
    Rect box;
    float score = 0.0f;
};

std::vector<std::pair<int, int>> yolox_grids_for_shape(size_t rows, int input_width, int input_height, std::vector<int>& strides)
{
    std::vector<std::pair<int, int>> grids;
    strides.clear();

    for (int stride : {8, 16, 32}) {
        const int grid_width = input_width / stride;
        const int grid_height = input_height / stride;
        for (int y = 0; y < grid_height; ++y) {
            for (int x = 0; x < grid_width; ++x) {
                grids.emplace_back(x, y);
                strides.push_back(stride);
            }
        }
    }

    if (grids.size() != rows) {
        grids.clear();
        strides.clear();
    }
    return grids;
}

bool output_looks_already_decoded(const float* output, size_t rows, size_t features, int input_width, int input_height)
{
    const size_t sample_count = std::min<size_t>(rows, 256);
    const size_t step = std::max<size_t>(1, rows / sample_count);
    const float wide_x = static_cast<float>(input_width) * 0.75f;
    const float tall_y = static_cast<float>(input_height) * 0.75f;
    const float wide_box = static_cast<float>(input_width) * 0.30f;
    const float tall_box = static_cast<float>(input_height) * 0.30f;

    for (size_t sample = 0; sample < sample_count; ++sample) {
        const float* row = output + std::min(rows - 1, sample * step) * features;
        if (row[0] > wide_x || row[1] > tall_y || row[2] > wide_box || row[3] > tall_box) {
            return true;
        }
    }
    return false;
}

Rect decode_yolox_box(
    const float* row,
    size_t row_index,
    size_t rows,
    int input_width,
    int input_height,
    const std::vector<std::pair<int, int>>& grids,
    const std::vector<int>& strides,
    bool already_decoded)
{
    float center_x = row[0];
    float center_y = row[1];
    float width = row[2];
    float height = row[3];

    if (!already_decoded && row_index < grids.size() && row_index < strides.size() && grids.size() == rows) {
        const int stride = strides[row_index];
        center_x = (center_x + static_cast<float>(grids[row_index].first)) * static_cast<float>(stride);
        center_y = (center_y + static_cast<float>(grids[row_index].second)) * static_cast<float>(stride);
        width = std::exp(std::clamp(width, -20.0f, 20.0f)) * static_cast<float>(stride);
        height = std::exp(std::clamp(height, -20.0f, 20.0f)) * static_cast<float>(stride);
    } else if (center_x <= 1.5f && center_y <= 1.5f && width <= 1.5f && height <= 1.5f) {
        center_x *= static_cast<float>(input_width);
        width *= static_cast<float>(input_width);
        center_y *= static_cast<float>(input_height);
        height *= static_cast<float>(input_height);
    }

    return make_centered_rect(center_x, center_y, width, height);
}

std::vector<Detection> nms(std::vector<Candidate> candidates, float nms_threshold)
{
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::vector<Detection> detections;
    std::vector<bool> suppressed(candidates.size(), false);

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i] || !candidates[i].box.valid()) {
            continue;
        }

        Detection detection;
        detection.box = candidates[i].box;
        detection.confidence = candidates[i].score;
        detection.class_id = person_class_id;
        detections.push_back(detection);

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] && intersection_over_union(candidates[i].box, candidates[j].box) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }

    return detections;
}

} // namespace

YoloXClassFilterResult evaluate_yolox_person_class_scores(
    const float* row,
    size_t features,
    const YoloXPostprocessConfig& config)
{
    YoloXClassFilterResult result;
    if (row == nullptr || features <= 5 + person_class_id) {
        return result;
    }

    result.objectness = row[4];
    result.person_class_score = row[5 + person_class_id];
    result.combined_score = result.objectness * result.person_class_score;

    for (size_t class_index = 0; class_index + 5 < features; ++class_index) {
        const float class_score = row[5 + class_index];
        if (result.best_class_id < 0 || class_score > result.best_class_score) {
            result.best_class_id = static_cast<int>(class_index);
            result.best_class_score = class_score;
        }
    }

    if (result.person_class_score < config.min_person_class_score || result.combined_score < config.score_floor) {
        return result;
    }

    const bool person_is_best = result.best_class_id == person_class_id;
    const bool person_is_near_best =
        !config.require_person_best_class &&
        result.best_class_score - result.person_class_score <= config.min_person_class_margin;
    result.accepted = person_is_best || person_is_near_best;
    return result;
}

std::vector<Detection> postprocess_yolox_person_detections(
    const float* output,
    size_t rows,
    size_t features,
    int input_width,
    int input_height,
    const LetterboxInfo& letterbox,
    const Size& source_size,
    const YoloXPostprocessConfig& config)
{
    if (output == nullptr || features < 6 || rows == 0 || input_width <= 0 || input_height <= 0) {
        return {};
    }

    std::vector<int> strides;
    std::vector<std::pair<int, int>> grids = yolox_grids_for_shape(rows, input_width, input_height, strides);
    const bool already_decoded = output_looks_already_decoded(output, rows, features, input_width, input_height);

    std::vector<Candidate> candidates;
    candidates.reserve(rows);

    for (size_t i = 0; i < rows; ++i) {
        const float* row = output + i * features;
        const YoloXClassFilterResult class_result = evaluate_yolox_person_class_scores(row, features, config);
        if (!class_result.accepted) {
            continue;
        }

        Rect box = decode_yolox_box(row, i, rows, input_width, input_height, grids, strides, already_decoded);
        box = map_letterboxed_model_box_to_source(box, letterbox, source_size);
        if (box.valid()) {
            candidates.push_back({box, class_result.combined_score});
        }
    }

    return nms(std::move(candidates), config.nms_threshold);
}

} // namespace autoframing
