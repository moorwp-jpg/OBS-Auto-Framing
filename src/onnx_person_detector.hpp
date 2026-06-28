#pragma once

#include "detector.hpp"

#include <memory>
#include <string>

namespace autoframing {

struct OnnxPersonDetectorConfig {
    std::string model_path;
    float score_floor = 0.30f;
    float nms_threshold = 0.45f;
    float min_person_class_score = 0.25f;
    float min_person_class_margin = 0.10f;
    bool require_person_best_class = false;
};

class OnnxPersonDetector final : public PersonDetector {
public:
    explicit OnnxPersonDetector(OnnxPersonDetectorConfig config);
    ~OnnxPersonDetector() override;

    OnnxPersonDetector(const OnnxPersonDetector&) = delete;
    OnnxPersonDetector& operator=(const OnnxPersonDetector&) = delete;

    bool ready() const;
    const std::string& error() const;
    std::vector<Detection> detect(const Frame& frame) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace autoframing
