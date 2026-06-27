#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace autoframing {

namespace setting_keys {
inline constexpr const char* tracking_speed = "tracking_speed";
inline constexpr const char* max_zoom = "max_zoom";
inline constexpr const char* framing_preset = "framing_preset";
inline constexpr const char* tracking_mode = "tracking_mode";
inline constexpr const char* detection_interval_ms = "detection_interval_ms";
inline constexpr const char* dead_zone = "dead_zone";
inline constexpr const char* debug_overlay = "debug_overlay";
inline constexpr const char* use_mock_detector = "use_mock_detector";
inline constexpr const char* detector_backend = "detector_backend";
inline constexpr const char* model_path = "model_path";
inline constexpr const char* detection_confidence = "detection_confidence";
inline constexpr const char* nms_threshold = "nms_threshold";
} // namespace setting_keys

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

struct AutoFramingSettings {
    double tracking_speed = 0.35;
    double max_zoom = 2.5;
    FramingPreset framing_preset = FramingPreset::Balanced;
    TrackingMode tracking_mode = TrackingMode::Presenter;
    uint32_t detection_interval_ms = 120;
    double dead_zone = 0.12;
    bool debug_overlay = false;
    bool use_mock_detector = true;
    DetectorBackend detector_backend = DetectorBackend::Mock;
    std::string model_path;
    double detection_confidence = 0.30;
    double nms_threshold = 0.45;
};

inline AutoFramingSettings default_settings()
{
    return {};
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

inline AutoFramingSettings sanitize_settings(AutoFramingSettings settings)
{
    settings.tracking_speed = std::clamp(settings.tracking_speed, 0.01, 1.0);
    settings.max_zoom = std::clamp(settings.max_zoom, 1.0, 8.0);
    settings.detection_interval_ms = std::clamp<uint32_t>(settings.detection_interval_ms, 16, 2000);
    settings.dead_zone = std::clamp(settings.dead_zone, 0.0, 0.45);
    settings.detection_confidence = std::clamp(settings.detection_confidence, 0.01, 0.99);
    settings.nms_threshold = std::clamp(settings.nms_threshold, 0.05, 0.95);
    return settings;
}

} // namespace autoframing
