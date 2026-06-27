#pragma once

#include "geometry.hpp"
#include "settings.hpp"
#include "tracker.hpp"

#include <vector>

namespace autoframing {

class CropController {
public:
    Rect update(const Size& source_size, const std::vector<PersonTrack>& tracks, const AutoFramingSettings& settings, double dt);

    Rect current_crop() const { return current_crop_; }
    Rect target_crop() const { return target_crop_; }
    bool has_crop() const { return has_crop_; }
    void reset();

private:
    Rect choose_subject(const std::vector<PersonTrack>& tracks, TrackingMode mode) const;
    Rect build_target_crop(const Size& source_size, const Rect& subject, const AutoFramingSettings& settings) const;
    Rect full_frame_crop(const Size& source_size) const;
    Rect preserve_aspect(Rect rect, float aspect) const;
    Rect enforce_zoom_and_bounds(Rect rect, const Size& source_size, double max_zoom) const;
    Rect apply_dead_zone(Rect target, double dead_zone) const;

    Rect current_crop_;
    Rect target_crop_;
    bool has_crop_ = false;
};

} // namespace autoframing
