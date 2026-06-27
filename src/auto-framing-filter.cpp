#include "crop_controller.hpp"
#include "crop_renderer.hpp"
#include "detector.hpp"
#include "onnx_person_detector.hpp"
#include "settings.hpp"
#include "tracker.hpp"
#include "yuv.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace autoframing {
namespace {

enum class RuntimeStatus {
    Running,
    NoFrameYet,
    DetectorUnavailable,
    ModelMissing,
    Error,
};

struct RuntimeInfo {
    RuntimeStatus status = RuntimeStatus::NoFrameYet;
    std::string status_detail = "Waiting for source video.";
    DetectorBackend detector_backend = DetectorBackend::Mock;
    std::string model_path;
    bool model_loaded = false;
    double last_inference_ms = 0.0;
    size_t last_detection_count = 0;
    size_t active_track_count = 0;
    Rect current_crop;
};

struct AutoFramingFilter {
    obs_source_t* source = nullptr;
    AutoFramingSettings settings = default_settings();
    std::mutex settings_mutex;

    std::unique_ptr<PersonDetector> detector;
    std::string detector_signature;
    IouTracker tracker;
    CropController crop_controller;
    CropRenderer renderer;

    uint64_t last_submit_ns = 0;
    uint64_t last_detection_log_ns = 0;
    uint64_t last_crop_log_ns = 0;
    std::vector<PersonTrack> tracks;
    std::atomic<bool> detector_available{false};

    std::mutex latest_frame_mutex;
    Frame latest_frame;
    bool has_latest_frame = false;

    std::thread detection_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    bool stop_worker = false;
    bool pending_frame_available = false;
    bool worker_enabled = true;
    bool render_enabled = true;
    Frame pending_frame;
    AutoFramingSettings pending_settings;

    std::mutex detection_result_mutex;
    std::vector<Detection> latest_detections;
    uint64_t latest_detection_timestamp_ns = 0;
    bool latest_detection_available = false;

    std::mutex debug_mutex;
    DebugOverlayData debug_data;

    std::mutex runtime_mutex;
    RuntimeInfo runtime;

    std::atomic<bool> unsupported_frame_logged{false};
    std::atomic<bool> captured_frame_logged{false};

    std::atomic<float> crop_x{0.0f};
    std::atomic<float> crop_y{0.0f};
    std::atomic<float> crop_width{0.0f};
    std::atomic<float> crop_height{0.0f};
    std::atomic<uint32_t> source_width{0};
    std::atomic<uint32_t> source_height{0};
};

void set_runtime_status(AutoFramingFilter* filter, RuntimeStatus status, std::string detail)
{
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.status = status;
    filter->runtime.status_detail = std::move(detail);
}

void set_runtime_detector_state(
    AutoFramingFilter* filter,
    DetectorBackend backend,
    std::string model_path,
    bool model_loaded,
    RuntimeStatus status,
    std::string detail)
{
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.detector_backend = backend;
    filter->runtime.model_path = std::move(model_path);
    filter->runtime.model_loaded = model_loaded;
    filter->runtime.status = status;
    filter->runtime.status_detail = std::move(detail);
}

void set_runtime_inference_stats(AutoFramingFilter* filter, double inference_ms, size_t detection_count)
{
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.last_inference_ms = inference_ms;
    filter->runtime.last_detection_count = detection_count;
}

void set_runtime_tracking_stats(AutoFramingFilter* filter, size_t active_track_count, const Rect& current_crop)
{
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.active_track_count = active_track_count;
    filter->runtime.current_crop = current_crop;
}

RuntimeInfo runtime_info_for(void* data)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return {};
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    return filter->runtime;
}

bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }

    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "FALSE") != 0;
}

const char* text(const char* key)
{
    const char* translated = obs_module_text(key);
    return translated != nullptr ? translated : key;
}

const char* runtime_status_to_string(RuntimeStatus status)
{
    switch (status) {
    case RuntimeStatus::Running:
        return "Running";
    case RuntimeStatus::DetectorUnavailable:
        return "Detector unavailable";
    case RuntimeStatus::ModelMissing:
        return "Model missing";
    case RuntimeStatus::Error:
        return "Error";
    case RuntimeStatus::NoFrameYet:
    default:
        return "No frame yet";
    }
}

const char* detector_backend_display(DetectorBackend backend)
{
    return backend == DetectorBackend::OnnxRuntimeCpu ? "ONNX Runtime CPU" : "Mock";
}

std::string format_rect(const Rect& rect)
{
    if (!rect.valid()) {
        return "n/a";
    }

    char buffer[128] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "x=%.1f y=%.1f w=%.1f h=%.1f",
        rect.x,
        rect.y,
        rect.width,
        rect.height);
    return buffer;
}

AutoFramingSettings load_settings(obs_data_t* data)
{
    AutoFramingSettings settings = default_settings();
    settings.tracking_speed = obs_data_get_double(data, setting_keys::tracking_speed);
    settings.max_zoom = obs_data_get_double(data, setting_keys::max_zoom);
    settings.framing_preset = framing_preset_from_string(obs_data_get_string(data, setting_keys::framing_preset));
    settings.tracking_mode = tracking_mode_from_string(obs_data_get_string(data, setting_keys::tracking_mode));
    settings.detection_interval_ms = static_cast<uint32_t>(obs_data_get_int(data, setting_keys::detection_interval_ms));
    settings.dead_zone = obs_data_get_double(data, setting_keys::dead_zone);
    settings.debug_overlay = obs_data_get_bool(data, setting_keys::debug_overlay);
    settings.use_mock_detector = obs_data_get_bool(data, setting_keys::use_mock_detector);
    settings.detector_backend = detector_backend_from_string(obs_data_get_string(data, setting_keys::detector_backend));
    settings.model_path = obs_data_get_string(data, setting_keys::model_path);
    settings.detection_confidence = obs_data_get_double(data, setting_keys::detection_confidence);
    settings.nms_threshold = obs_data_get_double(data, setting_keys::nms_threshold);
    return sanitize_settings(settings);
}

bool path_is_absolute(const std::string& path)
{
    if (path.empty()) {
        return false;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }

    return path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

bool file_exists(const std::string& path)
{
    return !path.empty() && os_file_exists(path.c_str());
}

struct ModelResolution {
    std::string path;
    std::string detail;
    bool found = false;
};

ModelResolution resolve_model_path(const std::string& configured_path, bool log_configured_missing = true)
{
    if (!configured_path.empty()) {
        if (path_is_absolute(configured_path) && file_exists(configured_path)) {
            return {configured_path, "Using explicit model path.", true};
        }

        if (!path_is_absolute(configured_path)) {
            char* module_relative_path = obs_module_file(configured_path.c_str());
            if (module_relative_path != nullptr) {
                std::string path = module_relative_path;
                bfree(module_relative_path);
                if (file_exists(path)) {
                    return {path, "Using explicit model path relative to the plugin data directory.", true};
                }
            }

            if (file_exists(configured_path)) {
                return {configured_path, "Using explicit model path relative to OBS current working directory.", true};
            }
        }

        if (log_configured_missing) {
            blog(LOG_WARNING, "[obs-auto-framing] configured ONNX model path does not exist: %s", configured_path.c_str());
        }
    }

    char* module_model_path = obs_module_file("models/yolox_nano.onnx");
    if (module_model_path != nullptr) {
        std::string path = module_model_path;
        bfree(module_model_path);
        if (file_exists(path)) {
            return {path, "Using model from OBS plugin data directory.", true};
        }
    }

#ifdef DEFAULT_YOLOX_NANO_MODEL
    if (file_exists(DEFAULT_YOLOX_NANO_MODEL)) {
        return {DEFAULT_YOLOX_NANO_MODEL, "Using development model path.", true};
    }
#endif

    return {{}, "YOLOX-Nano model file was not found.", false};
}

std::string signature_for(const AutoFramingSettings& settings)
{
    std::string signature = detector_backend_to_string(settings.detector_backend);
    if (settings.detector_backend == DetectorBackend::OnnxRuntimeCpu) {
        signature += "|";
        signature += resolve_model_path(settings.model_path, false).path;
        signature += "|";
        signature += std::to_string(settings.detection_confidence);
        signature += "|";
        signature += std::to_string(settings.nms_threshold);
    }
    return signature;
}

std::unique_ptr<PersonDetector> make_detector(AutoFramingFilter* filter, const AutoFramingSettings& settings)
{
    if (settings.detector_backend == DetectorBackend::Mock) {
        auto detector = std::make_unique<MockPersonDetector>();
        detector->set_enabled(true);
        blog(LOG_INFO, "[obs-auto-framing] detector backend: mock");
        if (filter != nullptr) {
            filter->detector_available.store(true, std::memory_order_relaxed);
        }
        set_runtime_detector_state(filter, DetectorBackend::Mock, {}, false, RuntimeStatus::Running, "Mock detector is running.");
        return detector;
    }

    const ModelResolution model = resolve_model_path(settings.model_path);
    blog(LOG_INFO, "[obs-auto-framing] detector backend: ONNX Runtime CPU");
    blog(LOG_INFO, "[obs-auto-framing] resolved ONNX model path: %s", model.path.empty() ? "(missing)" : model.path.c_str());

    if (!model.found) {
        if (filter != nullptr) {
            filter->detector_available.store(false, std::memory_order_relaxed);
        }
        blog(LOG_ERROR, "[obs-auto-framing] ONNX detector unavailable: %s", model.detail.c_str());
        set_runtime_detector_state(filter, DetectorBackend::OnnxRuntimeCpu, {}, false, RuntimeStatus::ModelMissing, model.detail);
        return std::make_unique<NullPersonDetector>();
    }

    OnnxPersonDetectorConfig config;
    config.model_path = model.path;
    config.confidence_threshold = static_cast<float>(settings.detection_confidence);
    config.nms_threshold = static_cast<float>(settings.nms_threshold);

    auto detector = std::make_unique<OnnxPersonDetector>(config);
    if (detector->ready()) {
        if (filter != nullptr) {
            filter->detector_available.store(true, std::memory_order_relaxed);
        }
        set_runtime_detector_state(
            filter,
            DetectorBackend::OnnxRuntimeCpu,
            model.path,
            true,
            RuntimeStatus::Running,
            "ONNX Runtime CPU detector is running.");
        return detector;
    }

    blog(
        LOG_ERROR,
        "[obs-auto-framing] ONNX detector unavailable; detections will be empty until settings are fixed: %s",
        detector->error().c_str());
    if (filter != nullptr) {
        filter->detector_available.store(false, std::memory_order_relaxed);
    }
    set_runtime_detector_state(
        filter,
        DetectorBackend::OnnxRuntimeCpu,
        model.path,
        false,
        RuntimeStatus::DetectorUnavailable,
        detector->error());
    return std::make_unique<NullPersonDetector>();
}

void detection_worker_loop(AutoFramingFilter* filter)
{
    blog(LOG_INFO, "[obs-auto-framing] detection worker started");

    for (;;) {
        Frame frame;
        AutoFramingSettings settings;

        {
            std::unique_lock<std::mutex> lock(filter->worker_mutex);
            filter->worker_cv.wait(lock, [filter] { return filter->stop_worker || filter->pending_frame_available; });
            if (filter->stop_worker) {
                blog(LOG_INFO, "[obs-auto-framing] detection worker stopped");
                return;
            }

            frame = std::move(filter->pending_frame);
            settings = filter->pending_settings;
            filter->pending_frame = {};
            filter->pending_frame_available = false;
        }

        const std::string signature = signature_for(settings);
        if (filter->detector == nullptr || filter->detector_signature != signature) {
            filter->detector = make_detector(filter, settings);
            filter->detector_signature = signature;
        }

        const uint64_t inference_start_ns = os_gettime_ns();
        std::vector<Detection> detections = filter->detector != nullptr ? filter->detector->detect(frame) : std::vector<Detection>{};
        const uint64_t inference_end_ns = os_gettime_ns();
        const double inference_ms = static_cast<double>(inference_end_ns - inference_start_ns) / 1000000.0;
        const size_t detection_count = detections.size();
        set_runtime_inference_stats(filter, inference_ms, detection_count);

        {
            std::lock_guard<std::mutex> lock(filter->detection_result_mutex);
            filter->latest_detections = std::move(detections);
            filter->latest_detection_timestamp_ns = frame.timestamp_ns;
            filter->latest_detection_available = true;
        }

        const uint64_t now = os_gettime_ns();
        if (filter->last_detection_log_ns == 0 || now - filter->last_detection_log_ns >= 2000000000ULL) {
            blog(
                LOG_INFO,
                "[obs-auto-framing] detections=%zu inference=%.2f ms backend=%s",
                detection_count,
                inference_ms,
                detector_backend_to_string(settings.detector_backend));
            filter->last_detection_log_ns = now;
        }
    }
}

bool copy_source_frame_to_rgba(const struct obs_source_frame* source_frame, Frame& frame)
{
    if (source_frame == nullptr || source_frame->width == 0 || source_frame->height == 0 || source_frame->data[0] == nullptr) {
        return false;
    }

    frame.width = source_frame->width;
    frame.height = source_frame->height;
    frame.timestamp_ns = source_frame->timestamp != 0 ? source_frame->timestamp : os_gettime_ns();
    frame.rgba_stride = source_frame->width * 4;
    frame.rgba.assign(static_cast<size_t>(frame.rgba_stride) * frame.height, 0);

    for (uint32_t y = 0; y < frame.height; ++y) {
        uint8_t* dst = frame.rgba.data() + static_cast<size_t>(y) * frame.rgba_stride;

        switch (source_frame->format) {
        case VIDEO_FORMAT_RGBA: {
            std::memcpy(dst, source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0], frame.rgba_stride);
            break;
        }
        case VIDEO_FORMAT_BGRA:
        case VIDEO_FORMAT_BGRX: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
            break;
        }
        case VIDEO_FORMAT_BGR3: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; ++x) {
                dst[x * 4 + 0] = src[x * 3 + 2];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 0];
                dst[x * 4 + 3] = 255;
            }
            break;
        }
        case VIDEO_FORMAT_Y800: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; ++x) {
                dst[x * 4 + 0] = src[x];
                dst[x * 4 + 1] = src[x];
                dst[x * 4 + 2] = src[x];
                dst[x * 4 + 3] = 255;
            }
            break;
        }
        case VIDEO_FORMAT_NV12: {
            if (source_frame->data[1] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* uv_plane = source_frame->data[1] + static_cast<size_t>(y / 2) * source_frame->linesize[1];
            for (uint32_t x = 0; x < frame.width; ++x) {
                yuv_to_rgba(y_plane[x], uv_plane[(x / 2) * 2], uv_plane[(x / 2) * 2 + 1], dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_I420: {
            if (source_frame->data[1] == nullptr || source_frame->data[2] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* u_plane = source_frame->data[1] + static_cast<size_t>(y / 2) * source_frame->linesize[1];
            const uint8_t* v_plane = source_frame->data[2] + static_cast<size_t>(y / 2) * source_frame->linesize[2];
            for (uint32_t x = 0; x < frame.width; ++x) {
                yuv_to_rgba(y_plane[x], u_plane[x / 2], v_plane[x / 2], dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_YUY2: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; x += 2) {
                const bool has_second_pixel = x + 1 < frame.width;
                const PackedYuvGroup group = decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Yuy2, has_second_pixel);
                yuv_to_rgba(group.y0, group.u, group.v, dst + x * 4);
                if (group.has_second_pixel) {
                    yuv_to_rgba(group.y1, group.u, group.v, dst + (x + 1) * 4);
                }
            }
            break;
        }
        case VIDEO_FORMAT_UYVY: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; x += 2) {
                const bool has_second_pixel = x + 1 < frame.width;
                const PackedYuvGroup group = decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Uyvy, has_second_pixel);
                yuv_to_rgba(group.y0, group.u, group.v, dst + x * 4);
                if (group.has_second_pixel) {
                    yuv_to_rgba(group.y1, group.u, group.v, dst + (x + 1) * 4);
                }
            }
            break;
        }
        case VIDEO_FORMAT_YVYU: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; x += 2) {
                const bool has_second_pixel = x + 1 < frame.width;
                const PackedYuvGroup group = decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Yvyu, has_second_pixel);
                yuv_to_rgba(group.y0, group.u, group.v, dst + x * 4);
                if (group.has_second_pixel) {
                    yuv_to_rgba(group.y1, group.u, group.v, dst + (x + 1) * 4);
                }
            }
            break;
        }
        default:
            return false;
        }
    }

    return true;
}

obs_source_t* target_for(AutoFramingFilter* filter)
{
    return filter != nullptr && filter->source != nullptr ? obs_filter_get_target(filter->source) : nullptr;
}

uint32_t target_width(AutoFramingFilter* filter)
{
    obs_source_t* target = target_for(filter);
    if (target == nullptr) {
        return 0;
    }

    uint32_t width = obs_source_get_base_width(target);
    if (width == 0) {
        width = obs_source_get_width(target);
    }
    return width;
}

uint32_t target_height(AutoFramingFilter* filter)
{
    obs_source_t* target = target_for(filter);
    if (target == nullptr) {
        return 0;
    }

    uint32_t height = obs_source_get_base_height(target);
    if (height == 0) {
        height = obs_source_get_height(target);
    }
    return height;
}

const char* auto_framing_get_name(void*)
{
    return text("AutoFramingFilter");
}

void auto_framing_defaults(obs_data_t* data)
{
    const AutoFramingSettings settings = default_settings();
    obs_data_set_default_double(data, setting_keys::tracking_speed, settings.tracking_speed);
    obs_data_set_default_double(data, setting_keys::max_zoom, settings.max_zoom);
    obs_data_set_default_string(data, setting_keys::framing_preset, framing_preset_to_string(settings.framing_preset));
    obs_data_set_default_string(data, setting_keys::tracking_mode, tracking_mode_to_string(settings.tracking_mode));
    obs_data_set_default_int(data, setting_keys::detection_interval_ms, settings.detection_interval_ms);
    obs_data_set_default_double(data, setting_keys::dead_zone, settings.dead_zone);
    obs_data_set_default_bool(data, setting_keys::debug_overlay, settings.debug_overlay);
    obs_data_set_default_bool(data, setting_keys::use_mock_detector, settings.use_mock_detector);
    obs_data_set_default_string(data, setting_keys::detector_backend, detector_backend_to_string(settings.detector_backend));
    obs_data_set_default_string(data, setting_keys::model_path, "");
    obs_data_set_default_double(data, setting_keys::detection_confidence, settings.detection_confidence);
    obs_data_set_default_double(data, setting_keys::nms_threshold, settings.nms_threshold);
}

obs_properties_t* auto_framing_properties(void* data)
{
    obs_properties_t* props = obs_properties_create();

    const RuntimeInfo runtime = runtime_info_for(data);
    const std::string status_text = std::string("Status: ") + runtime_status_to_string(runtime.status) + " - " + runtime.status_detail;
    const std::string backend_text = std::string("Detector backend: ") + detector_backend_display(runtime.detector_backend);
    const std::string model_loaded_text = std::string("Model loaded: ") + (runtime.model_loaded ? "yes" : "no");
    const std::string model_path_text = std::string("Model path: ") + (runtime.model_path.empty() ? "n/a" : runtime.model_path);
    char inference_buffer[128] = {};
    std::snprintf(inference_buffer, sizeof(inference_buffer), "Last inference: %.2f ms", runtime.last_inference_ms);
    const std::string detection_text = std::string("Last detections: ") + std::to_string(runtime.last_detection_count);
    const std::string track_text = std::string("Active tracks: ") + std::to_string(runtime.active_track_count);
    const std::string crop_text = std::string("Current crop: ") + format_rect(runtime.current_crop);

    obs_property_t* status_prop = obs_properties_add_text(props, "runtime_status", status_text.c_str(), OBS_TEXT_INFO);
    if (runtime.status == RuntimeStatus::Error || runtime.status == RuntimeStatus::DetectorUnavailable || runtime.status == RuntimeStatus::ModelMissing) {
        obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_ERROR);
    } else if (runtime.status == RuntimeStatus::NoFrameYet) {
        obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_WARNING);
    }
    obs_property_text_set_info_word_wrap(status_prop, true);

    obs_property_text_set_info_word_wrap(
        obs_properties_add_text(props, "runtime_refresh_note", "Runtime status refreshes when this properties window is reopened.", OBS_TEXT_INFO),
        true);

    obs_properties_add_text(props, "runtime_backend", backend_text.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_model_loaded", model_loaded_text.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(obs_properties_add_text(props, "runtime_model_path", model_path_text.c_str(), OBS_TEXT_INFO), true);
    obs_properties_add_text(props, "runtime_inference", inference_buffer, OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_detections", detection_text.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_tracks", track_text.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_crop", crop_text.c_str(), OBS_TEXT_INFO);

    obs_property_t* backend = obs_properties_add_list(
        props,
        setting_keys::detector_backend,
        text("DetectorBackend"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, text("DetectorBackend.Mock"), "mock");
    obs_property_list_add_string(backend, text("DetectorBackend.OnnxRuntimeCpu"), "onnxruntime_cpu");

    obs_properties_add_path(
        props,
        setting_keys::model_path,
        text("ModelPath"),
        OBS_PATH_FILE,
        "ONNX Model (*.onnx)",
        nullptr);
    obs_properties_add_float_slider(props, setting_keys::detection_confidence, text("DetectionConfidence"), 0.01, 0.99, 0.01);
    obs_properties_add_float_slider(props, setting_keys::nms_threshold, text("NmsThreshold"), 0.05, 0.95, 0.01);

    obs_properties_add_float_slider(props, setting_keys::tracking_speed, text("TrackingSpeed"), 0.01, 1.0, 0.01);
    obs_properties_add_float_slider(props, setting_keys::max_zoom, text("MaxZoom"), 1.0, 8.0, 0.1);

    obs_property_t* preset = obs_properties_add_list(
        props,
        setting_keys::framing_preset,
        text("FramingPreset"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(preset, text("FramingPreset.Balanced"), "balanced");
    obs_property_list_add_string(preset, text("FramingPreset.Tight"), "tight");
    obs_property_list_add_string(preset, text("FramingPreset.Headroom"), "headroom");
    obs_property_list_add_string(preset, text("FramingPreset.FullBody"), "full_body");

    obs_property_t* mode = obs_properties_add_list(
        props,
        setting_keys::tracking_mode,
        text("TrackingMode"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(mode, text("TrackingMode.Presenter"), "presenter");
    obs_property_list_add_string(mode, text("TrackingMode.Group"), "group");

    obs_properties_add_int_slider(props, setting_keys::detection_interval_ms, text("DetectionInterval"), 16, 2000, 1);
    obs_properties_add_float_slider(props, setting_keys::dead_zone, text("DeadZone"), 0.0, 0.45, 0.01);
    obs_properties_add_bool(props, setting_keys::debug_overlay, text("DebugOverlay"));

    return props;
}

void* auto_framing_create(obs_data_t* settings, obs_source_t* source)
{
    auto filter = std::make_unique<AutoFramingFilter>();
    filter->source = source;
    filter->settings = load_settings(settings);
    filter->worker_enabled = !env_flag_enabled("OBS_AUTO_FRAMING_DISABLE_WORKER");
    filter->render_enabled = !env_flag_enabled("OBS_AUTO_FRAMING_DISABLE_RENDER");
    filter->runtime.detector_backend = filter->settings.detector_backend;
    filter->runtime.status = RuntimeStatus::NoFrameYet;
    filter->runtime.status_detail = "Waiting for source video.";
    blog(
        LOG_INFO,
        "[obs-auto-framing] filter created; backend=%s render=%s worker=%s",
        detector_backend_to_string(filter->settings.detector_backend),
        filter->render_enabled ? "enabled" : "disabled",
        filter->worker_enabled ? "enabled" : "disabled");

    if (filter->render_enabled && !filter->renderer.initialize()) {
        blog(LOG_ERROR, "[obs-auto-framing] crop renderer initialization failed; filter will pass video through");
        filter->render_enabled = false;
        set_runtime_status(filter.get(), RuntimeStatus::Error, "Crop renderer unavailable.");
    }

    if (filter->worker_enabled) {
        filter->detection_thread = std::thread(detection_worker_loop, filter.get());
    } else {
        blog(LOG_WARNING, "[obs-auto-framing] detection worker disabled by OBS_AUTO_FRAMING_DISABLE_WORKER");
    }
    return filter.release();
}

void auto_framing_destroy(void* data)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return;
    }

    blog(LOG_INFO, "[obs-auto-framing] filter destroying");

    {
        std::lock_guard<std::mutex> lock(filter->worker_mutex);
        filter->stop_worker = true;
        filter->pending_frame_available = false;
    }
    filter->worker_cv.notify_one();

    if (filter->detection_thread.joinable()) {
        filter->detection_thread.join();
    }

    delete filter;
    blog(LOG_INFO, "[obs-auto-framing] filter destroyed");
}

void auto_framing_update(void* data, obs_data_t* obs_settings)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->settings_mutex);
    const AutoFramingSettings previous = filter->settings;
    filter->settings = load_settings(obs_settings);
    if (previous.detector_backend != filter->settings.detector_backend) {
        blog(
            LOG_INFO,
            "[obs-auto-framing] detector backend setting changed: %s",
            detector_backend_to_string(filter->settings.detector_backend));
    }
}

void auto_framing_video_tick(void* data, float seconds)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return;
    }

    AutoFramingSettings settings;
    {
        std::lock_guard<std::mutex> lock(filter->settings_mutex);
        settings = filter->settings;
    }

    uint32_t width = target_width(filter);
    uint32_t height = target_height(filter);
    if (width > 0 && height > 0) {
        filter->source_width.store(width, std::memory_order_relaxed);
        filter->source_height.store(height, std::memory_order_relaxed);
    } else {
        width = filter->source_width.load(std::memory_order_relaxed);
        height = filter->source_height.load(std::memory_order_relaxed);
    }

    if (width == 0 || height == 0) {
        filter->crop_controller.reset();
        set_runtime_status(filter, RuntimeStatus::NoFrameYet, "Waiting for source dimensions.");
        return;
    }

    const uint64_t now = os_gettime_ns();
    const uint64_t interval_ns = static_cast<uint64_t>(settings.detection_interval_ms) * 1000000ULL;
    const bool should_detect = filter->last_submit_ns == 0 || now - filter->last_submit_ns >= interval_ns;

    {
        std::lock_guard<std::mutex> lock(filter->detection_result_mutex);
        if (filter->latest_detection_available) {
            filter->tracks = filter->tracker.update(filter->latest_detections, filter->latest_detection_timestamp_ns);
            filter->latest_detection_available = false;
        }
    }

    if (filter->worker_enabled && should_detect) {
        Frame frame{width, height, now};
        if (settings.detector_backend == DetectorBackend::OnnxRuntimeCpu) {
            std::lock_guard<std::mutex> lock(filter->latest_frame_mutex);
            if (filter->has_latest_frame) {
                frame = filter->latest_frame;
            } else {
                frame = {};
                if (filter->detector_available.load(std::memory_order_relaxed)) {
                    set_runtime_status(filter, RuntimeStatus::NoFrameYet, "Waiting for captured RGBA video frame.");
                }
            }
        }

        if (settings.detector_backend == DetectorBackend::Mock || frame.has_rgba()) {
            std::lock_guard<std::mutex> lock(filter->worker_mutex);
            if (!filter->pending_frame_available) {
                filter->pending_frame = std::move(frame);
                filter->pending_settings = settings;
                filter->pending_frame_available = true;
                filter->last_submit_ns = now;
                filter->worker_cv.notify_one();
            }
        }
    }

    const Rect crop = filter->crop_controller.update(
        {static_cast<float>(width), static_cast<float>(height)},
        filter->tracks,
        settings,
        static_cast<double>(seconds));

    filter->crop_x.store(crop.x, std::memory_order_relaxed);
    filter->crop_y.store(crop.y, std::memory_order_relaxed);
    filter->crop_width.store(crop.width, std::memory_order_relaxed);
    filter->crop_height.store(crop.height, std::memory_order_relaxed);

    set_runtime_tracking_stats(filter, filter->tracks.size(), crop);

    DebugOverlayData debug_data;
    {
        std::lock_guard<std::mutex> lock(filter->detection_result_mutex);
        debug_data.detections = filter->latest_detections;
    }
    debug_data.tracks = filter->tracks;
    debug_data.current_crop = crop;
    debug_data.target_crop = filter->crop_controller.target_crop();
    debug_data.dead_zone = settings.dead_zone;
    {
        std::lock_guard<std::mutex> lock(filter->debug_mutex);
        filter->debug_data = std::move(debug_data);
    }

    if (settings.debug_overlay && (filter->last_crop_log_ns == 0 || now - filter->last_crop_log_ns >= 2000000000ULL)) {
        const Rect target = filter->crop_controller.target_crop();
        blog(
            LOG_INFO,
            "[obs-auto-framing] crop current=(%.1f, %.1f %.1fx%.1f) target=(%.1f, %.1f %.1fx%.1f)",
            crop.x,
            crop.y,
            crop.width,
            crop.height,
            target.x,
            target.y,
            target.width,
            target.height);
        filter->last_crop_log_ns = now;
    }
}

void auto_framing_video_render(void* data, gs_effect_t* effect)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return;
    }

    AutoFramingSettings settings;
    {
        std::lock_guard<std::mutex> lock(filter->settings_mutex);
        settings = filter->settings;
    }

    uint32_t width = filter->source_width.load(std::memory_order_relaxed);
    uint32_t height = filter->source_height.load(std::memory_order_relaxed);

    obs_source_t* target = target_for(filter);
    if (target != nullptr) {
        const uint32_t render_width = obs_source_get_width(target);
        const uint32_t render_height = obs_source_get_height(target);
        if (render_width > 0 && render_height > 0) {
            width = render_width;
            height = render_height;
            filter->source_width.store(width, std::memory_order_relaxed);
            filter->source_height.store(height, std::memory_order_relaxed);
        }
    }

    Rect crop{
        filter->crop_x.load(std::memory_order_relaxed),
        filter->crop_y.load(std::memory_order_relaxed),
        filter->crop_width.load(std::memory_order_relaxed),
        filter->crop_height.load(std::memory_order_relaxed)};

    if (!crop.valid() && width > 0 && height > 0) {
        crop = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    }

    if (!filter->render_enabled) {
        obs_source_skip_video_filter(filter->source);
        return;
    }

    DebugOverlayData debug_data;
    if (settings.debug_overlay) {
        std::lock_guard<std::mutex> lock(filter->debug_mutex);
        debug_data = filter->debug_data;
    }

    filter->renderer.render(filter->source, crop, width, height, settings.debug_overlay, debug_data, effect);
}

struct obs_source_frame* auto_framing_filter_video(void* data, struct obs_source_frame* frame)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr || frame == nullptr) {
        return frame;
    }

    AutoFramingSettings settings;
    {
        std::lock_guard<std::mutex> lock(filter->settings_mutex);
        settings = filter->settings;
    }

    if (settings.detector_backend != DetectorBackend::OnnxRuntimeCpu) {
        return frame;
    }

    Frame captured_frame;
    if (!copy_source_frame_to_rgba(frame, captured_frame)) {
        const bool already_logged = filter->unsupported_frame_logged.exchange(true);
        if (!already_logged) {
            blog(LOG_WARNING, "[obs-auto-framing] unsupported source frame format for ONNX capture: %d", frame->format);
        }
        set_runtime_status(filter, RuntimeStatus::Error, "Unsupported source frame format for ONNX capture.");
        return frame;
    }

    {
        std::lock_guard<std::mutex> lock(filter->latest_frame_mutex);
        filter->latest_frame = std::move(captured_frame);
        filter->has_latest_frame = true;
    }

    const bool already_logged = filter->captured_frame_logged.exchange(true);
    if (!already_logged) {
        blog(LOG_INFO,
            "[obs-auto-framing] captured first RGBA frame for ONNX detection: %ux%u",
            frame->width,
            frame->height);
    }

    return frame;
}

uint32_t auto_framing_get_width(void* data)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return 0;
    }

    return std::max<uint32_t>(1, filter->source_width.load(std::memory_order_relaxed));
}

uint32_t auto_framing_get_height(void* data)
{
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return 0;
    }

    return std::max<uint32_t>(1, filter->source_height.load(std::memory_order_relaxed));
}

} // namespace

void register_auto_framing_filter()
{
    blog(LOG_INFO, "[obs-auto-framing] registering Auto Framing filter");

    struct obs_source_info info = {};
    info.id = "obs_auto_framing_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = auto_framing_get_name;
    info.create = auto_framing_create;
    info.destroy = auto_framing_destroy;
    info.get_defaults = auto_framing_defaults;
    info.get_properties = auto_framing_properties;
    info.update = auto_framing_update;
    info.video_tick = auto_framing_video_tick;
    info.filter_video = auto_framing_filter_video;
    info.video_render = auto_framing_video_render;
    info.get_width = auto_framing_get_width;
    info.get_height = auto_framing_get_height;

    obs_register_source(&info);
    blog(LOG_INFO, "[obs-auto-framing] Auto Framing filter registered");
}

} // namespace autoframing
