#include "tracker/assignment.hpp"

#include <algorithm>

namespace autoframing {
namespace {

struct CandidateMatch {
    size_t track_index = 0;
    size_t detection_index = 0;
    float iou = 0.0f;
};

} // namespace

std::vector<AssignmentMatch> greedy_iou_match(
    const std::vector<Rect>& track_boxes,
    const std::vector<Rect>& detection_boxes,
    float iou_threshold)
{
    std::vector<CandidateMatch> candidates;
    candidates.reserve(track_boxes.size() * detection_boxes.size());

    for (size_t track_index = 0; track_index < track_boxes.size(); ++track_index) {
        if (!track_boxes[track_index].valid()) {
            continue;
        }

        for (size_t detection_index = 0; detection_index < detection_boxes.size(); ++detection_index) {
            if (!detection_boxes[detection_index].valid()) {
                continue;
            }

            const float iou = intersection_over_union(track_boxes[track_index], detection_boxes[detection_index]);
            if (iou >= iou_threshold) {
                candidates.push_back({track_index, detection_index, iou});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const CandidateMatch& a, const CandidateMatch& b) {
        return a.iou > b.iou;
    });

    std::vector<bool> matched_tracks(track_boxes.size(), false);
    std::vector<bool> matched_detections(detection_boxes.size(), false);
    std::vector<AssignmentMatch> matches;

    for (const CandidateMatch& candidate : candidates) {
        if (matched_tracks[candidate.track_index] || matched_detections[candidate.detection_index]) {
            continue;
        }

        matched_tracks[candidate.track_index] = true;
        matched_detections[candidate.detection_index] = true;
        matches.push_back({candidate.track_index, candidate.detection_index, candidate.iou});
    }

    return matches;
}

} // namespace autoframing

