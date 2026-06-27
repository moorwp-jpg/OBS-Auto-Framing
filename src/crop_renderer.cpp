#include "crop_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <graphics/vec4.h>
#include <util/platform.h>

namespace autoframing {
namespace {

struct Color {
    float r;
    float g;
    float b;
    float a;
};

Rect source_to_output_rect(const Rect& rect, const Rect& crop, uint32_t output_width, uint32_t output_height)
{
    if (!rect.valid() || !crop.valid()) {
        return {};
    }

    const float scale_x = static_cast<float>(output_width) / crop.width;
    const float scale_y = static_cast<float>(output_height) / crop.height;
    return {
        (rect.x - crop.x) * scale_x,
        (rect.y - crop.y) * scale_y,
        rect.width * scale_x,
        rect.height * scale_y,
    };
}

void set_solid_color(gs_effect_t* solid, Color color)
{
    vec4 value;
    value.x = color.r;
    value.y = color.g;
    value.z = color.b;
    value.w = color.a;
    gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &value);
}

void draw_filled_rect(float x, float y, float width, float height, gs_effect_t* solid, Color color)
{
    if (width <= 0.0f || height <= 0.0f || solid == nullptr) {
        return;
    }

    set_solid_color(solid, color);
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    while (gs_effect_loop(solid, "Solid")) {
        gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(std::ceil(width)), static_cast<uint32_t>(std::ceil(height)));
    }
    gs_matrix_pop();
}

void draw_rect_outline(const Rect& rect, float thickness, gs_effect_t* solid, Color color)
{
    if (!rect.valid() || solid == nullptr) {
        return;
    }

    const float line = std::max(1.0f, thickness);
    draw_filled_rect(rect.x, rect.y, rect.width, line, solid, color);
    draw_filled_rect(rect.x, rect.bottom() - line, rect.width, line, solid, color);
    draw_filled_rect(rect.x, rect.y, line, rect.height, solid, color);
    draw_filled_rect(rect.right() - line, rect.y, line, rect.height, solid, color);
}

bool digit_segment_enabled(int digit, int segment)
{
    static constexpr bool segments[10][7] = {
        {true, true, true, true, true, true, false},
        {false, true, true, false, false, false, false},
        {true, true, false, true, true, false, true},
        {true, true, true, true, false, false, true},
        {false, true, true, false, false, true, true},
        {true, false, true, true, false, true, true},
        {true, false, true, true, true, true, true},
        {true, true, true, false, false, false, false},
        {true, true, true, true, true, true, true},
        {true, true, true, true, false, true, true},
    };

    return digit >= 0 && digit <= 9 && segment >= 0 && segment < 7 && segments[digit][segment];
}

void draw_digit(int digit, float x, float y, float scale, gs_effect_t* solid, Color color)
{
    const float t = std::max(1.0f, scale);
    const float w = 3.0f * scale;
    const float h = 2.0f * scale;

    const Rect segment_rects[7] = {
        {x + t, y, w, t},
        {x + 4.0f * scale, y + t, t, h},
        {x + 4.0f * scale, y + 4.0f * scale, t, h},
        {x + t, y + 6.0f * scale, w, t},
        {x, y + 4.0f * scale, t, h},
        {x, y + t, t, h},
        {x + t, y + 3.0f * scale, w, t},
    };

    for (int segment = 0; segment < 7; ++segment) {
        if (digit_segment_enabled(digit, segment)) {
            draw_filled_rect(
                segment_rects[segment].x,
                segment_rects[segment].y,
                segment_rects[segment].width,
                segment_rects[segment].height,
                solid,
                color);
        }
    }
}

float draw_number(int value, float x, float y, float scale, gs_effect_t* solid, Color color)
{
    value = std::max(0, value);
    char buffer[16] = {};
    snprintf(buffer, sizeof(buffer), "%d", value);

    float cursor = x;
    for (const char* ch = buffer; *ch != '\0'; ++ch) {
        if (*ch >= '0' && *ch <= '9') {
            draw_digit(*ch - '0', cursor, y, scale, solid, color);
        }
        cursor += 6.0f * scale;
    }

    return cursor;
}

void draw_track_label(const PersonTrack& track, const Rect& output_box, gs_effect_t* solid)
{
    const float scale = 2.0f;
    const float label_x = std::max(2.0f, output_box.x);
    const float id_y = std::max(2.0f, output_box.y - 30.0f);
    const float score_y = id_y + 15.0f;

    const Color id_color{0.45f, 1.0f, 0.35f, 0.95f};
    const Color confidence_color{1.0f, 0.88f, 0.25f, 0.95f};

    draw_number(track.id, label_x, id_y, scale, solid, id_color);
    draw_number(static_cast<int>(std::round(track.confidence * 100.0f)), label_x, score_y, scale, solid, confidence_color);
}

} // namespace

CropRenderer::~CropRenderer()
{
    if (crop_effect_ != nullptr) {
        obs_enter_graphics();
        gs_effect_destroy(crop_effect_);
        obs_leave_graphics();
        crop_effect_ = nullptr;
    }
}

bool CropRenderer::ensure_effect()
{
    if (crop_effect_ != nullptr) {
        return true;
    }

    char* effect_path = obs_module_file("effects/crop.effect");
    if (effect_path == nullptr) {
        blog(LOG_ERROR, "[obs-auto-framing] failed to resolve effects/crop.effect from plugin data directory");
        return false;
    }

    char* errors = nullptr;
    crop_effect_ = gs_effect_create_from_file(effect_path, &errors);

    if (errors != nullptr) {
        blog(LOG_WARNING, "[obs-auto-framing] crop effect messages: %s", errors);
        bfree(errors);
    }
    if (effect_path != nullptr) {
        bfree(effect_path);
    }

    if (crop_effect_ == nullptr) {
        blog(LOG_WARNING, "[obs-auto-framing] failed to load crop effect");
        return false;
    }

    crop_min_param_ = gs_effect_get_param_by_name(crop_effect_, "crop_min");
    crop_size_param_ = gs_effect_get_param_by_name(crop_effect_, "crop_size");
    if (crop_min_param_ == nullptr || crop_size_param_ == nullptr) {
        blog(LOG_ERROR, "[obs-auto-framing] crop effect is missing required crop_min/crop_size parameters");
        gs_effect_destroy(crop_effect_);
        crop_effect_ = nullptr;
        crop_min_param_ = nullptr;
        crop_size_param_ = nullptr;
        return false;
    }

    blog(LOG_INFO, "[obs-auto-framing] crop effect loaded");
    return true;
}

bool CropRenderer::initialize()
{
    obs_enter_graphics();
    const bool ok = ensure_effect();
    obs_leave_graphics();
    return ok;
}

void CropRenderer::render(
    obs_source_t* filter_source,
    const Rect& crop,
        uint32_t source_width,
        uint32_t source_height,
        bool debug_overlay,
        const DebugOverlayData& debug_data,
        gs_effect_t* fallback_effect)
{
    if (filter_source == nullptr) {
        return;
    }

    if (source_width == 0 || source_height == 0 || !crop.valid()) {
        obs_source_skip_video_filter(filter_source);
        return;
    }

    gs_effect_t* effect = ensure_effect() ? crop_effect_ : fallback_effect;
    if (effect == nullptr) {
        obs_source_skip_video_filter(filter_source);
        return;
    }

    if (effect == crop_effect_ && (crop_min_param_ == nullptr || crop_size_param_ == nullptr)) {
        obs_source_skip_video_filter(filter_source);
        return;
    }

    // The crop shader samples OBS's intermediate "image" texture, so direct rendering is not safe here.
    if (!obs_source_process_filter_begin(filter_source, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
        obs_source_skip_video_filter(filter_source);
        return;
    }

    if (effect == crop_effect_) {
        vec2 crop_min;
        vec2 crop_size;
        vec2_set(&crop_min, crop.x / static_cast<float>(source_width), crop.y / static_cast<float>(source_height));
        vec2_set(
            &crop_size,
            crop.width / static_cast<float>(source_width),
            crop.height / static_cast<float>(source_height));

        gs_effect_set_vec2(crop_min_param_, &crop_min);
        gs_effect_set_vec2(crop_size_param_, &crop_size);
    }

    obs_source_process_filter_end(filter_source, effect, source_width, source_height);

    if (debug_overlay) {
        render_debug_overlay(crop, source_width, source_height, debug_data);
    }
}

void CropRenderer::render_debug_overlay(const Rect& crop, uint32_t source_width, uint32_t source_height, const DebugOverlayData& data)
{
    if (!crop.valid() || source_width == 0 || source_height == 0) {
        return;
    }

    gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (solid == nullptr) {
        return;
    }

    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
    gs_enable_depth_test(false);

    gs_projection_push();
    gs_ortho(0.0f, static_cast<float>(source_width), 0.0f, static_cast<float>(source_height), -100.0f, 100.0f);
    gs_matrix_push();
    gs_matrix_identity();

    const Color detection_color{0.15f, 0.72f, 1.0f, 0.90f};
    const Color track_color{0.45f, 1.0f, 0.35f, 0.95f};
    const Color current_crop_color{1.0f, 1.0f, 1.0f, 0.80f};
    const Color target_crop_color{1.0f, 0.25f, 0.85f, 0.85f};
    const Color dead_zone_color{1.0f, 0.88f, 0.25f, 0.70f};

    for (const Detection& detection : data.detections) {
        draw_rect_outline(source_to_output_rect(detection.box, crop, source_width, source_height), 2.0f, solid, detection_color);
    }

    for (const PersonTrack& track : data.tracks) {
        const Rect output_box = source_to_output_rect(track.box, crop, source_width, source_height);
        draw_rect_outline(output_box, 3.0f, solid, track_color);
        draw_track_label(track, output_box, solid);
    }

    draw_rect_outline({0.0f, 0.0f, static_cast<float>(source_width), static_cast<float>(source_height)}, 2.0f, solid, current_crop_color);

    if (data.target_crop.valid()) {
        draw_rect_outline(source_to_output_rect(data.target_crop, crop, source_width, source_height), 2.0f, solid, target_crop_color);
    }

    if (data.dead_zone > 0.0 && data.current_crop.valid()) {
        const Rect dead_zone = make_centered_rect(
            data.current_crop.center_x(),
            data.current_crop.center_y(),
            data.current_crop.width * static_cast<float>(data.dead_zone),
            data.current_crop.height * static_cast<float>(data.dead_zone));
        draw_rect_outline(source_to_output_rect(dead_zone, crop, source_width, source_height), 1.0f, solid, dead_zone_color);
    }

    gs_matrix_pop();
    gs_projection_pop();
    gs_blend_state_pop();
}

} // namespace autoframing
