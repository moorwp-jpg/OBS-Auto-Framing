#pragma once

#include "settings.hpp"
#include "tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace autoframing {

struct ByteTrackConfig {
    float track_high_thresh = 0.45f;
    float track_low_thresh = 0.10f;
    float new_track_thresh = 0.55f;
    float match_thresh = 0.30f;
    uint32_t track_buffer_frames = 30;
    uint32_t prediction_drift_guard_ms = 750;
    uint32_t prediction_hold_ms = 1500;
};

ByteTrackConfig bytetrack_config_for_sensitivity(TrackingSensitivity sensitivity);
ByteTrackConfig bytetrack_config_from_settings(const AutoFramingSettings& settings);

class ByteTrackTracker final : public Tracker {
  public:
    explicit ByteTrackTracker(ByteTrackConfig config = {});

    void set_config(ByteTrackConfig config);
    const ByteTrackConfig& config() const { return config_; }

    std::vector<PersonTrack> update(const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                    const TrackerUpdateOptions& options = {}) override;
    std::vector<PersonTrack> predict(uint64_t timestamp_ns, const TrackerUpdateOptions& options = {}) override;
    void reset() override;
    size_t active_track_count() const override;
    size_t lost_track_count() const override;
    std::vector<PersonTrack> debug_tracks() const override;

  private:
    struct KalmanBoxFilter {
        float center_x = 0.0f;
        float center_y = 0.0f;
        float aspect = 1.0f;
        float height = 0.0f;
        float velocity_x = 0.0f;
        float velocity_y = 0.0f;
        float velocity_aspect = 0.0f;
        float velocity_height = 0.0f;
        float measurement_center_x = 0.0f;
        float measurement_center_y = 0.0f;
        float measurement_aspect = 1.0f;
        float measurement_height = 0.0f;
        uint64_t motion_timestamp_ns = 0;
        uint64_t measurement_timestamp_ns = 0;
        bool initialized = false;
    };

    struct InternalTrack {
        PersonTrack person;
        KalmanBoxFilter motion;
    };

    static void initiate_motion(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns);
    static Rect predict_motion(KalmanBoxFilter& motion, uint64_t timestamp_ns, const ByteTrackConfig& config);
    static Rect update_motion(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns);
    static void remember_measurement(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns);
    static void update_track_from_detection(InternalTrack& track, const Detection& detection, uint64_t timestamp_ns,
                                            TrackState state);

    std::vector<InternalTrack> tracks_;
    ByteTrackConfig config_;
    int next_id_ = 1;
};

} // namespace autoframing
