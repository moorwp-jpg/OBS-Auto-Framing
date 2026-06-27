#include "onnx_person_detector.hpp"

#include "geometry.hpp"
#include "letterbox.hpp"

#include <obs-module.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace autoframing {
namespace {

constexpr int default_input_size = 416;
constexpr int person_class_id = 0;
constexpr float letterbox_pad = 114.0f;

struct Candidate {
    Rect box;
    float score = 0.0f;
};

std::wstring widen_path(const std::string& path)
{
#ifdef _WIN32
    if (path.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return std::wstring(path.begin(), path.end());
    }

    std::wstring wide(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), required);
    return wide;
#else
    return std::wstring(path.begin(), path.end());
#endif
}

std::string format_shape(const std::vector<int64_t>& shape)
{
    std::string formatted = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i > 0) {
            formatted += ", ";
        }
        formatted += std::to_string(shape[i]);
    }
    formatted += "]";
    return formatted;
}

float sample_channel(const Frame& frame, float x, float y, int channel)
{
    const float clamped_x = std::clamp(x, 0.0f, static_cast<float>(frame.width - 1));
    const float clamped_y = std::clamp(y, 0.0f, static_cast<float>(frame.height - 1));

    const int x0 = static_cast<int>(std::floor(clamped_x));
    const int y0 = static_cast<int>(std::floor(clamped_y));
    const int x1 = std::min<int>(x0 + 1, static_cast<int>(frame.width - 1));
    const int y1 = std::min<int>(y0 + 1, static_cast<int>(frame.height - 1));
    const float wx = clamped_x - static_cast<float>(x0);
    const float wy = clamped_y - static_cast<float>(y0);

    const auto pixel = [&frame, channel](int px, int py) -> float {
        const size_t offset = static_cast<size_t>(py) * frame.rgba_stride + static_cast<size_t>(px) * 4 + channel;
        return static_cast<float>(frame.rgba[offset]);
    };

    const float top = pixel(x0, y0) * (1.0f - wx) + pixel(x1, y0) * wx;
    const float bottom = pixel(x0, y1) * (1.0f - wx) + pixel(x1, y1) * wx;
    return top * (1.0f - wy) + bottom * wy;
}

std::vector<float> preprocess_rgba_to_chw(const Frame& frame, int input_width, int input_height, LetterboxInfo& info)
{
    info.input_width = input_width;
    info.input_height = input_height;
    info.scale = std::min(
        static_cast<float>(input_width) / static_cast<float>(frame.width),
        static_cast<float>(input_height) / static_cast<float>(frame.height));
    info.resized_width = std::max(1, static_cast<int>(std::round(static_cast<float>(frame.width) * info.scale)));
    info.resized_height = std::max(1, static_cast<int>(std::round(static_cast<float>(frame.height) * info.scale)));
    info.pad_x = 0.0f;
    info.pad_y = 0.0f;

    const size_t plane = static_cast<size_t>(input_width) * input_height;
    std::vector<float> tensor(plane * 3, letterbox_pad);

    for (int y = 0; y < info.resized_height; ++y) {
        const int dst_y = static_cast<int>(std::round(info.pad_y)) + y;
        if (dst_y < 0 || dst_y >= input_height) {
            continue;
        }

        const float src_y = (static_cast<float>(y) + 0.5f) / info.scale - 0.5f;
        for (int x = 0; x < info.resized_width; ++x) {
            const int dst_x = static_cast<int>(std::round(info.pad_x)) + x;
            if (dst_x < 0 || dst_x >= input_width) {
                continue;
            }

            const float src_x = (static_cast<float>(x) + 0.5f) / info.scale - 0.5f;
            const size_t index = static_cast<size_t>(dst_y) * input_width + dst_x;

            tensor[index] = sample_channel(frame, src_x, src_y, 0);
            tensor[plane + index] = sample_channel(frame, src_x, src_y, 1);
            tensor[plane * 2 + index] = sample_channel(frame, src_x, src_y, 2);
        }
    }

    return tensor;
}

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

Rect undo_letterbox(Rect box, const LetterboxInfo& info, const Size& source_size)
{
    return map_letterboxed_model_box_to_source(box, info, source_size);
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

struct OnnxPersonDetector::Impl {
    OnnxPersonDetectorConfig config;
    std::string error_message;
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<int64_t> input_shape;
    std::vector<int64_t> output_shape;
    int input_width = default_input_size;
    int input_height = default_input_size;

    explicit Impl(OnnxPersonDetectorConfig detector_config) : config(std::move(detector_config))
    {
        try {
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "obs-auto-framing");

            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(std::max(1u, std::min(4u, std::thread::hardware_concurrency())));
            options.SetInterOpNumThreads(1);

#ifdef _WIN32
            const std::wstring wide_model_path = widen_path(config.model_path);
            session = std::make_unique<Ort::Session>(*env, wide_model_path.c_str(), options);
#else
            session = std::make_unique<Ort::Session>(*env, config.model_path.c_str(), options);
#endif

            Ort::AllocatorWithDefaultOptions allocator;
            const size_t input_count = session->GetInputCount();
            const size_t output_count = session->GetOutputCount();

            for (size_t i = 0; i < input_count; ++i) {
                auto name = session->GetInputNameAllocated(i, allocator);
                input_names.emplace_back(name.get());
            }
            for (size_t i = 0; i < output_count; ++i) {
                auto name = session->GetOutputNameAllocated(i, allocator);
                output_names.emplace_back(name.get());
            }

            input_shape = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            output_shape = session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();

            if (input_shape.size() == 4) {
                if (input_shape[2] > 0) {
                    input_height = static_cast<int>(input_shape[2]);
                }
                if (input_shape[3] > 0) {
                    input_width = static_cast<int>(input_shape[3]);
                }
            }

            blog(
                LOG_INFO,
                "[obs-auto-framing] ONNX model loaded: %s",
                config.model_path.c_str());
            blog(
                LOG_INFO,
                "[obs-auto-framing] ONNX input tensor: name=%s shape=%s",
                input_names.empty() ? "(none)" : input_names.front().c_str(),
                format_shape(input_shape).c_str());
            blog(
                LOG_INFO,
                "[obs-auto-framing] ONNX output tensor: name=%s shape=%s",
                output_names.empty() ? "(none)" : output_names.front().c_str(),
                format_shape(output_shape).c_str());
            blog(LOG_INFO, "[obs-auto-framing] ONNX Runtime initialization succeeded");
        } catch (const std::exception& exception) {
            error_message = exception.what();
            blog(LOG_ERROR, "[obs-auto-framing] ONNX detector initialization failed: %s", error_message.c_str());
            session.reset();
        }
    }

    bool ready() const { return session != nullptr && input_names.size() == 1 && !output_names.empty(); }
};

OnnxPersonDetector::OnnxPersonDetector(OnnxPersonDetectorConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

OnnxPersonDetector::~OnnxPersonDetector() = default;

bool OnnxPersonDetector::ready() const
{
    return impl_ != nullptr && impl_->ready();
}

const std::string& OnnxPersonDetector::error() const
{
    static const std::string empty;
    return impl_ != nullptr ? impl_->error_message : empty;
}

std::vector<Detection> OnnxPersonDetector::detect(const Frame& frame)
{
    if (!ready()) {
        return {};
    }

    if (!frame.has_rgba()) {
        blog(LOG_WARNING, "[obs-auto-framing] ONNX detector received a frame without RGBA pixels");
        return {};
    }

    try {
        LetterboxInfo letterbox;
        std::vector<float> input_tensor = preprocess_rgba_to_chw(frame, impl_->input_width, impl_->input_height, letterbox);
        std::array<int64_t, 4> input_shape{
            1,
            3,
            static_cast<int64_t>(impl_->input_height),
            static_cast<int64_t>(impl_->input_width)};

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory_info,
            input_tensor.data(),
            input_tensor.size(),
            input_shape.data(),
            input_shape.size());

        std::array<const char*, 1> input_names{impl_->input_names.front().c_str()};
        std::vector<const char*> output_names;
        output_names.reserve(impl_->output_names.size());
        for (const std::string& output_name : impl_->output_names) {
            output_names.push_back(output_name.c_str());
        }

        std::vector<Ort::Value> outputs = impl_->session->Run(
            Ort::RunOptions{nullptr},
            input_names.data(),
            &input,
            input_names.size(),
            output_names.data(),
            output_names.size());

        if (outputs.empty() || !outputs.front().IsTensor()) {
            blog(LOG_WARNING, "[obs-auto-framing] ONNX inference returned no tensor outputs");
            return {};
        }

        Ort::TensorTypeAndShapeInfo output_info = outputs.front().GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> shape = output_info.GetShape();
        const size_t element_count = output_info.GetElementCount();
        const float* output = outputs.front().GetTensorData<float>();

        size_t rows = 0;
        size_t features = 0;
        if (shape.size() == 3) {
            rows = static_cast<size_t>(shape[1]);
            features = static_cast<size_t>(shape[2]);
        } else if (shape.size() == 2) {
            rows = static_cast<size_t>(shape[0]);
            features = static_cast<size_t>(shape[1]);
        } else {
            blog(LOG_WARNING, "[obs-auto-framing] unexpected YOLOX output rank: %zu", shape.size());
            return {};
        }

        if (features < 6 || rows == 0 || rows * features > element_count) {
            blog(LOG_WARNING, "[obs-auto-framing] unexpected YOLOX output shape rows=%zu features=%zu", rows, features);
            return {};
        }

        std::vector<int> strides;
        std::vector<std::pair<int, int>> grids = yolox_grids_for_shape(rows, impl_->input_width, impl_->input_height, strides);
        const bool already_decoded = output_looks_already_decoded(output, rows, features, impl_->input_width, impl_->input_height);

        std::vector<Candidate> candidates;
        candidates.reserve(rows);
        const Size source_size{static_cast<float>(frame.width), static_cast<float>(frame.height)};

        for (size_t i = 0; i < rows; ++i) {
            const float* row = output + i * features;
            const float objectness = row[4];
            const float person_score = row[5 + person_class_id];
            const float score = objectness * person_score;

            if (score < impl_->config.confidence_threshold) {
                continue;
            }

            Rect box = decode_yolox_box(row, i, rows, impl_->input_width, impl_->input_height, grids, strides, already_decoded);
            box = undo_letterbox(box, letterbox, source_size);
            if (box.valid()) {
                candidates.push_back({box, score});
            }
        }

        std::vector<Detection> detections = nms(std::move(candidates), impl_->config.nms_threshold);
        return detections;
    } catch (const std::exception& exception) {
        blog(LOG_ERROR, "[obs-auto-framing] ONNX inference failure: %s", exception.what());
        return {};
    }
}

} // namespace autoframing
