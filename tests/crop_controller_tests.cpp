#include "crop_controller.hpp"
#include "letterbox.hpp"
#include "yuv.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace autoframing;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(float actual, float expected, float tolerance, const std::string& message)
{
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
    }
}

PersonTrack track(int id, Rect box)
{
    PersonTrack person;
    person.id = id;
    person.box = box;
    person.confidence = 0.9f;
    return person;
}

void crop_stays_in_bounds_and_respects_max_zoom()
{
    CropController controller;
    AutoFramingSettings settings = default_settings();
    settings.max_zoom = 2.0;
    settings.tracking_speed = 1.0;
    settings.dead_zone = 0.0;

    const Rect crop = controller.update({1920.0f, 1080.0f}, {track(1, {900.0f, 420.0f, 80.0f, 220.0f})}, settings, 1.0);

    require(crop.x >= 0.0f && crop.y >= 0.0f, "crop min bounds");
    require(crop.right() <= 1920.0f && crop.bottom() <= 1080.0f, "crop max bounds");
    require(crop.width >= 960.0f && crop.height >= 540.0f, "max zoom is enforced");
    require_near(crop.width / crop.height, 16.0f / 9.0f, 0.001f, "aspect ratio is preserved");
}

void crop_eases_back_to_full_frame_without_target()
{
    CropController controller;
    AutoFramingSettings settings = default_settings();
    settings.max_zoom = 3.0;
    settings.tracking_speed = 1.0;
    settings.dead_zone = 0.0;

    controller.update({1280.0f, 720.0f}, {track(1, {520.0f, 210.0f, 120.0f, 320.0f})}, settings, 1.0);
    Rect crop = {};
    for (int i = 0; i < 90; ++i) {
        crop = controller.update({1280.0f, 720.0f}, {}, settings, 1.0 / 60.0);
    }

    require_near(crop.x, 0.0f, 2.5f, "no-target x returns to full frame");
    require_near(crop.y, 0.0f, 2.5f, "no-target y returns to full frame");
    require_near(crop.width, 1280.0f, 2.5f, "no-target width returns to full frame");
    require_near(crop.height, 720.0f, 2.5f, "no-target height returns to full frame");
}

void group_mode_contains_people()
{
    CropController controller;
    AutoFramingSettings settings = default_settings();
    settings.tracking_mode = TrackingMode::Group;
    settings.max_zoom = 4.0;

    const Rect crop = controller.update(
        {1920.0f, 1080.0f},
        {track(1, {220.0f, 350.0f, 170.0f, 430.0f}), track(2, {1450.0f, 330.0f, 180.0f, 450.0f})},
        settings,
        1.0);

    require(crop.left() <= 220.0f, "group crop contains left subject");
    require(crop.right() >= 1630.0f, "group crop contains right subject");
}

void dead_zone_holds_center_for_small_motion()
{
    CropController controller;
    AutoFramingSettings settings = default_settings();
    settings.max_zoom = 3.0;
    settings.tracking_speed = 1.0;
    settings.dead_zone = 0.4;

    Rect first = controller.update({1920.0f, 1080.0f}, {track(1, {830.0f, 320.0f, 240.0f, 500.0f})}, settings, 1.0);
    Rect second = controller.update({1920.0f, 1080.0f}, {track(1, {850.0f, 320.0f, 240.0f, 500.0f})}, settings, 1.0);

    require_near(second.center_x(), first.center_x(), 0.5f, "dead zone keeps crop center stable");
}

void letterboxed_model_box_maps_to_source_coordinates()
{
    LetterboxInfo letterbox;
    letterbox.input_width = 416;
    letterbox.input_height = 416;
    letterbox.resized_width = 416;
    letterbox.resized_height = 312;
    letterbox.scale = 0.65f;
    letterbox.pad_x = 0.0f;
    letterbox.pad_y = 0.0f;

    const Rect source_box = map_letterboxed_model_box_to_source({65.0f, 32.5f, 130.0f, 65.0f}, letterbox, {640.0f, 480.0f});

    require_near(source_box.x, 100.0f, 0.001f, "letterbox x maps to source");
    require_near(source_box.y, 50.0f, 0.001f, "letterbox y maps to source");
    require_near(source_box.width, 200.0f, 0.001f, "letterbox width maps to source");
    require_near(source_box.height, 100.0f, 0.001f, "letterbox height maps to source");
}

void packed_yuv_odd_trailing_pixel_does_not_require_second_pixel_bytes()
{
    const uint8_t yuy2_tail[] = {90, 120};
    const PackedYuvGroup yuy2 = decode_packed_yuv_group(yuy2_tail, PackedYuvFormat::Yuy2, false);
    require(yuy2.y0 == 90, "YUY2 trailing pixel Y is decoded");
    require(yuy2.u == 120, "YUY2 trailing pixel U is decoded");
    require(yuy2.v == 128, "YUY2 trailing pixel missing V is neutral");
    require(!yuy2.has_second_pixel, "YUY2 trailing pixel has no second pixel");

    const uint8_t uyvy_tail[] = {121, 91};
    const PackedYuvGroup uyvy = decode_packed_yuv_group(uyvy_tail, PackedYuvFormat::Uyvy, false);
    require(uyvy.y0 == 91, "UYVY trailing pixel Y is decoded");
    require(uyvy.u == 121, "UYVY trailing pixel U is decoded");
    require(uyvy.v == 128, "UYVY trailing pixel missing V is neutral");
    require(!uyvy.has_second_pixel, "UYVY trailing pixel has no second pixel");

    const uint8_t yvyu_tail[] = {92, 122};
    const PackedYuvGroup yvyu = decode_packed_yuv_group(yvyu_tail, PackedYuvFormat::Yvyu, false);
    require(yvyu.y0 == 92, "YVYU trailing pixel Y is decoded");
    require(yvyu.u == 128, "YVYU trailing pixel missing U is neutral");
    require(yvyu.v == 122, "YVYU trailing pixel V is decoded");
    require(!yvyu.has_second_pixel, "YVYU trailing pixel has no second pixel");
}

void packed_yuv_even_pixel_pair_still_decodes_both_pixels()
{
    const uint8_t yuy2_pair[] = {90, 120, 91, 122};
    const PackedYuvGroup yuy2 = decode_packed_yuv_group(yuy2_pair, PackedYuvFormat::Yuy2, true);
    require(yuy2.y0 == 90 && yuy2.y1 == 91, "YUY2 pair Ys are decoded");
    require(yuy2.u == 120 && yuy2.v == 122, "YUY2 pair chroma is decoded");
    require(yuy2.has_second_pixel, "YUY2 pair has second pixel");

    const uint8_t uyvy_pair[] = {120, 90, 122, 91};
    const PackedYuvGroup uyvy = decode_packed_yuv_group(uyvy_pair, PackedYuvFormat::Uyvy, true);
    require(uyvy.y0 == 90 && uyvy.y1 == 91, "UYVY pair Ys are decoded");
    require(uyvy.u == 120 && uyvy.v == 122, "UYVY pair chroma is decoded");
    require(uyvy.has_second_pixel, "UYVY pair has second pixel");

    const uint8_t yvyu_pair[] = {90, 122, 91, 120};
    const PackedYuvGroup yvyu = decode_packed_yuv_group(yvyu_pair, PackedYuvFormat::Yvyu, true);
    require(yvyu.y0 == 90 && yvyu.y1 == 91, "YVYU pair Ys are decoded");
    require(yvyu.u == 120 && yvyu.v == 122, "YVYU pair chroma is decoded");
    require(yvyu.has_second_pixel, "YVYU pair has second pixel");
}

} // namespace

int main()
{
    try {
        crop_stays_in_bounds_and_respects_max_zoom();
        crop_eases_back_to_full_frame_without_target();
        group_mode_contains_people();
        dead_zone_holds_center_for_small_motion();
        letterboxed_model_box_maps_to_source_coordinates();
        packed_yuv_odd_trailing_pixel_does_not_require_second_pixel_bytes();
        packed_yuv_even_pixel_pair_still_decodes_both_pixels();
    } catch (const std::exception& error) {
        std::cerr << "crop_controller_tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "crop_controller_tests passed\n";
    return 0;
}
