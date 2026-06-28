#include "tracker/bytetrack_tracker.hpp"

#include "tracker/assignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace autoframing {
namespace {

constexpr float minimum_aspect = 0.05f;
constexpr float minimum_height = 1.0f;
constexpr double nanoseconds_per_second = 1000000000.0;
constexpr double nominal_motion_step_seconds = 1.0 / 30.0;
constexpr double minimum_velocity_interval_seconds = 0.001;
constexpr double maximum_velocity_interval_seconds = 2.0;
constexpr double maximum_prediction_step_seconds = 0.25;

struct BoxMeasurement {
    float center_x = 0.0f;
    float center_y = 0.0f;
    float aspect = 1.0f;
    float height = 0.0f;
};

float finite_or(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float clamp_threshold(float value, float minimum, float maximum) {
    return std::clamp(finite_or(value, minimum), minimum, maximum);
}

BoxMeasurement measurement_from_rect(const Rect& box) {
    if (!box.valid()) {
        return {};
    }

    return {
        box.center_x(),
        box.center_y(),
        std::max(minimum_aspect, box.width / std::max(minimum_height, box.height)),
        std::max(minimum_height, box.height),
    };
}

bool is_active_state(TrackState state) { return state == TrackState::New || state == TrackState::Tracked; }

bool is_matchable_state(TrackState state) {
    return state == TrackState::New || state == TrackState::Tracked || state == TrackState::Lost;
}

bool is_active_track(const PersonTrack& track) { return is_active_state(track.state) && track.missed_frames == 0; }

bool track_is_locked_or_unlocked(const PersonTrack& track, const TrackerUpdateOptions& options) {
    return options.locked_track_ids.empty() ||
           std::find(options.locked_track_ids.begin(), options.locked_track_ids.end(), track.id) !=
               options.locked_track_ids.end();
}

ByteTrackConfig sanitize_config(ByteTrackConfig config) {
    config.track_high_thresh = clamp_threshold(config.track_high_thresh, 0.01f, 0.99f);
    config.track_low_thresh = clamp_threshold(config.track_low_thresh, 0.01f, config.track_high_thresh);
    config.new_track_thresh = clamp_threshold(config.new_track_thresh, 0.01f, 0.99f);
    config.match_thresh = clamp_threshold(config.match_thresh, 0.01f, 0.99f);
    config.track_buffer_frames = std::clamp<uint32_t>(config.track_buffer_frames, 1, 600);
    config.prediction_drift_guard_ms = std::clamp<uint32_t>(config.prediction_drift_guard_ms, 250, 10000);
    config.prediction_hold_ms =
        std::clamp<uint32_t>(std::max(config.prediction_hold_ms, config.prediction_drift_guard_ms),
                             config.prediction_drift_guard_ms, 20000);
    return config;
}

double elapsed_seconds(uint64_t previous_timestamp_ns, uint64_t current_timestamp_ns) {
    if (previous_timestamp_ns == 0 || current_timestamp_ns <= previous_timestamp_ns) {
        return 0.0;
    }

    return static_cast<double>(current_timestamp_ns - previous_timestamp_ns) / nanoseconds_per_second;
}

} // namespace

ByteTrackConfig bytetrack_config_for_sensitivity(TrackingSensitivity sensitivity) {
    switch (sensitivity) {
    case TrackingSensitivity::Conservative:
        return {0.55f, 0.15f, 0.65f, 0.35f, 12};
    case TrackingSensitivity::Persistent:
        return {0.35f, 0.05f, 0.45f, 0.25f, 60};
    case TrackingSensitivity::Balanced:
    default:
        return {0.45f, 0.10f, 0.55f, 0.30f, 30};
    }
}

ByteTrackConfig bytetrack_config_from_settings(const AutoFramingSettings& settings) {
    ByteTrackConfig config = bytetrack_config_for_sensitivity(settings.tracking_sensitivity);

    if (settings.bytetrack_track_high_thresh > 0.0) {
        config.track_high_thresh = static_cast<float>(settings.bytetrack_track_high_thresh);
    }
    if (settings.bytetrack_track_low_thresh > 0.0) {
        config.track_low_thresh = static_cast<float>(settings.bytetrack_track_low_thresh);
    }
    if (settings.bytetrack_new_track_thresh > 0.0) {
        config.new_track_thresh = static_cast<float>(settings.bytetrack_new_track_thresh);
    }
    if (settings.bytetrack_match_thresh > 0.0) {
        config.match_thresh = static_cast<float>(settings.bytetrack_match_thresh);
    }
    if (settings.bytetrack_track_buffer_frames > 0) {
        config.track_buffer_frames = settings.bytetrack_track_buffer_frames;
    }
    config.prediction_drift_guard_ms = std::max<uint32_t>(750, settings.detection_interval_ms * 2);
    config.prediction_hold_ms =
        std::max<uint32_t>(config.prediction_drift_guard_ms * 2, settings.detection_interval_ms * 4);

    return sanitize_config(config);
}

ByteTrackTracker::ByteTrackTracker(ByteTrackConfig config) : config_(sanitize_config(config)) {}

void ByteTrackTracker::set_config(ByteTrackConfig config) { config_ = sanitize_config(config); }

void ByteTrackTracker::reset() {
    tracks_.clear();
    next_id_ = 1;
}

void ByteTrackTracker::initiate_motion(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns) {
    const BoxMeasurement measurement = measurement_from_rect(box);
    motion.center_x = measurement.center_x;
    motion.center_y = measurement.center_y;
    motion.aspect = measurement.aspect;
    motion.height = measurement.height;
    motion.velocity_x = 0.0f;
    motion.velocity_y = 0.0f;
    motion.velocity_aspect = 0.0f;
    motion.velocity_height = 0.0f;
    motion.motion_timestamp_ns = timestamp_ns;
    remember_measurement(motion, box, timestamp_ns);
    motion.initialized = true;
}

void ByteTrackTracker::remember_measurement(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns) {
    const BoxMeasurement measurement = measurement_from_rect(box);
    motion.measurement_center_x = measurement.center_x;
    motion.measurement_center_y = measurement.center_y;
    motion.measurement_aspect = measurement.aspect;
    motion.measurement_height = measurement.height;
    motion.measurement_timestamp_ns = timestamp_ns;
}

Rect ByteTrackTracker::predict_motion(KalmanBoxFilter& motion, uint64_t timestamp_ns, const ByteTrackConfig& config) {
    if (!motion.initialized) {
        return {};
    }

    double step_seconds =
        std::min(elapsed_seconds(motion.motion_timestamp_ns, timestamp_ns), maximum_prediction_step_seconds);
    const double age_since_measurement_seconds = elapsed_seconds(motion.measurement_timestamp_ns, timestamp_ns);
    const double guard_seconds = static_cast<double>(config.prediction_drift_guard_ms) / 1000.0;
    const double hold_seconds = static_cast<double>(config.prediction_hold_ms) / 1000.0;
    float velocity_scale = 1.0f;

    if (age_since_measurement_seconds >= hold_seconds) {
        step_seconds = 0.0;
        motion.velocity_x = 0.0f;
        motion.velocity_y = 0.0f;
        motion.velocity_aspect = 0.0f;
        motion.velocity_height = 0.0f;
    } else if (age_since_measurement_seconds > guard_seconds && hold_seconds > guard_seconds) {
        const double guard_window = hold_seconds - guard_seconds;
        const double guarded_age = age_since_measurement_seconds - guard_seconds;
        velocity_scale = static_cast<float>(std::clamp(1.0 - guarded_age / guard_window, 0.0, 1.0));
        motion.velocity_x *= velocity_scale;
        motion.velocity_y *= velocity_scale;
        motion.velocity_aspect *= velocity_scale;
        motion.velocity_height *= velocity_scale;
    }

    if (step_seconds > 0.0) {
        motion.center_x += motion.velocity_x * static_cast<float>(step_seconds);
        motion.center_y += motion.velocity_y * static_cast<float>(step_seconds);
        motion.aspect =
            std::max(minimum_aspect, motion.aspect + motion.velocity_aspect * static_cast<float>(step_seconds));
        motion.height =
            std::max(minimum_height, motion.height + motion.velocity_height * static_cast<float>(step_seconds));
    }
    if (timestamp_ns > motion.motion_timestamp_ns) {
        motion.motion_timestamp_ns = timestamp_ns;
    }

    const float height = std::max(minimum_height, motion.height);
    const float width = std::max(minimum_height, motion.aspect * height);
    return make_centered_rect(motion.center_x, motion.center_y, width, height);
}

Rect ByteTrackTracker::update_motion(KalmanBoxFilter& motion, const Rect& box, uint64_t timestamp_ns) {
    if (!motion.initialized) {
        initiate_motion(motion, box, timestamp_ns);
        return box;
    }

    const BoxMeasurement measurement = measurement_from_rect(box);
    const float residual_x = measurement.center_x - motion.center_x;
    const float residual_y = measurement.center_y - motion.center_y;
    const float residual_aspect = measurement.aspect - motion.aspect;
    const float residual_height = measurement.height - motion.height;

    constexpr float position_gain = 0.85f;
    constexpr float velocity_gain = 0.50f;
    constexpr float velocity_decay = 0.50f;

    motion.center_x += residual_x * position_gain;
    motion.center_y += residual_y * position_gain;
    motion.aspect = std::max(minimum_aspect, motion.aspect + residual_aspect * position_gain);
    motion.height = std::max(minimum_height, motion.height + residual_height * position_gain);

    double velocity_interval_seconds = elapsed_seconds(motion.measurement_timestamp_ns, timestamp_ns);
    if (velocity_interval_seconds < minimum_velocity_interval_seconds ||
        velocity_interval_seconds > maximum_velocity_interval_seconds) {
        velocity_interval_seconds = nominal_motion_step_seconds;
    }

    motion.velocity_x = motion.velocity_x * velocity_decay + ((measurement.center_x - motion.measurement_center_x) /
                                                              static_cast<float>(velocity_interval_seconds)) *
                                                                 velocity_gain;
    motion.velocity_y = motion.velocity_y * velocity_decay + ((measurement.center_y - motion.measurement_center_y) /
                                                              static_cast<float>(velocity_interval_seconds)) *
                                                                 velocity_gain;
    motion.velocity_aspect =
        motion.velocity_aspect * velocity_decay +
        ((measurement.aspect - motion.measurement_aspect) / static_cast<float>(velocity_interval_seconds)) *
            velocity_gain;
    motion.velocity_height =
        motion.velocity_height * velocity_decay +
        ((measurement.height - motion.measurement_height) / static_cast<float>(velocity_interval_seconds)) *
            velocity_gain;

    if (timestamp_ns > motion.motion_timestamp_ns) {
        motion.motion_timestamp_ns = timestamp_ns;
    }
    remember_measurement(motion, box, timestamp_ns);

    const float height = std::max(minimum_height, motion.height);
    const float width = std::max(minimum_height, motion.aspect * height);
    return make_centered_rect(motion.center_x, motion.center_y, width, height);
}

void ByteTrackTracker::update_track_from_detection(InternalTrack& track, const Detection& detection,
                                                   uint64_t timestamp_ns, TrackState state) {
    track.person.box = update_motion(track.motion, detection.box, timestamp_ns);
    track.person.confidence = detection.confidence;
    track.person.last_seen_ns = timestamp_ns;
    track.person.missed_frames = 0;
    track.person.state = state;
}

std::vector<PersonTrack> ByteTrackTracker::update(const std::vector<Detection>& detections, uint64_t timestamp_ns,
                                                  const TrackerUpdateOptions& options) {
    for (InternalTrack& track : tracks_) {
        if (track.person.state == TrackState::Removed) {
            continue;
        }

        const Rect predicted = predict_motion(track.motion, timestamp_ns, config_);
        if (predicted.valid()) {
            track.person.box = predicted;
        }
        if (track.person.missed_frames < std::numeric_limits<uint32_t>::max()) {
            ++track.person.missed_frames;
        }
    }

    std::vector<size_t> high_detection_indices;
    std::vector<size_t> low_detection_indices;
    high_detection_indices.reserve(detections.size());
    low_detection_indices.reserve(detections.size());

    for (size_t i = 0; i < detections.size(); ++i) {
        if (!detections[i].box.valid() || detections[i].confidence < config_.track_low_thresh) {
            continue;
        }

        if (detections[i].confidence >= config_.track_high_thresh) {
            high_detection_indices.push_back(i);
        } else {
            low_detection_indices.push_back(i);
        }
    }

    std::vector<size_t> first_track_indices;
    std::vector<Rect> first_track_boxes;
    first_track_indices.reserve(tracks_.size());
    first_track_boxes.reserve(tracks_.size());

    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (track_is_locked_or_unlocked(tracks_[i].person, options) && is_matchable_state(tracks_[i].person.state)) {
            first_track_indices.push_back(i);
            first_track_boxes.push_back(tracks_[i].person.box);
        }
    }

    std::vector<Rect> high_detection_boxes;
    high_detection_boxes.reserve(high_detection_indices.size());
    for (const size_t detection_index : high_detection_indices) {
        high_detection_boxes.push_back(detections[detection_index].box);
    }

    std::vector<bool> track_matched(tracks_.size(), false);
    std::vector<bool> high_detection_matched(high_detection_indices.size(), false);

    for (const AssignmentMatch& match :
         greedy_iou_match(first_track_boxes, high_detection_boxes, config_.match_thresh)) {
        const size_t track_index = first_track_indices[match.track_index];
        const size_t high_index = match.detection_index;
        const size_t detection_index = high_detection_indices[high_index];
        update_track_from_detection(tracks_[track_index], detections[detection_index], timestamp_ns,
                                    TrackState::Tracked);
        track_matched[track_index] = true;
        high_detection_matched[high_index] = true;
    }

    std::vector<size_t> second_track_indices;
    std::vector<Rect> second_track_boxes;
    second_track_indices.reserve(tracks_.size());
    second_track_boxes.reserve(tracks_.size());

    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (!track_matched[i] && track_is_locked_or_unlocked(tracks_[i].person, options) &&
            is_active_state(tracks_[i].person.state)) {
            second_track_indices.push_back(i);
            second_track_boxes.push_back(tracks_[i].person.box);
        }
    }

    std::vector<Rect> low_detection_boxes;
    low_detection_boxes.reserve(low_detection_indices.size());
    for (const size_t detection_index : low_detection_indices) {
        low_detection_boxes.push_back(detections[detection_index].box);
    }

    for (const AssignmentMatch& match :
         greedy_iou_match(second_track_boxes, low_detection_boxes, config_.match_thresh)) {
        const size_t track_index = second_track_indices[match.track_index];
        const size_t detection_index = low_detection_indices[match.detection_index];
        update_track_from_detection(tracks_[track_index], detections[detection_index], timestamp_ns,
                                    TrackState::Tracked);
        track_matched[track_index] = true;
    }

    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (!track_matched[i] && is_active_state(tracks_[i].person.state)) {
            tracks_[i].person.state = TrackState::Lost;
        }
    }

    if (options.allow_new_tracks) {
        for (size_t high_index = 0; high_index < high_detection_indices.size(); ++high_index) {
            if (high_detection_matched[high_index]) {
                continue;
            }

            const Detection& detection = detections[high_detection_indices[high_index]];
            if (detection.confidence < config_.new_track_thresh) {
                continue;
            }

            InternalTrack track;
            track.person.id = next_id_++;
            track.person.box = detection.box;
            track.person.confidence = detection.confidence;
            track.person.last_seen_ns = timestamp_ns;
            track.person.missed_frames = 0;
            track.person.state = TrackState::New;
            initiate_motion(track.motion, detection.box, timestamp_ns);
            tracks_.push_back(track);
        }
    }

    for (InternalTrack& track : tracks_) {
        if (track.person.state == TrackState::Lost && track.person.missed_frames > config_.track_buffer_frames) {
            track.person.state = TrackState::Removed;
        }
    }

    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [](const InternalTrack& track) { return track.person.state == TrackState::Removed; }),
                  tracks_.end());

    std::vector<PersonTrack> active_tracks;
    for (const InternalTrack& track : tracks_) {
        if (track_is_locked_or_unlocked(track.person, options) && is_active_track(track.person)) {
            active_tracks.push_back(track.person);
        }
    }
    return active_tracks;
}

std::vector<PersonTrack> ByteTrackTracker::predict(uint64_t timestamp_ns, const TrackerUpdateOptions& options) {
    std::vector<PersonTrack> active_tracks;
    active_tracks.reserve(tracks_.size());

    for (InternalTrack& track : tracks_) {
        if (!track_is_locked_or_unlocked(track.person, options) || !is_active_track(track.person)) {
            continue;
        }

        const Rect predicted = predict_motion(track.motion, timestamp_ns, config_);
        if (predicted.valid()) {
            track.person.box = predicted;
        }
        active_tracks.push_back(track.person);
    }

    return active_tracks;
}

size_t ByteTrackTracker::active_track_count() const {
    return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(),
                                             [](const InternalTrack& track) { return is_active_track(track.person); }));
}

size_t ByteTrackTracker::lost_track_count() const {
    return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(), [](const InternalTrack& track) {
        return track.person.state == TrackState::Lost;
    }));
}

std::vector<PersonTrack> ByteTrackTracker::debug_tracks() const {
    std::vector<PersonTrack> tracks;
    tracks.reserve(tracks_.size());
    for (const InternalTrack& track : tracks_) {
        if (track.person.state != TrackState::Removed) {
            tracks.push_back(track.person);
        }
    }
    return tracks;
}

} // namespace autoframing
