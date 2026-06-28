#pragma once

#include "detector.hpp"
#include "geometry.hpp"
#include "settings.hpp"
#include "tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <obs-module.h>

namespace autoframing {

struct DebugOverlayData {
    std::vector<Detection> detections;
    std::vector<PersonTrack> tracks;
    std::vector<PersonTrack> lost_tracks;
    Rect current_crop;
    Rect target_crop;
    double dead_zone = 0.0;
    TrackingAlgorithm tracking_algorithm = TrackingAlgorithm::ByteTrack;
    SubjectLockMode subject_lock_mode = SubjectLockMode::Off;
    std::vector<int> locked_track_ids;
    size_t ignored_detection_count = 0;
    bool subject_lock_lost = false;
    size_t active_track_count = 0;
    size_t lost_track_count = 0;
};

class CropRenderer {
public:
    CropRenderer() = default;
    ~CropRenderer();

    CropRenderer(const CropRenderer&) = delete;
    CropRenderer& operator=(const CropRenderer&) = delete;

    bool initialize();

    void render(
        obs_source_t* filter_source,
        const Rect& crop,
        uint32_t source_width,
        uint32_t source_height,
        bool debug_overlay,
        const DebugOverlayData& debug_data,
        gs_effect_t* fallback_effect);

private:
    bool ensure_effect();
    void render_debug_overlay(const Rect& crop, uint32_t source_width, uint32_t source_height, const DebugOverlayData& data);

    gs_effect_t* crop_effect_ = nullptr;
    gs_eparam_t* crop_min_param_ = nullptr;
    gs_eparam_t* crop_size_param_ = nullptr;
};

} // namespace autoframing
