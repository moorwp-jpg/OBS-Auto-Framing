#pragma once

#include "detector.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace autoframing {

enum class TrackState {
    New,
    Tracked,
    Lost,
    Removed,
};

inline const char* track_state_to_string(TrackState state) {
    switch (state) {
    case TrackState::New:
        return "New";
    case TrackState::Lost:
        return "Lost";
    case TrackState::Removed:
        return "Removed";
    case TrackState::Tracked:
    default:
        return "Tracked";
    }
}

inline const char* track_state_short_name(TrackState state) {
    switch (state) {
    case TrackState::New:
        return "NEW";
    case TrackState::Lost:
        return "LOST";
    case TrackState::Removed:
        return "REM";
    case TrackState::Tracked:
    default:
        return "TRK";
    }
}

struct PersonTrack {
    int id = 0;
    Rect box;
    float confidence = 0.0f;
    uint64_t last_seen_ns = 0;
    uint32_t missed_frames = 0;
    TrackState state = TrackState::Tracked;
};

struct TrackerUpdateOptions {
    bool allow_new_tracks = true;
    std::vector<int> locked_track_ids;
};

class Tracker {
  public:
    virtual ~Tracker() = default;
    virtual std::vector<PersonTrack> update(const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                            const TrackerUpdateOptions& options = {}) = 0;
    virtual std::vector<PersonTrack> predict(uint64_t timestamp_ns, const TrackerUpdateOptions& options = {}) = 0;
    virtual void reset() = 0;
    virtual size_t active_track_count() const = 0;
    virtual size_t lost_track_count() const = 0;
    virtual std::vector<PersonTrack> debug_tracks() const = 0;
};

class IouTracker final : public Tracker {
  public:
    std::vector<PersonTrack> update(const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                    const TrackerUpdateOptions& options = {}) override;
    std::vector<PersonTrack> predict(uint64_t timestamp_ns, const TrackerUpdateOptions& options = {}) override;
    void reset() override;
    size_t active_track_count() const override;
    size_t lost_track_count() const override;
    std::vector<PersonTrack> debug_tracks() const override;

  private:
    std::vector<PersonTrack> tracks_;
    int next_id_ = 1;
    float iou_threshold_ = 0.25f;
    uint32_t max_missed_frames_ = 8;
};

} // namespace autoframing
