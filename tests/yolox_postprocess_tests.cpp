#include "yolox_postprocess.hpp"

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

YoloXPostprocessConfig test_config()
{
    YoloXPostprocessConfig config;
    config.score_floor = 0.05f;
    config.nms_threshold = 0.45f;
    config.min_person_class_score = 0.20f;
    config.min_person_class_margin = 0.05f;
    config.require_person_best_class = false;
    return config;
}

LetterboxInfo identity_letterbox()
{
    LetterboxInfo letterbox;
    letterbox.input_width = 416;
    letterbox.input_height = 416;
    letterbox.resized_width = 416;
    letterbox.resized_height = 416;
    letterbox.scale = 1.0f;
    letterbox.pad_x = 0.0f;
    letterbox.pad_y = 0.0f;
    return letterbox;
}

std::vector<float> row(float objectness, float person, float chair, float bottle)
{
    return {
        340.0f,
        208.0f,
        96.0f,
        180.0f,
        objectness,
        person,
        chair,
        bottle,
    };
}

std::vector<Detection> postprocess_one_row(const std::vector<float>& output, const YoloXPostprocessConfig& config)
{
    return postprocess_yolox_person_detections(
        output.data(),
        1,
        8,
        416,
        416,
        identity_letterbox(),
        {416.0f, 416.0f},
        config);
}

void person_best_class_is_accepted()
{
    const YoloXPostprocessConfig config = test_config();
    const std::vector<Detection> detections = postprocess_one_row(row(0.90f, 0.80f, 0.20f, 0.10f), config);

    require(detections.size() == 1, "person best class creates one detection");
    require(detections.front().class_id == 0, "postprocess keeps output as person class");
    require_near(detections.front().confidence, 0.72f, 0.001f, "confidence is objectness times person class score");
}

void non_person_best_class_rejects_weak_person_score()
{
    const YoloXPostprocessConfig config = test_config();

    const std::vector<Detection> chair = postprocess_one_row(row(0.95f, 0.24f, 0.82f, 0.12f), config);
    require(chair.empty(), "chair best class rejects a weak person score even above score floor");

    const std::vector<Detection> bottle = postprocess_one_row(row(0.95f, 0.23f, 0.10f, 0.78f), config);
    require(bottle.empty(), "bottle best class rejects a weak person score even above score floor");
}

void person_near_best_class_only_within_margin()
{
    YoloXPostprocessConfig config = test_config();
    config.min_person_class_margin = 0.05f;

    std::vector<Detection> detections = postprocess_one_row(row(0.90f, 0.70f, 0.73f, 0.10f), config);
    require(detections.size() == 1, "person near-best class is accepted within margin");

    detections = postprocess_one_row(row(0.90f, 0.70f, 0.78f, 0.10f), config);
    require(detections.empty(), "person near-best class is rejected outside margin");
}

} // namespace

int main()
{
    try {
        person_best_class_is_accepted();
        non_person_best_class_rejects_weak_person_score();
        person_near_best_class_only_within_margin();
    } catch (const std::exception& error) {
        std::cerr << "yolox_postprocess_tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "yolox_postprocess_tests passed\n";
    return 0;
}
