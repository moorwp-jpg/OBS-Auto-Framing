#pragma once

#include "detector.hpp"

#include <cstdint>
#include <vector>

namespace autoframing {

struct PersonTrack {
    int id = 0;
    Rect box;
    float confidence = 0.0f;
    uint64_t last_seen_ns = 0;
    uint32_t missed_frames = 0;
};

class Tracker {
public:
    virtual ~Tracker() = default;
    virtual std::vector<PersonTrack> update(const std::vector<Detection>& detections, uint64_t timestamp_ns) = 0;
};

class IouTracker final : public Tracker {
public:
    std::vector<PersonTrack> update(const std::vector<Detection>& detections, uint64_t timestamp_ns) override;
    void reset();

private:
    std::vector<PersonTrack> tracks_;
    int next_id_ = 1;
    float iou_threshold_ = 0.25f;
    uint32_t max_missed_frames_ = 8;
};

// TODO: Replace IouTracker with a ByteTrack-style tracker once detector confidence and frame cadence are stable.

} // namespace autoframing

