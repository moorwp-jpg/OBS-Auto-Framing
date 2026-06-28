#include "crop_controller.hpp"

#include <algorithm>
#include <cmath>

namespace autoframing {
namespace {

struct PresetMargins {
    float width_multiplier;
    float height_multiplier;
    float center_y_offset_boxes;
};

PresetMargins margins_for_preset(FramingPreset preset)
{
    switch (preset) {
    case FramingPreset::Tight:
        return {1.25f, 1.25f, -0.03f};
    case FramingPreset::Headroom:
        return {1.70f, 1.95f, -0.18f};
    case FramingPreset::FullBody:
        return {2.15f, 2.15f, 0.02f};
    case FramingPreset::Balanced:
    default:
        return {1.75f, 1.75f, -0.08f};
    }
}

Rect lerp_rect(const Rect& from, const Rect& to, float amount)
{
    return {
        from.x + (to.x - from.x) * amount,
        from.y + (to.y - from.y) * amount,
        from.width + (to.width - from.width) * amount,
        from.height + (to.height - from.height) * amount,
    };
}

float smoothstep(float amount)
{
    const float t = std::clamp(amount, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float soft_dead_zone_center(float current_center, float target_center, float outer_radius)
{
    if (outer_radius <= 0.0f) {
        return target_center;
    }

    constexpr float inner_radius_ratio = 0.45f;
    const float delta = target_center - current_center;
    const float distance = std::fabs(delta);
    const float inner_radius = outer_radius * inner_radius_ratio;

    if (distance <= inner_radius) {
        return current_center;
    }

    if (distance >= outer_radius) {
        return target_center;
    }

    const float influence = smoothstep((distance - inner_radius) / (outer_radius - inner_radius));
    return current_center + delta * influence;
}

Rect focus_region_for_preset(const Rect& subject, FramingPreset preset)
{
    if (preset != FramingPreset::Tight || !subject.valid()) {
        return subject;
    }

    return make_centered_rect(
        subject.center_x(),
        subject.y + subject.height * 0.28f,
        subject.width * 1.15f,
        subject.height * 0.50f);
}

} // namespace

void CropController::reset()
{
    current_crop_ = {};
    target_crop_ = {};
    has_crop_ = false;
}

Rect CropController::update(
    const Size& source_size,
    const std::vector<PersonTrack>& tracks,
    const AutoFramingSettings& raw_settings,
    double dt)
{
    if (!source_size.valid()) {
        reset();
        return {};
    }

    const AutoFramingSettings settings = sanitize_settings(raw_settings);
    const Rect subject = choose_subject(tracks, settings.tracking_mode);
    Rect target = subject.valid() ? build_target_crop(source_size, subject, settings) : full_frame_crop(source_size);
    target = apply_dead_zone(target, settings.dead_zone);
    target = enforce_zoom_and_bounds(target, source_size, settings.max_zoom);
    target_crop_ = target;

    if (!has_crop_) {
        current_crop_ = target;
        has_crop_ = true;
        return current_crop_;
    }

    const double response = settings.tracking_speed * 12.0;
    const float amount = static_cast<float>(std::clamp(1.0 - std::exp(-response * std::max(0.0, dt)), 0.0, 1.0));
    current_crop_ = lerp_rect(current_crop_, target, amount);
    current_crop_ = enforce_zoom_and_bounds(current_crop_, source_size, settings.max_zoom);
    return current_crop_;
}

Rect CropController::choose_subject(const std::vector<PersonTrack>& tracks, TrackingMode mode) const
{
    if (tracks.empty()) {
        return {};
    }

    if (mode == TrackingMode::Group) {
        std::vector<Rect> boxes;
        boxes.reserve(tracks.size());
        for (const PersonTrack& track : tracks) {
            boxes.push_back(track.box);
        }
        return union_rects(boxes);
    }

    const PersonTrack* best = &tracks.front();
    for (const PersonTrack& track : tracks) {
        const float best_score = best->confidence * 0.65f + best->box.area() * 0.000001f;
        const float score = track.confidence * 0.65f + track.box.area() * 0.000001f;
        if (score > best_score) {
            best = &track;
        }
    }
    return best->box;
}

Rect CropController::build_target_crop(
    const Size& source_size,
    const Rect& subject,
    const AutoFramingSettings& settings) const
{
    const PresetMargins margins = margins_for_preset(settings.framing_preset);
    const Rect focus_region = focus_region_for_preset(subject, settings.framing_preset);
    Rect target = make_centered_rect(
        focus_region.center_x(),
        focus_region.center_y() + focus_region.height * margins.center_y_offset_boxes,
        focus_region.width * margins.width_multiplier,
        focus_region.height * margins.height_multiplier);

    target = preserve_aspect(target, source_size.aspect());
    return enforce_zoom_and_bounds(target, source_size, settings.max_zoom);
}

Rect CropController::full_frame_crop(const Size& source_size) const
{
    return {0.0f, 0.0f, source_size.width, source_size.height};
}

Rect CropController::preserve_aspect(Rect rect, float aspect) const
{
    if (!rect.valid() || aspect <= 0.0f) {
        return rect;
    }

    const float current_aspect = rect.width / rect.height;
    const float center_x = rect.center_x();
    const float center_y = rect.center_y();

    if (current_aspect < aspect) {
        rect.width = rect.height * aspect;
    } else {
        rect.height = rect.width / aspect;
    }

    rect.x = center_x - rect.width * 0.5f;
    rect.y = center_y - rect.height * 0.5f;
    return rect;
}

Rect CropController::enforce_zoom_and_bounds(Rect rect, const Size& source_size, double max_zoom) const
{
    if (!source_size.valid()) {
        return {};
    }

    if (!rect.valid()) {
        return full_frame_crop(source_size);
    }

    const float aspect = source_size.aspect();
    rect = preserve_aspect(rect, aspect);

    const float min_width = source_size.width / static_cast<float>(std::max(1.0, max_zoom));
    const float min_height = source_size.height / static_cast<float>(std::max(1.0, max_zoom));
    float width = std::max(rect.width, min_width);
    float height = width / aspect;

    if (height < min_height) {
        height = min_height;
        width = height * aspect;
    }

    if (width > source_size.width) {
        width = source_size.width;
        height = width / aspect;
    }
    if (height > source_size.height) {
        height = source_size.height;
        width = height * aspect;
    }

    rect = make_centered_rect(rect.center_x(), rect.center_y(), width, height);
    return clamp_rect_to_size(rect, source_size);
}

Rect CropController::apply_dead_zone(Rect target, double dead_zone) const
{
    if (!has_crop_ || dead_zone <= 0.0 || !target.valid()) {
        return target;
    }

    const float outer_radius_x = current_crop_.width * static_cast<float>(dead_zone) * 0.5f;
    const float outer_radius_y = current_crop_.height * static_cast<float>(dead_zone) * 0.5f;
    const float softened_center_x = soft_dead_zone_center(current_crop_.center_x(), target.center_x(), outer_radius_x);
    const float softened_center_y = soft_dead_zone_center(current_crop_.center_y(), target.center_y(), outer_radius_y);
    target.x = softened_center_x - target.width * 0.5f;
    target.y = softened_center_y - target.height * 0.5f;

    return target;
}

} // namespace autoframing
