#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace autoframing {

namespace setting_keys {
inline constexpr const char* user_preset = "user_preset";
inline constexpr const char* tracking_speed = "tracking_speed";
inline constexpr const char* tracking_algorithm = "tracking_algorithm";
inline constexpr const char* tracking_sensitivity = "tracking_sensitivity";
inline constexpr const char* max_zoom = "max_zoom";
inline constexpr const char* framing_preset = "framing_preset";
inline constexpr const char* tracking_mode = "tracking_mode";
inline constexpr const char* detection_interval_ms = "detection_interval_ms";
inline constexpr const char* dead_zone = "dead_zone";
inline constexpr const char* debug_overlay = "debug_overlay";
inline constexpr const char* use_mock_detector = "use_mock_detector";
inline constexpr const char* detector_backend = "detector_backend";
inline constexpr const char* detector_model_quality = "detector_model_quality";
inline constexpr const char* model_path = "model_path";
inline constexpr const char* detection_confidence = "detection_confidence";
inline constexpr const char* detector_score_floor = "detector_score_floor";
inline constexpr const char* nms_threshold = "nms_threshold";
inline constexpr const char* bytetrack_track_high_thresh = "bytetrack_track_high_thresh";
inline constexpr const char* bytetrack_track_low_thresh = "bytetrack_track_low_thresh";
inline constexpr const char* bytetrack_new_track_thresh = "bytetrack_new_track_thresh";
inline constexpr const char* bytetrack_match_thresh = "bytetrack_match_thresh";
inline constexpr const char* bytetrack_track_buffer_frames = "bytetrack_track_buffer_frames";
inline constexpr const char* subject_lock_mode = "subject_lock_mode";
} // namespace setting_keys

enum class TrackingAlgorithm {
    SimpleIou,
    ByteTrack,
};

enum class TrackingSensitivity {
    Conservative,
    Balanced,
    Persistent,
};

enum class FramingPreset {
    Balanced,
    Tight,
    Headroom,
    FullBody,
};

enum class TrackingMode {
    Presenter,
    Group,
};

enum class DetectorBackend {
    Mock,
    OnnxRuntimeCpu,
};

enum class DetectorModelQuality {
    FastNano,
    BalancedTiny,
    AccurateSmall,
    Custom,
};

enum class SubjectLockMode {
    Off,
    AutoLockFirstSubject,
    Manual,
};

struct AutoFramingSettings {
    double tracking_speed = 0.32;
    TrackingAlgorithm tracking_algorithm = TrackingAlgorithm::ByteTrack;
    TrackingSensitivity tracking_sensitivity = TrackingSensitivity::Balanced;
    double max_zoom = 2.4;
    FramingPreset framing_preset = FramingPreset::Headroom;
    TrackingMode tracking_mode = TrackingMode::Presenter;
    uint32_t detection_interval_ms = 150;
    double dead_zone = 0.12;
    bool debug_overlay = false;
    bool use_mock_detector = false;
    DetectorBackend detector_backend = DetectorBackend::OnnxRuntimeCpu;
    DetectorModelQuality detector_model_quality = DetectorModelQuality::BalancedTiny;
    std::string model_path;
    double detection_confidence = 0.30;
    double detector_score_floor = 0.05;
    double nms_threshold = 0.45;
    double bytetrack_track_high_thresh = 0.0;
    double bytetrack_track_low_thresh = 0.0;
    double bytetrack_new_track_thresh = 0.0;
    double bytetrack_match_thresh = 0.0;
    uint32_t bytetrack_track_buffer_frames = 0;
    SubjectLockMode subject_lock_mode = SubjectLockMode::Off;
};

inline AutoFramingSettings default_settings()
{
    return {};
}

inline TrackingAlgorithm tracking_algorithm_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "simple_iou") == 0) {
        return TrackingAlgorithm::SimpleIou;
    }
    return TrackingAlgorithm::ByteTrack;
}

inline const char* tracking_algorithm_to_string(TrackingAlgorithm algorithm)
{
    return algorithm == TrackingAlgorithm::SimpleIou ? "simple_iou" : "bytetrack";
}

inline TrackingSensitivity tracking_sensitivity_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "conservative") == 0) {
        return TrackingSensitivity::Conservative;
    }
    if (value != nullptr && std::strcmp(value, "persistent") == 0) {
        return TrackingSensitivity::Persistent;
    }
    return TrackingSensitivity::Balanced;
}

inline const char* tracking_sensitivity_to_string(TrackingSensitivity sensitivity)
{
    switch (sensitivity) {
    case TrackingSensitivity::Conservative:
        return "conservative";
    case TrackingSensitivity::Persistent:
        return "persistent";
    case TrackingSensitivity::Balanced:
    default:
        return "balanced";
    }
}

inline FramingPreset framing_preset_from_string(const char* value)
{
    if (value == nullptr) {
        return FramingPreset::Balanced;
    }
    if (std::strcmp(value, "tight") == 0) {
        return FramingPreset::Tight;
    }
    if (std::strcmp(value, "headroom") == 0) {
        return FramingPreset::Headroom;
    }
    if (std::strcmp(value, "full_body") == 0) {
        return FramingPreset::FullBody;
    }
    return FramingPreset::Balanced;
}

inline const char* framing_preset_to_string(FramingPreset preset)
{
    switch (preset) {
    case FramingPreset::Tight:
        return "tight";
    case FramingPreset::Headroom:
        return "headroom";
    case FramingPreset::FullBody:
        return "full_body";
    case FramingPreset::Balanced:
    default:
        return "balanced";
    }
}

inline TrackingMode tracking_mode_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "group") == 0) {
        return TrackingMode::Group;
    }
    return TrackingMode::Presenter;
}

inline const char* tracking_mode_to_string(TrackingMode mode)
{
    return mode == TrackingMode::Group ? "group" : "presenter";
}

inline DetectorBackend detector_backend_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "onnxruntime_cpu") == 0) {
        return DetectorBackend::OnnxRuntimeCpu;
    }
    return DetectorBackend::Mock;
}

inline const char* detector_backend_to_string(DetectorBackend backend)
{
    return backend == DetectorBackend::OnnxRuntimeCpu ? "onnxruntime_cpu" : "mock";
}

inline DetectorModelQuality detector_model_quality_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "fast_nano") == 0) {
        return DetectorModelQuality::FastNano;
    }
    if (value != nullptr && std::strcmp(value, "accurate_small") == 0) {
        return DetectorModelQuality::AccurateSmall;
    }
    if (value != nullptr && std::strcmp(value, "custom") == 0) {
        return DetectorModelQuality::Custom;
    }
    return DetectorModelQuality::BalancedTiny;
}

inline const char* detector_model_quality_to_string(DetectorModelQuality quality)
{
    switch (quality) {
    case DetectorModelQuality::FastNano:
        return "fast_nano";
    case DetectorModelQuality::AccurateSmall:
        return "accurate_small";
    case DetectorModelQuality::Custom:
        return "custom";
    case DetectorModelQuality::BalancedTiny:
    default:
        return "balanced_tiny";
    }
}

inline const char* detector_model_filename(DetectorModelQuality quality)
{
    switch (quality) {
    case DetectorModelQuality::FastNano:
        return "yolox_nano.onnx";
    case DetectorModelQuality::AccurateSmall:
        return "yolox_s.onnx";
    case DetectorModelQuality::BalancedTiny:
    case DetectorModelQuality::Custom:
    default:
        return "yolox_tiny.onnx";
    }
}

inline SubjectLockMode subject_lock_mode_from_string(const char* value)
{
    if (value != nullptr && std::strcmp(value, "auto_lock_first_subject") == 0) {
        return SubjectLockMode::AutoLockFirstSubject;
    }
    if (value != nullptr && std::strcmp(value, "manual") == 0) {
        return SubjectLockMode::Manual;
    }
    return SubjectLockMode::Off;
}

inline const char* subject_lock_mode_to_string(SubjectLockMode mode)
{
    switch (mode) {
    case SubjectLockMode::AutoLockFirstSubject:
        return "auto_lock_first_subject";
    case SubjectLockMode::Manual:
        return "manual";
    case SubjectLockMode::Off:
    default:
        return "off";
    }
}

inline AutoFramingSettings sanitize_settings(AutoFramingSettings settings)
{
    settings.tracking_speed = std::clamp(settings.tracking_speed, 0.01, 1.0);
    settings.max_zoom = std::clamp(settings.max_zoom, 1.0, 8.0);
    settings.detection_interval_ms = std::clamp<uint32_t>(settings.detection_interval_ms, 16, 2000);
    settings.dead_zone = std::clamp(settings.dead_zone, 0.0, 0.45);
    settings.detection_confidence = std::clamp(settings.detection_confidence, 0.01, 0.99);
    settings.detector_score_floor = std::clamp(settings.detector_score_floor, 0.01, 0.99);
    settings.nms_threshold = std::clamp(settings.nms_threshold, 0.05, 0.95);
    settings.bytetrack_track_high_thresh = std::clamp(settings.bytetrack_track_high_thresh, 0.0, 0.99);
    settings.bytetrack_track_low_thresh = std::clamp(settings.bytetrack_track_low_thresh, 0.0, 0.99);
    settings.bytetrack_new_track_thresh = std::clamp(settings.bytetrack_new_track_thresh, 0.0, 0.99);
    settings.bytetrack_match_thresh = std::clamp(settings.bytetrack_match_thresh, 0.0, 0.99);
    settings.bytetrack_track_buffer_frames = std::min<uint32_t>(settings.bytetrack_track_buffer_frames, 600);
    return settings;
}

} // namespace autoframing
