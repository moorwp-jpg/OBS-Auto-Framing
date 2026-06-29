#include "crop_controller.hpp"
#include "crop_renderer.hpp"
#include "detector.hpp"
#include "onnx_person_detector.hpp"
#include "settings.hpp"
#include "tracker.hpp"
#include "tracker/bytetrack_tracker.hpp"
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

#ifndef PLUGIN_RELEASE_CHANNEL
#define PLUGIN_RELEASE_CHANNEL ""
#endif

namespace autoframing {
namespace {

enum class RuntimeStatus {
    Running,
    NoFrameYet,
    DetectorUnavailable,
    ModelMissing,
    Error,
};

enum class TrackerRuntimeState {
    Detecting,
    Predicting,
    PredictionGuarded,
};

struct RuntimeInfo {
    RuntimeStatus status = RuntimeStatus::NoFrameYet;
    std::string status_detail = "Waiting for source video.";
    DetectorBackend detector_backend = DetectorBackend::Mock;
    DetectorModelQuality detector_model_quality = DetectorModelQuality::BalancedTiny;
    std::string model_path;
    bool model_loaded = false;
    TrackingAlgorithm tracking_algorithm = TrackingAlgorithm::ByteTrack;
    SubjectLockMode subject_lock_mode = SubjectLockMode::Off;
    std::vector<int> locked_track_ids;
    bool subject_lock_lost = false;
    size_t ignored_detection_count = 0;
    double last_inference_ms = 0.0;
    size_t last_detection_count = 0;
    uint64_t last_detection_timestamp_ns = 0;
    double detection_age_ms = 0.0;
    uint32_t detection_interval_ms = default_settings().detection_interval_ms;
    TrackerRuntimeState tracker_runtime_state = TrackerRuntimeState::Predicting;
    size_t active_track_count = 0;
    size_t lost_track_count = 0;
    Rect current_crop;
};

struct SubjectLockState {
    std::vector<int> locked_track_ids;
    bool locked_subject_lost = false;
    size_t ignored_detection_count = 0;
    bool auto_lock_suppressed_until_empty = false;
};

struct AutoFramingFilter {
    obs_source_t* source = nullptr;
    AutoFramingSettings settings = default_settings();
    std::mutex settings_mutex;

    std::unique_ptr<PersonDetector> detector;
    std::string detector_signature;
    std::mutex detector_mutex;
    std::mutex tracking_mutex;
    IouTracker iou_tracker;
    ByteTrackTracker bytetrack_tracker;
    CropController crop_controller;
    CropRenderer renderer;

    uint64_t last_submit_ns = 0;
    uint64_t last_detection_log_ns = 0;
    uint64_t last_detection_age_log_ns = 0;
    uint64_t last_processed_detection_timestamp_ns = 0;
    uint64_t last_crop_log_ns = 0;
    std::vector<PersonTrack> tracks;
    std::vector<PersonTrack> debug_tracks;
    SubjectLockState subject_lock;
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
    std::string last_applied_user_preset = "presenter_smooth";

    std::atomic<bool> unsupported_frame_logged{false};
    std::atomic<bool> captured_frame_logged{false};

    std::atomic<float> crop_x{0.0f};
    std::atomic<float> crop_y{0.0f};
    std::atomic<float> crop_width{0.0f};
    std::atomic<float> crop_height{0.0f};
    std::atomic<uint32_t> source_width{0};
    std::atomic<uint32_t> source_height{0};
};

void set_runtime_status(AutoFramingFilter* filter, RuntimeStatus status, std::string detail) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.status = status;
    filter->runtime.status_detail = std::move(detail);
}

void set_runtime_detector_state(AutoFramingFilter* filter, DetectorBackend backend, DetectorModelQuality model_quality,
                                std::string model_path, bool model_loaded, RuntimeStatus status, std::string detail) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.detector_backend = backend;
    filter->runtime.detector_model_quality = model_quality;
    filter->runtime.model_path = std::move(model_path);
    filter->runtime.model_loaded = model_loaded;
    filter->runtime.status = status;
    filter->runtime.status_detail = std::move(detail);
}

void set_runtime_subject_lock_state(AutoFramingFilter* filter, SubjectLockMode mode, std::vector<int> locked_track_ids,
                                    bool locked_subject_lost, size_t ignored_detection_count) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.subject_lock_mode = mode;
    filter->runtime.locked_track_ids = std::move(locked_track_ids);
    filter->runtime.subject_lock_lost = locked_subject_lost;
    filter->runtime.ignored_detection_count = ignored_detection_count;
}

void set_runtime_inference_stats(AutoFramingFilter* filter, double inference_ms, size_t detection_count) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.last_inference_ms = inference_ms;
    filter->runtime.last_detection_count = detection_count;
}

void set_runtime_detection_age(AutoFramingFilter* filter, uint64_t last_detection_timestamp_ns, double detection_age_ms,
                               uint32_t detection_interval_ms, TrackerRuntimeState tracker_runtime_state) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.last_detection_timestamp_ns = last_detection_timestamp_ns;
    filter->runtime.detection_age_ms = detection_age_ms;
    filter->runtime.detection_interval_ms = detection_interval_ms;
    filter->runtime.tracker_runtime_state = tracker_runtime_state;
}

void set_runtime_tracking_stats(AutoFramingFilter* filter, TrackingAlgorithm algorithm, size_t active_track_count,
                                size_t lost_track_count, const Rect& current_crop) {
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    filter->runtime.tracking_algorithm = algorithm;
    filter->runtime.active_track_count = active_track_count;
    filter->runtime.lost_track_count = lost_track_count;
    filter->runtime.current_crop = current_crop;
}

RuntimeInfo runtime_info_for(void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return {};
    }

    std::lock_guard<std::mutex> lock(filter->runtime_mutex);
    return filter->runtime;
}

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }

    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "FALSE") != 0;
}

const char* text(const char* key) {
    const char* translated = obs_module_text(key);
    return translated != nullptr ? translated : key;
}

const char* runtime_status_to_string(RuntimeStatus status) {
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

const char* detector_backend_display(DetectorBackend backend) {
    return backend == DetectorBackend::OnnxRuntimeCpu ? "ONNX Runtime CPU" : "Mock";
}

const char* detector_model_quality_display(DetectorModelQuality quality) {
    switch (quality) {
    case DetectorModelQuality::FastNano:
        return "Fast - YOLOX-Nano";
    case DetectorModelQuality::AccurateSmall:
        return "Accurate / Slower - YOLOX-S";
    case DetectorModelQuality::Custom:
        return "Custom ONNX";
    case DetectorModelQuality::BalancedTiny:
    default:
        return "Balanced - YOLOX-Tiny";
    }
}

const char* tracking_algorithm_display(TrackingAlgorithm algorithm) {
    return algorithm == TrackingAlgorithm::SimpleIou ? "Simple IoU" : "ByteTrack";
}

const char* tracking_algorithm_short_display(TrackingAlgorithm algorithm) {
    return algorithm == TrackingAlgorithm::SimpleIou ? "IOU" : "BT";
}

const char* tracker_runtime_state_display(TrackerRuntimeState state) {
    switch (state) {
    case TrackerRuntimeState::Detecting:
        return "using latest detector result";
    case TrackerRuntimeState::PredictionGuarded:
        return "prediction drift guard active";
    case TrackerRuntimeState::Predicting:
    default:
        return "predicting between detector results";
    }
}

float detector_score_floor_for(const AutoFramingSettings& settings) {
    if (settings.tracking_algorithm != TrackingAlgorithm::ByteTrack) {
        return static_cast<float>(settings.detection_confidence);
    }

    return static_cast<float>(settings.detector_score_floor);
}

struct UserPresetDefinition {
    const char* id;
    DetectorModelQuality detector_model_quality;
    TrackingMode tracking_mode;
    FramingPreset framing_preset;
    TrackingSensitivity tracking_sensitivity;
    double tracking_speed;
    double max_zoom;
    uint32_t detection_interval_ms;
    double detector_score_floor;
    double detection_confidence;
};

constexpr const char* default_user_preset_id = "presenter_smooth";

constexpr UserPresetDefinition user_presets[] = {
    {"presenter_smooth", DetectorModelQuality::BalancedTiny, TrackingMode::Presenter, FramingPreset::Headroom,
     TrackingSensitivity::Balanced, 0.32, 2.4, 150, 0.05, 0.30},
    {"presenter_fast", DetectorModelQuality::BalancedTiny, TrackingMode::Presenter, FramingPreset::Tight,
     TrackingSensitivity::Balanced, 0.62, 3.2, 80, 0.06, 0.30},
    {"group", DetectorModelQuality::BalancedTiny, TrackingMode::Group, FramingPreset::FullBody,
     TrackingSensitivity::Persistent, 0.35, 1.8, 150, 0.05, 0.30},
    {"accurate_slower", DetectorModelQuality::AccurateSmall, TrackingMode::Presenter, FramingPreset::Headroom,
     TrackingSensitivity::Balanced, 0.28, 2.4, 300, 0.05, 0.30},
    {"low_cpu", DetectorModelQuality::BalancedTiny, TrackingMode::Presenter, FramingPreset::Balanced,
     TrackingSensitivity::Conservative, 0.25, 2.0, 350, 0.08, 0.35},
};

const UserPresetDefinition& user_preset_for_id(const char* id) {
    for (const UserPresetDefinition& preset : user_presets) {
        if (id != nullptr && std::strcmp(id, preset.id) == 0) {
            return preset;
        }
    }
    return user_presets[0];
}

void apply_user_preset_to_obs_data(obs_data_t* settings, const UserPresetDefinition& preset) {
    if (settings == nullptr) {
        return;
    }

    obs_data_set_string(settings, setting_keys::user_preset, preset.id);
    obs_data_set_string(settings, setting_keys::detector_backend,
                        detector_backend_to_string(DetectorBackend::OnnxRuntimeCpu));
    obs_data_set_string(settings, setting_keys::detector_model_quality,
                        detector_model_quality_to_string(preset.detector_model_quality));
    obs_data_set_string(settings, setting_keys::tracking_algorithm,
                        tracking_algorithm_to_string(TrackingAlgorithm::ByteTrack));
    obs_data_set_string(settings, setting_keys::tracking_mode, tracking_mode_to_string(preset.tracking_mode));
    obs_data_set_string(settings, setting_keys::framing_preset, framing_preset_to_string(preset.framing_preset));
    obs_data_set_string(settings, setting_keys::tracking_sensitivity,
                        tracking_sensitivity_to_string(preset.tracking_sensitivity));
    obs_data_set_double(settings, setting_keys::tracking_speed, preset.tracking_speed);
    obs_data_set_double(settings, setting_keys::max_zoom, preset.max_zoom);
    obs_data_set_int(settings, setting_keys::detection_interval_ms, preset.detection_interval_ms);
    obs_data_set_double(settings, setting_keys::detector_score_floor, preset.detector_score_floor);
    obs_data_set_double(settings, setting_keys::detection_confidence, preset.detection_confidence);
    obs_data_set_double(settings, setting_keys::bytetrack_track_high_thresh, 0.0);
    obs_data_set_double(settings, setting_keys::bytetrack_track_low_thresh, 0.0);
    obs_data_set_double(settings, setting_keys::bytetrack_new_track_thresh, 0.0);
    obs_data_set_double(settings, setting_keys::bytetrack_match_thresh, 0.0);
    obs_data_set_int(settings, setting_keys::bytetrack_track_buffer_frames, 0);
}

std::vector<int> sorted_unique_ids(std::vector<int> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::string format_track_ids(std::vector<int> ids) {
    ids = sorted_unique_ids(std::move(ids));
    if (ids.empty()) {
        return "none";
    }

    std::string formatted;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            formatted += ", ";
        }
        formatted += std::to_string(ids[i]);
    }
    return formatted;
}

std::string format_subject_lock_status(const RuntimeInfo& runtime) {
    if (runtime.subject_lock_mode == SubjectLockMode::Off) {
        return "Subject lock: off";
    }

    if (runtime.locked_track_ids.empty()) {
        return "Subject lock: waiting for subject";
    }

    std::string status =
        runtime.subject_lock_lost ? "Subject lock: locked subject lost" : "Subject lock: locked to track ID(s)";
    status += " ";
    status += format_track_ids(runtime.locked_track_ids);
    if (runtime.ignored_detection_count > 0) {
        status += " (ignored detections: ";
        status += std::to_string(runtime.ignored_detection_count);
        status += ")";
    }
    return status;
}

std::string format_rect(const Rect& rect) {
    if (!rect.valid()) {
        return "n/a";
    }

    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "x=%.1f y=%.1f w=%.1f h=%.1f", rect.x, rect.y, rect.width, rect.height);
    return buffer;
}

std::string plugin_version_display() {
    std::string version = PLUGIN_VERSION;
    const char* release_channel = PLUGIN_RELEASE_CHANNEL;
    if (release_channel != nullptr && release_channel[0] != '\0') {
        version += " ";
        version += release_channel;
    }
    return version;
}

struct RuntimeStatusText {
    std::string plugin_version;
    std::string status;
    std::string backend;
    std::string model_quality;
    std::string model_loaded;
    std::string model_path;
    std::string tracker;
    std::string subject_lock;
    std::string inference;
    std::string detections;
    std::string detection_age;
    std::string tracker_state;
    std::string performance;
    std::string tracks;
    std::string crop;
};

double stale_detection_age_threshold_ms(uint32_t detection_interval_ms) {
    return std::max(500.0, static_cast<double>(detection_interval_ms) * 2.0);
}

std::string format_performance_guidance(const RuntimeInfo& runtime) {
    const double slow_inference_threshold_ms =
        std::max(200.0, static_cast<double>(runtime.detection_interval_ms) * 1.25);
    if (runtime.detector_model_quality == DetectorModelQuality::AccurateSmall &&
        runtime.last_inference_ms > slow_inference_threshold_ms) {
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "Performance: YOLOX-S is more accurate but slower on CPU (last inference %.0f ms). Use "
                      "YOLOX-Tiny or increase Detection Interval.",
                      runtime.last_inference_ms);
        return buffer;
    }

    if (runtime.last_detection_timestamp_ns != 0 &&
        runtime.detection_age_ms > stale_detection_age_threshold_ms(runtime.detection_interval_ms)) {
        return "Performance: detector results are stale; tracker prediction is carrying the crop between detections.";
    }

    return "Performance: OK";
}

RuntimeStatusText format_runtime_status_text(const RuntimeInfo& runtime) {
    RuntimeStatusText text;
    text.plugin_version = std::string("Plugin version: ") + plugin_version_display();
    text.status = std::string("Status: ") + runtime_status_to_string(runtime.status) + " - " + runtime.status_detail;
    text.backend = std::string("Detector backend: ") + detector_backend_display(runtime.detector_backend);
    text.model_quality =
        std::string("Detection model: ") + detector_model_quality_display(runtime.detector_model_quality);
    text.model_loaded = std::string("Model loaded: ") + (runtime.model_loaded ? "yes" : "no");
    text.model_path = std::string("Model path: ") + (runtime.model_path.empty() ? "n/a" : runtime.model_path);
    text.tracker = std::string("Tracker: ") + tracking_algorithm_display(runtime.tracking_algorithm);
    text.subject_lock = format_subject_lock_status(runtime);

    char inference_buffer[128] = {};
    std::snprintf(inference_buffer, sizeof(inference_buffer), "Last inference: %.2f ms", runtime.last_inference_ms);
    text.inference = inference_buffer;

    text.detections = std::string("Last detections: ") + std::to_string(runtime.last_detection_count);

    if (runtime.last_detection_timestamp_ns == 0) {
        text.detection_age = "Detection age: n/a";
    } else {
        char detection_age_buffer[128] = {};
        std::snprintf(detection_age_buffer, sizeof(detection_age_buffer),
                      "Detection age: %.0f ms (target interval: %u ms)", runtime.detection_age_ms,
                      runtime.detection_interval_ms);
        text.detection_age = detection_age_buffer;
    }
    text.tracker_state =
        std::string("Tracker prediction: ") + tracker_runtime_state_display(runtime.tracker_runtime_state);
    text.performance = format_performance_guidance(runtime);

    text.tracks = std::string("Tracks: active=") + std::to_string(runtime.active_track_count) +
                  " lost=" + std::to_string(runtime.lost_track_count);
    text.crop = std::string("Current crop: ") + format_rect(runtime.current_crop);
    return text;
}

void apply_runtime_status_text(obs_properties_t* props, const RuntimeInfo& runtime) {
    if (props == nullptr) {
        return;
    }

    const RuntimeStatusText runtime_text = format_runtime_status_text(runtime);

    obs_property_t* status_prop = obs_properties_get(props, "runtime_status");
    if (status_prop != nullptr) {
        obs_property_set_description(status_prop, runtime_text.status.c_str());
        obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_NORMAL);
        if (runtime.status == RuntimeStatus::Error || runtime.status == RuntimeStatus::DetectorUnavailable ||
            runtime.status == RuntimeStatus::ModelMissing) {
            obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_ERROR);
        } else if (runtime.status == RuntimeStatus::NoFrameYet) {
            obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_WARNING);
        }
    }

    const auto set_description = [props](const char* name, const std::string& description) {
        obs_property_t* property = obs_properties_get(props, name);
        if (property != nullptr) {
            obs_property_set_description(property, description.c_str());
        }
    };

    set_description("runtime_backend", runtime_text.backend);
    set_description("runtime_plugin_version", runtime_text.plugin_version);
    set_description("runtime_model_quality", runtime_text.model_quality);
    set_description("runtime_model_loaded", runtime_text.model_loaded);
    set_description("runtime_model_path", runtime_text.model_path);
    set_description("runtime_tracker", runtime_text.tracker);
    set_description("runtime_subject_lock", runtime_text.subject_lock);
    set_description("runtime_inference", runtime_text.inference);
    set_description("runtime_detections", runtime_text.detections);
    set_description("runtime_detection_age", runtime_text.detection_age);
    set_description("runtime_tracker_state", runtime_text.tracker_state);
    set_description("runtime_performance", runtime_text.performance);
    set_description("runtime_tracks", runtime_text.tracks);
    set_description("runtime_crop", runtime_text.crop);

    obs_property_t* performance_prop = obs_properties_get(props, "runtime_performance");
    if (performance_prop != nullptr) {
        const bool warn = runtime_text.performance != "Performance: OK";
        obs_property_text_set_info_type(performance_prop, warn ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
    }
}

bool refresh_runtime_status_clicked(obs_properties_t* props, obs_property_t*, void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    apply_runtime_status_text(props, runtime_info_for(filter));
    return true;
}

bool user_preset_modified(void* data, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    if (settings == nullptr) {
        return false;
    }

    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    const char* selected_id = obs_data_get_string(settings, setting_keys::user_preset);
    if (selected_id == nullptr || selected_id[0] == '\0') {
        selected_id = default_user_preset_id;
    }

    if (filter != nullptr && filter->last_applied_user_preset == selected_id) {
        return false;
    }

    const UserPresetDefinition& preset = user_preset_for_id(selected_id);
    apply_user_preset_to_obs_data(settings, preset);

    if (filter != nullptr) {
        filter->last_applied_user_preset = preset.id;
    }

    return true;
}

AutoFramingSettings load_settings(obs_data_t* data) {
    AutoFramingSettings settings = default_settings();
    settings.tracking_speed = obs_data_get_double(data, setting_keys::tracking_speed);
    settings.tracking_algorithm =
        tracking_algorithm_from_string(obs_data_get_string(data, setting_keys::tracking_algorithm));
    settings.tracking_sensitivity =
        tracking_sensitivity_from_string(obs_data_get_string(data, setting_keys::tracking_sensitivity));
    settings.max_zoom = obs_data_get_double(data, setting_keys::max_zoom);
    settings.framing_preset = framing_preset_from_string(obs_data_get_string(data, setting_keys::framing_preset));
    settings.tracking_mode = tracking_mode_from_string(obs_data_get_string(data, setting_keys::tracking_mode));
    settings.detection_interval_ms = static_cast<uint32_t>(obs_data_get_int(data, setting_keys::detection_interval_ms));
    settings.dead_zone = obs_data_get_double(data, setting_keys::dead_zone);
    settings.debug_overlay = obs_data_get_bool(data, setting_keys::debug_overlay);
    settings.use_mock_detector = obs_data_get_bool(data, setting_keys::use_mock_detector);
    settings.detector_backend = detector_backend_from_string(obs_data_get_string(data, setting_keys::detector_backend));
    const bool has_model_quality = obs_data_has_user_value(data, setting_keys::detector_model_quality);
    settings.detector_model_quality =
        detector_model_quality_from_string(obs_data_get_string(data, setting_keys::detector_model_quality));
    settings.model_path = obs_data_get_string(data, setting_keys::model_path);
    if (!has_model_quality && !settings.model_path.empty()) {
        settings.detector_model_quality = DetectorModelQuality::Custom;
    }
    settings.detection_confidence = obs_data_get_double(data, setting_keys::detection_confidence);
    settings.detector_score_floor = obs_data_get_double(data, setting_keys::detector_score_floor);
    settings.nms_threshold = obs_data_get_double(data, setting_keys::nms_threshold);
    settings.bytetrack_track_high_thresh = obs_data_get_double(data, setting_keys::bytetrack_track_high_thresh);
    settings.bytetrack_track_low_thresh = obs_data_get_double(data, setting_keys::bytetrack_track_low_thresh);
    settings.bytetrack_new_track_thresh = obs_data_get_double(data, setting_keys::bytetrack_new_track_thresh);
    settings.bytetrack_match_thresh = obs_data_get_double(data, setting_keys::bytetrack_match_thresh);
    settings.bytetrack_track_buffer_frames =
        static_cast<uint32_t>(obs_data_get_int(data, setting_keys::bytetrack_track_buffer_frames));
    settings.subject_lock_mode =
        subject_lock_mode_from_string(obs_data_get_string(data, setting_keys::subject_lock_mode));
    return sanitize_settings(settings);
}

bool path_is_absolute(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }

    return path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

bool file_exists(const std::string& path) { return !path.empty() && os_file_exists(path.c_str()); }

struct ModelResolution {
    std::string path;
    std::string detail;
    bool found = false;
};

std::string join_model_path(const std::string& directory, const char* filename) {
    if (directory.empty() || filename == nullptr || filename[0] == '\0') {
        return {};
    }

    const char last = directory.back();
    if (last == '/' || last == '\\') {
        return directory + filename;
    }
    return directory + "/" + filename;
}

ModelResolution resolve_custom_model_path(const std::string& configured_path, bool log_configured_missing) {
    if (configured_path.empty()) {
        return {{}, "Custom ONNX model path is empty.", false};
    }

    if (path_is_absolute(configured_path) && file_exists(configured_path)) {
        return {configured_path, "Using custom ONNX model path.", true};
    }

    if (!path_is_absolute(configured_path)) {
        char* module_relative_path = obs_module_file(configured_path.c_str());
        if (module_relative_path != nullptr) {
            std::string path = module_relative_path;
            bfree(module_relative_path);
            if (file_exists(path)) {
                return {path, "Using custom ONNX model path relative to the plugin data directory.", true};
            }
        }

        if (file_exists(configured_path)) {
            return {configured_path, "Using custom ONNX model path relative to OBS current working directory.", true};
        }
    }

    if (log_configured_missing) {
        blog(LOG_WARNING, "[obs-auto-framing] configured ONNX model path does not exist: %s", configured_path.c_str());
    }
    return {{}, "Custom ONNX model path was not found.", false};
}

ModelResolution resolve_model_path(const AutoFramingSettings& settings, bool log_configured_missing = true) {
    if (settings.detector_model_quality == DetectorModelQuality::Custom) {
        return resolve_custom_model_path(settings.model_path, log_configured_missing);
    }

    const char* filename = detector_model_filename(settings.detector_model_quality);
    const std::string module_relative = std::string("models/") + filename;
    char* module_model_path = obs_module_file(module_relative.c_str());
    if (module_model_path != nullptr) {
        std::string path = module_model_path;
        bfree(module_model_path);
        if (file_exists(path)) {
            return {path, "Using bundled model from OBS plugin data directory.", true};
        }
    }

#ifdef DEFAULT_YOLOX_MODEL_DIR
    const std::string development_model_path = join_model_path(DEFAULT_YOLOX_MODEL_DIR, filename);
    if (file_exists(development_model_path)) {
        return {development_model_path, "Using development model path.", true};
    }
#endif

    return {{},
            std::string(detector_model_quality_display(settings.detector_model_quality)) + " model file was not found.",
            false};
}

std::string signature_for(const AutoFramingSettings& settings) {
    std::string signature = detector_backend_to_string(settings.detector_backend);
    if (settings.detector_backend == DetectorBackend::OnnxRuntimeCpu) {
        signature += "|";
        signature += detector_model_quality_to_string(settings.detector_model_quality);
        signature += "|";
        signature += resolve_model_path(settings, false).path;
        signature += "|";
        signature += std::to_string(detector_score_floor_for(settings));
        signature += "|";
        signature += std::to_string(settings.nms_threshold);
    }
    return signature;
}

bool detector_settings_changed(const AutoFramingSettings& previous, const AutoFramingSettings& current) {
    return previous.detector_backend != current.detector_backend ||
           previous.tracking_algorithm != current.tracking_algorithm ||
           previous.detector_model_quality != current.detector_model_quality ||
           previous.model_path != current.model_path || previous.detection_confidence != current.detection_confidence ||
           previous.detector_score_floor != current.detector_score_floor ||
           previous.nms_threshold != current.nms_threshold;
}

bool tracking_state_settings_changed(const AutoFramingSettings& previous, const AutoFramingSettings& current) {
    return previous.tracking_algorithm != current.tracking_algorithm ||
           previous.tracking_sensitivity != current.tracking_sensitivity ||
           previous.bytetrack_track_high_thresh != current.bytetrack_track_high_thresh ||
           previous.bytetrack_track_low_thresh != current.bytetrack_track_low_thresh ||
           previous.bytetrack_new_track_thresh != current.bytetrack_new_track_thresh ||
           previous.bytetrack_match_thresh != current.bytetrack_match_thresh ||
           previous.bytetrack_track_buffer_frames != current.bytetrack_track_buffer_frames ||
           previous.tracking_mode != current.tracking_mode || previous.framing_preset != current.framing_preset ||
           previous.max_zoom != current.max_zoom || previous.tracking_speed != current.tracking_speed ||
           previous.dead_zone != current.dead_zone;
}

void reset_tracking_state(AutoFramingFilter* filter, bool reset_detector) {
    if (filter == nullptr) {
        return;
    }

    if (reset_detector) {
        std::lock_guard<std::mutex> detector_lock(filter->detector_mutex);
        filter->detector.reset();
        filter->detector_signature.clear();
        filter->detector_available.store(false, std::memory_order_relaxed);
        filter->captured_frame_logged.store(false, std::memory_order_relaxed);
        filter->unsupported_frame_logged.store(false, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> worker_lock(filter->worker_mutex);
        filter->pending_frame = {};
        filter->pending_settings = {};
        filter->pending_frame_available = false;
        filter->last_submit_ns = 0;
        filter->last_detection_age_log_ns = 0;
        filter->last_processed_detection_timestamp_ns = 0;
    }

    {
        std::lock_guard<std::mutex> frame_lock(filter->latest_frame_mutex);
        if (reset_detector) {
            filter->latest_frame = {};
            filter->has_latest_frame = false;
        }
    }

    {
        std::lock_guard<std::mutex> detection_lock(filter->detection_result_mutex);
        filter->latest_detections.clear();
        filter->latest_detection_timestamp_ns = 0;
        filter->latest_detection_available = false;
    }

    {
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        filter->iou_tracker.reset();
        filter->bytetrack_tracker.reset();
        filter->tracks.clear();
        filter->debug_tracks.clear();
        filter->subject_lock = {};
        filter->crop_controller.reset();
        filter->crop_x.store(0.0f, std::memory_order_relaxed);
        filter->crop_y.store(0.0f, std::memory_order_relaxed);
        filter->crop_width.store(0.0f, std::memory_order_relaxed);
        filter->crop_height.store(0.0f, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> debug_lock(filter->debug_mutex);
        filter->debug_data = {};
    }

    {
        std::lock_guard<std::mutex> runtime_lock(filter->runtime_mutex);
        filter->runtime.last_inference_ms = 0.0;
        filter->runtime.last_detection_count = 0;
        filter->runtime.last_detection_timestamp_ns = 0;
        filter->runtime.detection_age_ms = 0.0;
        filter->runtime.detection_interval_ms = filter->settings.detection_interval_ms;
        filter->runtime.tracker_runtime_state = TrackerRuntimeState::Predicting;
        filter->runtime.active_track_count = 0;
        filter->runtime.lost_track_count = 0;
        filter->runtime.detector_model_quality = filter->settings.detector_model_quality;
        filter->runtime.tracking_algorithm = filter->settings.tracking_algorithm;
        filter->runtime.subject_lock_mode = filter->settings.subject_lock_mode;
        filter->runtime.locked_track_ids.clear();
        filter->runtime.subject_lock_lost = false;
        filter->runtime.ignored_detection_count = 0;
        filter->runtime.current_crop = {};
        if (reset_detector) {
            filter->runtime.model_loaded = false;
            filter->runtime.model_path.clear();
        }
        filter->runtime.status = RuntimeStatus::NoFrameYet;
        filter->runtime.status_detail = reset_detector ? "Detector settings changed; waiting for a new frame."
                                                       : "Tracking settings changed; waiting for a new frame.";
    }
}

std::unique_ptr<PersonDetector> make_detector(AutoFramingFilter* filter, const AutoFramingSettings& settings) {
    if (settings.detector_backend == DetectorBackend::Mock) {
        auto detector = std::make_unique<MockPersonDetector>();
        detector->set_enabled(true);
        blog(LOG_INFO, "[obs-auto-framing] detector backend: mock");
        if (filter != nullptr) {
            filter->detector_available.store(true, std::memory_order_relaxed);
        }
        set_runtime_detector_state(filter, DetectorBackend::Mock, settings.detector_model_quality, {}, false,
                                   RuntimeStatus::Running, "Mock detector is running.");
        return detector;
    }

    const ModelResolution model = resolve_model_path(settings);
    blog(LOG_INFO, "[obs-auto-framing] detector backend: ONNX Runtime CPU");
    blog(LOG_INFO, "[obs-auto-framing] resolved ONNX model path: %s",
         model.path.empty() ? "(missing)" : model.path.c_str());

    if (!model.found) {
        if (filter != nullptr) {
            filter->detector_available.store(false, std::memory_order_relaxed);
        }
        blog(LOG_ERROR, "[obs-auto-framing] ONNX detector unavailable: %s", model.detail.c_str());
        set_runtime_detector_state(filter, DetectorBackend::OnnxRuntimeCpu, settings.detector_model_quality, {}, false,
                                   RuntimeStatus::ModelMissing, model.detail);
        return std::make_unique<NullPersonDetector>();
    }

    OnnxPersonDetectorConfig config;
    config.model_path = model.path;
    config.score_floor = detector_score_floor_for(settings);
    config.nms_threshold = static_cast<float>(settings.nms_threshold);
    config.min_person_class_score = 0.25f;
    config.min_person_class_margin = 0.10f;
    config.require_person_best_class = false;

    auto detector = std::make_unique<OnnxPersonDetector>(config);
    if (detector->ready()) {
        if (filter != nullptr) {
            filter->detector_available.store(true, std::memory_order_relaxed);
        }
        set_runtime_detector_state(filter, DetectorBackend::OnnxRuntimeCpu, settings.detector_model_quality, model.path,
                                   true, RuntimeStatus::Running, "ONNX Runtime CPU detector is running.");
        return detector;
    }

    blog(LOG_ERROR,
         "[obs-auto-framing] ONNX detector unavailable; detections will be empty until settings are fixed: %s",
         detector->error().c_str());
    if (filter != nullptr) {
        filter->detector_available.store(false, std::memory_order_relaxed);
    }
    set_runtime_detector_state(filter, DetectorBackend::OnnxRuntimeCpu, settings.detector_model_quality, model.path,
                               false, RuntimeStatus::DetectorUnavailable, detector->error());
    return std::make_unique<NullPersonDetector>();
}

void detection_worker_loop(AutoFramingFilter* filter) {
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

        const uint64_t inference_start_ns = os_gettime_ns();
        std::vector<Detection> detections;
        {
            std::lock_guard<std::mutex> detector_lock(filter->detector_mutex);
            const std::string signature = signature_for(settings);
            if (filter->detector == nullptr || filter->detector_signature != signature) {
                filter->detector = make_detector(filter, settings);
                filter->detector_signature = signature;
            }
            detections = filter->detector != nullptr ? filter->detector->detect(frame) : std::vector<Detection>{};
        }
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
            blog(LOG_INFO, "[obs-auto-framing] detections=%zu inference=%.2f ms backend=%s", detection_count,
                 inference_ms, detector_backend_to_string(settings.detector_backend));
            filter->last_detection_log_ns = now;
        }
    }
}

std::vector<PersonTrack> update_selected_tracker(AutoFramingFilter* filter, const AutoFramingSettings& settings,
                                                 const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                                 const TrackerUpdateOptions& options) {
    if (settings.tracking_algorithm == TrackingAlgorithm::SimpleIou) {
        return filter->iou_tracker.update(detections, timestamp_ns, options);
    }

    filter->bytetrack_tracker.set_config(bytetrack_config_from_settings(settings));
    return filter->bytetrack_tracker.update(detections, timestamp_ns, options);
}

std::vector<PersonTrack> predict_selected_tracker(AutoFramingFilter* filter, const AutoFramingSettings& settings,
                                                  uint64_t timestamp_ns, const TrackerUpdateOptions& options) {
    if (settings.tracking_algorithm == TrackingAlgorithm::SimpleIou) {
        return filter->iou_tracker.predict(timestamp_ns, options);
    }

    filter->bytetrack_tracker.set_config(bytetrack_config_from_settings(settings));
    return filter->bytetrack_tracker.predict(timestamp_ns, options);
}

size_t selected_tracker_lost_count(const AutoFramingFilter* filter, TrackingAlgorithm algorithm) {
    return algorithm == TrackingAlgorithm::SimpleIou ? filter->iou_tracker.lost_track_count()
                                                     : filter->bytetrack_tracker.lost_track_count();
}

std::vector<PersonTrack> selected_tracker_debug_tracks(const AutoFramingFilter* filter, TrackingAlgorithm algorithm) {
    return algorithm == TrackingAlgorithm::SimpleIou ? filter->iou_tracker.debug_tracks()
                                                     : filter->bytetrack_tracker.debug_tracks();
}

bool track_id_is_locked(int id, const std::vector<int>& locked_ids) {
    return locked_ids.empty() || std::find(locked_ids.begin(), locked_ids.end(), id) != locked_ids.end();
}

float presenter_track_score(const PersonTrack& track) {
    return track.confidence * 0.65f + track.box.area() * 0.000001f;
}

std::vector<int> lock_ids_for_tracks(const std::vector<PersonTrack>& tracks, TrackingMode mode) {
    if (tracks.empty()) {
        return {};
    }

    std::vector<int> ids;
    if (mode == TrackingMode::Group) {
        ids.reserve(tracks.size());
        for (const PersonTrack& track : tracks) {
            ids.push_back(track.id);
        }
        return sorted_unique_ids(std::move(ids));
    }

    const PersonTrack* best = &tracks.front();
    float best_score = presenter_track_score(*best);
    for (const PersonTrack& track : tracks) {
        const float score = presenter_track_score(track);
        if (score > best_score) {
            best = &track;
            best_score = score;
        }
    }
    return {best->id};
}

std::vector<PersonTrack> filter_tracks_to_locked_ids(const std::vector<PersonTrack>& tracks,
                                                     const std::vector<int>& locked_track_ids) {
    if (locked_track_ids.empty()) {
        return tracks;
    }

    std::vector<PersonTrack> filtered;
    filtered.reserve(tracks.size());
    for (const PersonTrack& track : tracks) {
        if (track_id_is_locked(track.id, locked_track_ids)) {
            filtered.push_back(track);
        }
    }
    return filtered;
}

size_t count_ignored_detections_for_lock(const std::vector<Detection>& detections,
                                         const std::vector<PersonTrack>& locked_active_tracks) {
    size_t ignored = 0;
    for (const Detection& detection : detections) {
        bool matched_locked_track = false;
        for (const PersonTrack& track : locked_active_tracks) {
            if (intersection_over_union(detection.box, track.box) > 0.10f) {
                matched_locked_track = true;
                break;
            }
        }
        if (!matched_locked_track) {
            ++ignored;
        }
    }
    return ignored;
}

TrackerUpdateOptions tracker_options_for_subject_lock(const AutoFramingSettings& settings,
                                                      const SubjectLockState& subject_lock) {
    TrackerUpdateOptions options;
    if (settings.subject_lock_mode != SubjectLockMode::Off && !subject_lock.locked_track_ids.empty()) {
        options.allow_new_tracks = false;
        options.locked_track_ids = subject_lock.locked_track_ids;
    }
    return options;
}

void reconcile_subject_lock_after_tracker_update(const AutoFramingSettings& settings,
                                                 const std::vector<Detection>& detections,
                                                 SubjectLockState& subject_lock,
                                                 std::vector<PersonTrack>& active_tracks) {
    if (settings.subject_lock_mode == SubjectLockMode::Off) {
        subject_lock = {};
        return;
    }

    if (settings.subject_lock_mode == SubjectLockMode::AutoLockFirstSubject) {
        if (subject_lock.auto_lock_suppressed_until_empty) {
            if (active_tracks.empty()) {
                subject_lock.auto_lock_suppressed_until_empty = false;
            }
        } else if (subject_lock.locked_track_ids.empty() && !active_tracks.empty()) {
            subject_lock.locked_track_ids = lock_ids_for_tracks(active_tracks, settings.tracking_mode);
        }
    }

    if (subject_lock.locked_track_ids.empty()) {
        subject_lock.locked_subject_lost = false;
        subject_lock.ignored_detection_count = 0;
        return;
    }

    active_tracks = filter_tracks_to_locked_ids(active_tracks, subject_lock.locked_track_ids);
    subject_lock.locked_subject_lost = active_tracks.empty();
    subject_lock.ignored_detection_count = count_ignored_detections_for_lock(detections, active_tracks);
}

void reconcile_subject_lock_after_tracker_prediction(const AutoFramingSettings& settings,
                                                     SubjectLockState& subject_lock,
                                                     std::vector<PersonTrack>& active_tracks) {
    if (settings.subject_lock_mode == SubjectLockMode::Off) {
        subject_lock = {};
        return;
    }

    if (settings.subject_lock_mode == SubjectLockMode::AutoLockFirstSubject) {
        if (subject_lock.auto_lock_suppressed_until_empty) {
            if (active_tracks.empty()) {
                subject_lock.auto_lock_suppressed_until_empty = false;
            }
        } else if (subject_lock.locked_track_ids.empty() && !active_tracks.empty()) {
            subject_lock.locked_track_ids = lock_ids_for_tracks(active_tracks, settings.tracking_mode);
        }
    }

    if (subject_lock.locked_track_ids.empty()) {
        subject_lock.locked_subject_lost = false;
        return;
    }

    active_tracks = filter_tracks_to_locked_ids(active_tracks, subject_lock.locked_track_ids);
    subject_lock.locked_subject_lost = active_tracks.empty();
}

void persist_subject_lock_mode(AutoFramingFilter* filter, SubjectLockMode mode) {
    if (filter == nullptr || filter->source == nullptr) {
        return;
    }

    obs_data_t* source_settings = obs_source_get_settings(filter->source);
    if (source_settings == nullptr) {
        return;
    }

    obs_data_set_string(source_settings, setting_keys::subject_lock_mode, subject_lock_mode_to_string(mode));
    obs_source_update(filter->source, source_settings);
    obs_data_release(source_settings);
}

bool lock_current_subject_clicked(obs_properties_t* props, obs_property_t*, void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return true;
    }

    AutoFramingSettings settings;
    bool switched_to_manual = false;
    {
        std::lock_guard<std::mutex> settings_lock(filter->settings_mutex);
        if (filter->settings.subject_lock_mode == SubjectLockMode::Off) {
            filter->settings.subject_lock_mode = SubjectLockMode::Manual;
            switched_to_manual = true;
        }
        settings = filter->settings;
    }

    if (switched_to_manual) {
        persist_subject_lock_mode(filter, SubjectLockMode::Manual);
    }

    std::vector<int> locked_ids;
    {
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        locked_ids = lock_ids_for_tracks(filter->tracks, settings.tracking_mode);
        filter->subject_lock.locked_track_ids = locked_ids;
        filter->subject_lock.locked_subject_lost = false;
        filter->subject_lock.ignored_detection_count = 0;
        filter->subject_lock.auto_lock_suppressed_until_empty = false;
    }

    blog(LOG_INFO, "[obs-auto-framing] subject lock requested; mode=%s ids=%s",
         subject_lock_mode_to_string(settings.subject_lock_mode), format_track_ids(locked_ids).c_str());
    set_runtime_subject_lock_state(filter, settings.subject_lock_mode, locked_ids, false, 0);
    apply_runtime_status_text(props, runtime_info_for(filter));
    return true;
}

bool unlock_subject_clicked(obs_properties_t* props, obs_property_t*, void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return true;
    }

    AutoFramingSettings settings;
    {
        std::lock_guard<std::mutex> settings_lock(filter->settings_mutex);
        settings = filter->settings;
    }

    {
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        filter->subject_lock = {};
        filter->subject_lock.auto_lock_suppressed_until_empty =
            settings.subject_lock_mode == SubjectLockMode::AutoLockFirstSubject;
    }

    blog(LOG_INFO, "[obs-auto-framing] subject lock cleared");
    set_runtime_subject_lock_state(filter, settings.subject_lock_mode, {}, false, 0);
    apply_runtime_status_text(props, runtime_info_for(filter));
    return true;
}

std::string source_frame_linesizes_to_string(const obs_source_frame* frame) {
    if (frame == nullptr) {
        return "n/a";
    }

    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "[%u, %u, %u, %u]", frame->linesize[0], frame->linesize[1],
                  frame->linesize[2], frame->linesize[3]);
    return buffer;
}

std::string unsupported_source_frame_detail(enum video_format format) {
    std::string detail = "Unsupported source frame format: ";
    detail += get_video_format_name(format);
    detail += ". Try NV12, YUY2, I420, or BGRA, or disable HDR/10-bit output. ";
    detail += "Model initialization is blocked because no supported frame was captured.";
    return detail;
}

bool copy_source_frame_to_rgba(const struct obs_source_frame* source_frame, Frame& frame) {
    if (source_frame == nullptr || source_frame->width == 0 || source_frame->height == 0 ||
        source_frame->data[0] == nullptr) {
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
            std::memcpy(dst, source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0],
                        frame.rgba_stride);
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
        case VIDEO_FORMAT_I422: {
            if (source_frame->data[1] == nullptr || source_frame->data[2] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* u_plane = source_frame->data[1] + static_cast<size_t>(y) * source_frame->linesize[1];
            const uint8_t* v_plane = source_frame->data[2] + static_cast<size_t>(y) * source_frame->linesize[2];
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint32_t chroma_x = i422_chroma_index(x);
                yuv_to_rgba(y_plane[x], u_plane[chroma_x], v_plane[chroma_x], dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_I444: {
            if (source_frame->data[1] == nullptr || source_frame->data[2] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* u_plane = source_frame->data[1] + static_cast<size_t>(y) * source_frame->linesize[1];
            const uint8_t* v_plane = source_frame->data[2] + static_cast<size_t>(y) * source_frame->linesize[2];
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint32_t chroma_x = i444_chroma_index(x);
                yuv_to_rgba(y_plane[x], u_plane[chroma_x], v_plane[chroma_x], dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_AYUV: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; ++x) {
                const AyuvPixel pixel = unpack_ayuv_pixel(src + x * 4);
                yuv_to_rgba(pixel.y, pixel.u, pixel.v, dst + x * 4);
                dst[x * 4 + 3] = pixel.a;
            }
            break;
        }
        case VIDEO_FORMAT_I010: {
            if (source_frame->data[1] == nullptr || source_frame->data[2] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* u_plane = source_frame->data[1] + static_cast<size_t>(y / 2) * source_frame->linesize[1];
            const uint8_t* v_plane = source_frame->data[2] + static_cast<size_t>(y / 2) * source_frame->linesize[2];
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint32_t chroma_x = i422_chroma_index(x);
                const uint8_t y8 = i010_sample_to_u8(read_le16(y_plane + static_cast<size_t>(x) * 2));
                const uint8_t u8 = i010_sample_to_u8(read_le16(u_plane + static_cast<size_t>(chroma_x) * 2));
                const uint8_t v8 = i010_sample_to_u8(read_le16(v_plane + static_cast<size_t>(chroma_x) * 2));
                yuv_to_rgba(y8, u8, v8, dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_P010: {
            if (source_frame->data[1] == nullptr) {
                return false;
            }
            const uint8_t* y_plane = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            const uint8_t* uv_plane = source_frame->data[1] + static_cast<size_t>(y / 2) * source_frame->linesize[1];
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint32_t chroma_x = i422_chroma_index(x);
                const uint8_t y8 = p010_sample_to_u8(read_le16(y_plane + static_cast<size_t>(x) * 2));
                const uint8_t u8 = p010_sample_to_u8(read_le16(uv_plane + static_cast<size_t>(chroma_x) * 4));
                const uint8_t v8 =
                    p010_sample_to_u8(read_le16(uv_plane + static_cast<size_t>(chroma_x) * 4 + 2));
                yuv_to_rgba(y8, u8, v8, dst + x * 4);
            }
            break;
        }
        case VIDEO_FORMAT_YUY2: {
            const uint8_t* src = source_frame->data[0] + static_cast<size_t>(y) * source_frame->linesize[0];
            for (uint32_t x = 0; x < frame.width; x += 2) {
                const bool has_second_pixel = x + 1 < frame.width;
                const PackedYuvGroup group =
                    decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Yuy2, has_second_pixel);
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
                const PackedYuvGroup group =
                    decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Uyvy, has_second_pixel);
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
                const PackedYuvGroup group =
                    decode_packed_yuv_group(src + x * 2, PackedYuvFormat::Yvyu, has_second_pixel);
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

obs_source_t* target_for(AutoFramingFilter* filter) {
    return filter != nullptr && filter->source != nullptr ? obs_filter_get_target(filter->source) : nullptr;
}

uint32_t target_width(AutoFramingFilter* filter) {
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

uint32_t target_height(AutoFramingFilter* filter) {
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

const char* auto_framing_get_name(void*) { return text("AutoFramingFilter"); }

void auto_framing_defaults(obs_data_t* data) {
    const AutoFramingSettings settings = default_settings();
    obs_data_set_default_string(data, setting_keys::user_preset, default_user_preset_id);
    obs_data_set_default_double(data, setting_keys::tracking_speed, settings.tracking_speed);
    obs_data_set_default_string(data, setting_keys::tracking_algorithm,
                                tracking_algorithm_to_string(settings.tracking_algorithm));
    obs_data_set_default_string(data, setting_keys::tracking_sensitivity,
                                tracking_sensitivity_to_string(settings.tracking_sensitivity));
    obs_data_set_default_double(data, setting_keys::max_zoom, settings.max_zoom);
    obs_data_set_default_string(data, setting_keys::framing_preset, framing_preset_to_string(settings.framing_preset));
    obs_data_set_default_string(data, setting_keys::tracking_mode, tracking_mode_to_string(settings.tracking_mode));
    obs_data_set_default_int(data, setting_keys::detection_interval_ms, settings.detection_interval_ms);
    obs_data_set_default_double(data, setting_keys::dead_zone, settings.dead_zone);
    obs_data_set_default_bool(data, setting_keys::debug_overlay, settings.debug_overlay);
    obs_data_set_default_bool(data, setting_keys::use_mock_detector, settings.use_mock_detector);
    obs_data_set_default_string(data, setting_keys::detector_backend,
                                detector_backend_to_string(settings.detector_backend));
    obs_data_set_default_string(data, setting_keys::detector_model_quality,
                                detector_model_quality_to_string(settings.detector_model_quality));
    obs_data_set_default_string(data, setting_keys::model_path, "");
    obs_data_set_default_double(data, setting_keys::detection_confidence, settings.detection_confidence);
    obs_data_set_default_double(data, setting_keys::detector_score_floor, settings.detector_score_floor);
    obs_data_set_default_double(data, setting_keys::nms_threshold, settings.nms_threshold);
    obs_data_set_default_double(data, setting_keys::bytetrack_track_high_thresh, settings.bytetrack_track_high_thresh);
    obs_data_set_default_double(data, setting_keys::bytetrack_track_low_thresh, settings.bytetrack_track_low_thresh);
    obs_data_set_default_double(data, setting_keys::bytetrack_new_track_thresh, settings.bytetrack_new_track_thresh);
    obs_data_set_default_double(data, setting_keys::bytetrack_match_thresh, settings.bytetrack_match_thresh);
    obs_data_set_default_int(data, setting_keys::bytetrack_track_buffer_frames, settings.bytetrack_track_buffer_frames);
    obs_data_set_default_string(data, setting_keys::subject_lock_mode,
                                subject_lock_mode_to_string(settings.subject_lock_mode));
}

obs_properties_t* auto_framing_properties(void* data) {
    obs_properties_t* props = obs_properties_create();

    const RuntimeInfo runtime = runtime_info_for(data);
    const RuntimeStatusText runtime_text = format_runtime_status_text(runtime);

    obs_property_t* status_prop =
        obs_properties_add_text(props, "runtime_status", runtime_text.status.c_str(), OBS_TEXT_INFO);
    if (runtime.status == RuntimeStatus::Error || runtime.status == RuntimeStatus::DetectorUnavailable ||
        runtime.status == RuntimeStatus::ModelMissing) {
        obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_ERROR);
    } else if (runtime.status == RuntimeStatus::NoFrameYet) {
        obs_property_text_set_info_type(status_prop, OBS_TEXT_INFO_WARNING);
    }
    obs_property_text_set_info_word_wrap(status_prop, true);

    obs_property_text_set_info_word_wrap(
        obs_properties_add_text(props, "runtime_refresh_note",
                                "Use Refresh Runtime Status to update these fields without closing this window.",
                                OBS_TEXT_INFO),
        true);

    obs_properties_add_button2(props, "runtime_refresh", text("RefreshRuntimeStatus"), refresh_runtime_status_clicked,
                               data);

    obs_properties_add_text(props, "runtime_plugin_version", runtime_text.plugin_version.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_backend", runtime_text.backend.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_model_quality", runtime_text.model_quality.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_model_loaded", runtime_text.model_loaded.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(
        obs_properties_add_text(props, "runtime_model_path", runtime_text.model_path.c_str(), OBS_TEXT_INFO), true);
    obs_properties_add_text(props, "runtime_tracker", runtime_text.tracker.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(
        obs_properties_add_text(props, "runtime_subject_lock", runtime_text.subject_lock.c_str(), OBS_TEXT_INFO), true);
    obs_properties_add_text(props, "runtime_inference", runtime_text.inference.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_detections", runtime_text.detections.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_detection_age", runtime_text.detection_age.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_tracker_state", runtime_text.tracker_state.c_str(), OBS_TEXT_INFO);
    obs_property_t* runtime_performance =
        obs_properties_add_text(props, "runtime_performance", runtime_text.performance.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(runtime_performance, true);
    obs_property_text_set_info_type(runtime_performance,
                                    runtime_text.performance == "Performance: OK" ? OBS_TEXT_INFO_NORMAL
                                                                                  : OBS_TEXT_INFO_WARNING);
    obs_properties_add_text(props, "runtime_tracks", runtime_text.tracks.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "runtime_crop", runtime_text.crop.c_str(), OBS_TEXT_INFO);

    obs_property_t* user_preset =
        obs_properties_add_list(props, setting_keys::user_preset, text("UserPreset"), OBS_COMBO_TYPE_LIST,
                                OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(user_preset, text("UserPreset.PresenterSmooth"), "presenter_smooth");
    obs_property_list_add_string(user_preset, text("UserPreset.PresenterFast"), "presenter_fast");
    obs_property_list_add_string(user_preset, text("UserPreset.Group"), "group");
    obs_property_list_add_string(user_preset, text("UserPreset.AccurateSlower"), "accurate_slower");
    obs_property_list_add_string(user_preset, text("UserPreset.LowCpu"), "low_cpu");
    obs_property_set_modified_callback2(user_preset, user_preset_modified, data);

    obs_property_t* backend = obs_properties_add_list(props, setting_keys::detector_backend, text("DetectorBackend"),
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, text("DetectorBackend.Mock"), "mock");
    obs_property_list_add_string(backend, text("DetectorBackend.OnnxRuntimeCpu"), "onnxruntime_cpu");

    obs_property_t* model_quality =
        obs_properties_add_list(props, setting_keys::detector_model_quality, text("DetectionModel"),
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(model_quality, text("DetectionModel.FastNano"), "fast_nano");
    obs_property_list_add_string(model_quality, text("DetectionModel.BalancedTiny"), "balanced_tiny");
    obs_property_list_add_string(model_quality, text("DetectionModel.AccurateSmall"), "accurate_small");
    obs_property_list_add_string(model_quality, text("DetectionModel.Custom"), "custom");

    obs_property_t* mode = obs_properties_add_list(props, setting_keys::tracking_mode, text("TrackingMode"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(mode, text("TrackingMode.Presenter"), "presenter");
    obs_property_list_add_string(mode, text("TrackingMode.Group"), "group");

    obs_property_t* preset = obs_properties_add_list(props, setting_keys::framing_preset, text("FramingPreset"),
                                                     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(preset, text("FramingPreset.Balanced"), "balanced");
    obs_property_list_add_string(preset, text("FramingPreset.Tight"), "tight");
    obs_property_list_add_string(preset, text("FramingPreset.Headroom"), "headroom");
    obs_property_list_add_string(preset, text("FramingPreset.FullBody"), "full_body");

    obs_properties_add_float_slider(props, setting_keys::tracking_speed, text("TrackingSpeed"), 0.01, 1.0, 0.01);
    obs_properties_add_float_slider(props, setting_keys::max_zoom, text("MaxZoom"), 1.0, 8.0, 0.1);

    obs_property_t* subject_lock_mode = obs_properties_add_list(
        props, setting_keys::subject_lock_mode, text("SubjectLockMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(subject_lock_mode, text("SubjectLockMode.Off"), "off");
    obs_property_list_add_string(subject_lock_mode, text("SubjectLockMode.AutoLockFirstSubject"),
                                 "auto_lock_first_subject");
    obs_property_list_add_string(subject_lock_mode, text("SubjectLockMode.Manual"), "manual");
    obs_properties_add_button2(props, "lock_current_subject", text("LockCurrentSubject"), lock_current_subject_clicked,
                               data);
    obs_properties_add_button2(props, "unlock_subject", text("UnlockSubject"), unlock_subject_clicked, data);

    obs_properties_add_bool(props, setting_keys::debug_overlay, text("DebugOverlay"));

    obs_properties_t* advanced = obs_properties_create();
    obs_properties_add_group(props, "advanced_settings", text("AdvancedSettings"), OBS_GROUP_NORMAL, advanced);

    obs_properties_add_path(advanced, setting_keys::model_path, text("ModelPath"), OBS_PATH_FILE, "ONNX Model (*.onnx)",
                            nullptr);

    obs_property_t* algorithm = obs_properties_add_list(advanced, setting_keys::tracking_algorithm,
                                                        text("TrackingAlgorithm"), OBS_COMBO_TYPE_LIST,
                                                        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(algorithm, text("TrackingAlgorithm.SimpleIou"), "simple_iou");
    obs_property_list_add_string(algorithm, text("TrackingAlgorithm.ByteTrack"), "bytetrack");

    obs_property_t* sensitivity =
        obs_properties_add_list(advanced, setting_keys::tracking_sensitivity, text("TrackingSensitivity"),
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(sensitivity, text("TrackingSensitivity.Conservative"), "conservative");
    obs_property_list_add_string(sensitivity, text("TrackingSensitivity.Balanced"), "balanced");
    obs_property_list_add_string(sensitivity, text("TrackingSensitivity.Persistent"), "persistent");

    obs_properties_add_int_slider(advanced, setting_keys::detection_interval_ms, text("DetectionInterval"), 16, 2000,
                                  1);
    obs_properties_add_float_slider(advanced, setting_keys::dead_zone, text("DeadZone"), 0.0, 0.45, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::detection_confidence, text("DetectionConfidence"), 0.01,
                                    0.99, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::detector_score_floor, text("DetectorScoreFloor"), 0.01,
                                    0.99, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::nms_threshold, text("NmsThreshold"), 0.05, 0.95, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::bytetrack_track_high_thresh,
                                    text("ByteTrackTrackHighThresh"), 0.0, 0.99, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::bytetrack_track_low_thresh,
                                    text("ByteTrackTrackLowThresh"), 0.0, 0.99, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::bytetrack_new_track_thresh,
                                    text("ByteTrackNewTrackThresh"), 0.0, 0.99, 0.01);
    obs_properties_add_float_slider(advanced, setting_keys::bytetrack_match_thresh, text("ByteTrackMatchThresh"), 0.0,
                                    0.99, 0.01);
    obs_properties_add_int_slider(advanced, setting_keys::bytetrack_track_buffer_frames,
                                  text("ByteTrackTrackBufferFrames"), 0, 600, 1);

    return props;
}

void* auto_framing_create(obs_data_t* settings, obs_source_t* source) {
    auto filter = std::make_unique<AutoFramingFilter>();
    filter->source = source;
    filter->settings = load_settings(settings);
    const char* user_preset = obs_data_get_string(settings, setting_keys::user_preset);
    filter->last_applied_user_preset =
        user_preset != nullptr && user_preset[0] != '\0' ? user_preset : default_user_preset_id;
    filter->worker_enabled = !env_flag_enabled("OBS_AUTO_FRAMING_DISABLE_WORKER");
    filter->render_enabled = !env_flag_enabled("OBS_AUTO_FRAMING_DISABLE_RENDER");
    filter->runtime.detector_backend = filter->settings.detector_backend;
    filter->runtime.detector_model_quality = filter->settings.detector_model_quality;
    filter->runtime.detection_interval_ms = filter->settings.detection_interval_ms;
    filter->runtime.tracking_algorithm = filter->settings.tracking_algorithm;
    filter->runtime.subject_lock_mode = filter->settings.subject_lock_mode;
    filter->runtime.status = RuntimeStatus::NoFrameYet;
    filter->runtime.status_detail = "Waiting for source video.";
    blog(LOG_INFO, "[obs-auto-framing] filter created; backend=%s tracker=%s render=%s worker=%s",
         detector_backend_to_string(filter->settings.detector_backend),
         tracking_algorithm_to_string(filter->settings.tracking_algorithm),
         filter->render_enabled ? "enabled" : "disabled", filter->worker_enabled ? "enabled" : "disabled");

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

void auto_framing_destroy(void* data) {
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

void auto_framing_update(void* data, obs_data_t* obs_settings) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(filter->settings_mutex);
    const AutoFramingSettings previous = filter->settings;
    filter->settings = load_settings(obs_settings);
    const char* user_preset = obs_data_get_string(obs_settings, setting_keys::user_preset);
    if (user_preset != nullptr && user_preset[0] != '\0') {
        filter->last_applied_user_preset = user_preset;
    }

    const bool detector_changed = detector_settings_changed(previous, filter->settings);
    const bool tracking_changed = tracking_state_settings_changed(previous, filter->settings);
    const bool subject_lock_changed = previous.subject_lock_mode != filter->settings.subject_lock_mode;

    if (detector_changed) {
        blog(LOG_INFO,
             "[obs-auto-framing] detector settings changed; resetting detector, tracker, detections, and crop state "
             "(backend=%s)",
             detector_backend_to_string(filter->settings.detector_backend));
        reset_tracking_state(filter, true);
    } else if (tracking_changed) {
        blog(LOG_INFO, "[obs-auto-framing] tracking/crop settings changed; resetting tracker and crop state");
        reset_tracking_state(filter, false);
    } else if (subject_lock_changed) {
        {
            std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
            filter->subject_lock = {};
        }
        set_runtime_subject_lock_state(filter, filter->settings.subject_lock_mode, {}, false, 0);
    }
}

void auto_framing_video_tick(void* data, float seconds) {
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
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        filter->crop_controller.reset();
        set_runtime_status(filter, RuntimeStatus::NoFrameYet, "Waiting for source dimensions.");
        return;
    }

    const uint64_t now = os_gettime_ns();
    const uint64_t interval_ns = static_cast<uint64_t>(settings.detection_interval_ms) * 1000000ULL;
    const bool should_detect = filter->last_submit_ns == 0 || now - filter->last_submit_ns >= interval_ns;

    std::vector<Detection> detections_to_track;
    uint64_t detection_timestamp_ns = 0;
    bool has_detection_update = false;

    {
        std::lock_guard<std::mutex> lock(filter->detection_result_mutex);
        if (filter->latest_detection_available) {
            detections_to_track = filter->latest_detections;
            detection_timestamp_ns = filter->latest_detection_timestamp_ns;
            filter->latest_detection_available = false;
            has_detection_update = true;
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

    if (has_detection_update) {
        filter->last_processed_detection_timestamp_ns = detection_timestamp_ns != 0 ? detection_timestamp_ns : now;
    }

    const uint64_t last_detection_timestamp_ns = filter->last_processed_detection_timestamp_ns;
    double detection_age_ms = 0.0;
    if (last_detection_timestamp_ns != 0 && now > last_detection_timestamp_ns) {
        detection_age_ms = static_cast<double>(now - last_detection_timestamp_ns) / 1000000.0;
    }
    TrackerRuntimeState tracker_runtime_state = TrackerRuntimeState::Predicting;
    if (has_detection_update) {
        tracker_runtime_state = TrackerRuntimeState::Detecting;
    } else if (last_detection_timestamp_ns != 0 &&
               detection_age_ms > stale_detection_age_threshold_ms(settings.detection_interval_ms)) {
        tracker_runtime_state = TrackerRuntimeState::PredictionGuarded;
    }

    Rect crop;
    Rect target_crop;
    std::vector<PersonTrack> active_tracks;
    std::vector<PersonTrack> debug_tracks;
    size_t lost_track_count = 0;

    {
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        const TrackerUpdateOptions tracker_options = tracker_options_for_subject_lock(settings, filter->subject_lock);

        if (has_detection_update) {
            filter->tracks = update_selected_tracker(filter, settings, detections_to_track, now, tracker_options);
            reconcile_subject_lock_after_tracker_update(settings, detections_to_track, filter->subject_lock,
                                                        filter->tracks);
        } else {
            filter->tracks = predict_selected_tracker(filter, settings, now, tracker_options);
            reconcile_subject_lock_after_tracker_prediction(settings, filter->subject_lock, filter->tracks);
        }
        filter->debug_tracks = selected_tracker_debug_tracks(filter, settings.tracking_algorithm);

        crop = filter->crop_controller.update({static_cast<float>(width), static_cast<float>(height)}, filter->tracks,
                                              settings, static_cast<double>(seconds));
        target_crop = filter->crop_controller.target_crop();
        active_tracks = filter->tracks;
        debug_tracks = filter->debug_tracks;
        lost_track_count = selected_tracker_lost_count(filter, settings.tracking_algorithm);
        set_runtime_subject_lock_state(filter, settings.subject_lock_mode, filter->subject_lock.locked_track_ids,
                                       filter->subject_lock.locked_subject_lost,
                                       filter->subject_lock.ignored_detection_count);

        filter->crop_x.store(crop.x, std::memory_order_relaxed);
        filter->crop_y.store(crop.y, std::memory_order_relaxed);
        filter->crop_width.store(crop.width, std::memory_order_relaxed);
        filter->crop_height.store(crop.height, std::memory_order_relaxed);
    }

    set_runtime_detection_age(filter, last_detection_timestamp_ns, detection_age_ms, settings.detection_interval_ms,
                              tracker_runtime_state);
    set_runtime_tracking_stats(filter, settings.tracking_algorithm, active_tracks.size(), lost_track_count, crop);

    if (tracker_runtime_state != TrackerRuntimeState::Detecting && last_detection_timestamp_ns != 0 &&
        detection_age_ms > stale_detection_age_threshold_ms(settings.detection_interval_ms) &&
        (filter->last_detection_age_log_ns == 0 || now - filter->last_detection_age_log_ns >= 2000000000ULL)) {
        blog(LOG_DEBUG,
             "[obs-auto-framing] detection age %.0f ms exceeds %.0f ms; tracker=%s model=%s interval=%u ms "
             "active=%zu lost=%zu",
             detection_age_ms, stale_detection_age_threshold_ms(settings.detection_interval_ms),
             tracker_runtime_state_display(tracker_runtime_state),
             detector_model_quality_display(settings.detector_model_quality), settings.detection_interval_ms,
             active_tracks.size(), lost_track_count);
        filter->last_detection_age_log_ns = now;
    }

    DebugOverlayData debug_data;
    {
        std::lock_guard<std::mutex> lock(filter->detection_result_mutex);
        debug_data.detections = filter->latest_detections;
    }
    debug_data.tracks = active_tracks;
    for (const PersonTrack& track : debug_tracks) {
        if (track.state == TrackState::Lost) {
            debug_data.lost_tracks.push_back(track);
        }
    }
    debug_data.current_crop = crop;
    debug_data.target_crop = target_crop;
    debug_data.dead_zone = settings.dead_zone;
    debug_data.tracking_algorithm = settings.tracking_algorithm;
    debug_data.subject_lock_mode = settings.subject_lock_mode;
    {
        std::lock_guard<std::mutex> tracking_lock(filter->tracking_mutex);
        debug_data.locked_track_ids = filter->subject_lock.locked_track_ids;
        debug_data.ignored_detection_count = filter->subject_lock.ignored_detection_count;
        debug_data.subject_lock_lost = filter->subject_lock.locked_subject_lost;
    }
    debug_data.active_track_count = active_tracks.size();
    debug_data.lost_track_count = lost_track_count;
    {
        std::lock_guard<std::mutex> lock(filter->debug_mutex);
        filter->debug_data = std::move(debug_data);
    }

    if (settings.debug_overlay && (filter->last_crop_log_ns == 0 || now - filter->last_crop_log_ns >= 2000000000ULL)) {
        blog(LOG_INFO,
             "[obs-auto-framing] crop current=(%.1f, %.1f %.1fx%.1f) target=(%.1f, %.1f %.1fx%.1f) tracker=%s "
             "active=%zu lost=%zu",
             crop.x, crop.y, crop.width, crop.height, target_crop.x, target_crop.y, target_crop.width,
             target_crop.height, tracking_algorithm_short_display(settings.tracking_algorithm), active_tracks.size(),
             lost_track_count);
        filter->last_crop_log_ns = now;
    }
}

void auto_framing_video_render(void* data, gs_effect_t* effect) {
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

    Rect crop{filter->crop_x.load(std::memory_order_relaxed), filter->crop_y.load(std::memory_order_relaxed),
              filter->crop_width.load(std::memory_order_relaxed), filter->crop_height.load(std::memory_order_relaxed)};

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

struct obs_source_frame* auto_framing_filter_video(void* data, struct obs_source_frame* frame) {
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
            blog(LOG_WARNING,
                 "[obs-auto-framing] unsupported source frame format for ONNX capture: format=%d (%s) size=%ux%u "
                 "linesizes=%s",
                 static_cast<int>(frame->format), get_video_format_name(frame->format), frame->width, frame->height,
                 source_frame_linesizes_to_string(frame).c_str());
        }
        set_runtime_status(filter, RuntimeStatus::Error, unsupported_source_frame_detail(frame->format));
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
             "[obs-auto-framing] captured first RGBA frame for ONNX detection: source_format=%d (%s) size=%ux%u "
             "linesizes=%s",
             static_cast<int>(frame->format), get_video_format_name(frame->format), frame->width, frame->height,
             source_frame_linesizes_to_string(frame).c_str());
    }

    return frame;
}

uint32_t auto_framing_get_width(void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return 0;
    }

    return std::max<uint32_t>(1, filter->source_width.load(std::memory_order_relaxed));
}

uint32_t auto_framing_get_height(void* data) {
    AutoFramingFilter* filter = static_cast<AutoFramingFilter*>(data);
    if (filter == nullptr) {
        return 0;
    }

    return std::max<uint32_t>(1, filter->source_height.load(std::memory_order_relaxed));
}

} // namespace

void register_auto_framing_filter() {
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
