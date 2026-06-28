#include "tracker.hpp"

#include <algorithm>
#include <limits>

namespace autoframing {
namespace {

bool track_is_locked_or_unlocked(const PersonTrack& track, const TrackerUpdateOptions& options) {
    return options.locked_track_ids.empty() ||
           std::find(options.locked_track_ids.begin(), options.locked_track_ids.end(), track.id) !=
               options.locked_track_ids.end();
}

} // namespace

void IouTracker::reset() {
    tracks_.clear();
    next_id_ = 1;
}

std::vector<PersonTrack> IouTracker::update(const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                            const TrackerUpdateOptions& options) {
    std::vector<bool> matched_detections(detections.size(), false);

    for (PersonTrack& track : tracks_) {
        float best_iou = iou_threshold_;
        size_t best_index = std::numeric_limits<size_t>::max();

        if (track_is_locked_or_unlocked(track, options)) {
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
        }

        if (best_index != std::numeric_limits<size_t>::max()) {
            const Detection& detection = detections[best_index];
            track.box = detection.box;
            track.confidence = detection.confidence;
            track.last_seen_ns = timestamp_ns;
            track.missed_frames = 0;
            track.state = TrackState::Tracked;
            matched_detections[best_index] = true;
        } else {
            ++track.missed_frames;
            track.state = TrackState::Lost;
        }
    }

    if (options.allow_new_tracks) {
        for (size_t i = 0; i < detections.size(); ++i) {
            if (matched_detections[i]) {
                continue;
            }

            PersonTrack track;
            track.id = next_id_++;
            track.box = detections[i].box;
            track.confidence = detections[i].confidence;
            track.last_seen_ns = timestamp_ns;
            track.state = TrackState::New;
            tracks_.push_back(track);
        }
    }

    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [this](const PersonTrack& track) { return track.missed_frames > max_missed_frames_; }),
                  tracks_.end());

    std::vector<PersonTrack> visible_tracks;
    for (const PersonTrack& track : tracks_) {
        if (track.missed_frames == 0 && track_is_locked_or_unlocked(track, options)) {
            visible_tracks.push_back(track);
        }
    }
    return visible_tracks;
}

std::vector<PersonTrack> IouTracker::predict(uint64_t, const TrackerUpdateOptions& options) {
    std::vector<PersonTrack> visible_tracks;
    visible_tracks.reserve(tracks_.size());
    for (const PersonTrack& track : tracks_) {
        if (track.missed_frames == 0 && track_is_locked_or_unlocked(track, options)) {
            visible_tracks.push_back(track);
        }
    }
    return visible_tracks;
}

size_t IouTracker::active_track_count() const {
    return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(),
                                             [](const PersonTrack& track) { return track.missed_frames == 0; }));
}

size_t IouTracker::lost_track_count() const {
    return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(),
                                             [](const PersonTrack& track) { return track.missed_frames > 0; }));
}

std::vector<PersonTrack> IouTracker::debug_tracks() const { return tracks_; }

} // namespace autoframing
