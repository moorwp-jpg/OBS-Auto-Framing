#include "tracker.hpp"

#include <algorithm>
#include <limits>

namespace autoframing {

void IouTracker::reset()
{
    tracks_.clear();
    next_id_ = 1;
}

std::vector<PersonTrack> IouTracker::update(const std::vector<Detection>& detections, uint64_t timestamp_ns)
{
    std::vector<bool> matched_detections(detections.size(), false);

    for (PersonTrack& track : tracks_) {
        float best_iou = iou_threshold_;
        size_t best_index = std::numeric_limits<size_t>::max();

        for (size_t i = 0; i < detections.size(); ++i) {
            if (matched_detections[i]) {
                continue;
            }

            const float iou = intersection_over_union(track.box, detections[i].box);
            if (iou > best_iou) {
                best_iou = iou;
                best_index = i;
            }
        }

        if (best_index != std::numeric_limits<size_t>::max()) {
            const Detection& detection = detections[best_index];
            track.box = detection.box;
            track.confidence = detection.confidence;
            track.last_seen_ns = timestamp_ns;
            track.missed_frames = 0;
            matched_detections[best_index] = true;
        } else {
            ++track.missed_frames;
        }
    }

    for (size_t i = 0; i < detections.size(); ++i) {
        if (matched_detections[i]) {
            continue;
        }

        PersonTrack track;
        track.id = next_id_++;
        track.box = detections[i].box;
        track.confidence = detections[i].confidence;
        track.last_seen_ns = timestamp_ns;
        tracks_.push_back(track);
    }

    tracks_.erase(
        std::remove_if(
            tracks_.begin(),
            tracks_.end(),
            [this](const PersonTrack& track) { return track.missed_frames > max_missed_frames_; }),
        tracks_.end());

    std::vector<PersonTrack> visible_tracks;
    for (const PersonTrack& track : tracks_) {
        if (track.missed_frames == 0) {
            visible_tracks.push_back(track);
        }
    }
    return visible_tracks;
}

} // namespace autoframing

