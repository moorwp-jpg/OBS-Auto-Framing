#pragma once

#include "geometry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace autoframing {

struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp_ns = 0;
    uint32_t rgba_stride = 0;
    std::vector<uint8_t> rgba;

    bool has_rgba() const
    {
        return width > 0 && height > 0 && rgba_stride >= width * 4 && rgba.size() >= static_cast<size_t>(rgba_stride) * height;
    }
};

struct Detection {
    Rect box;
    float confidence = 0.0f;
    int class_id = 0;
};

class PersonDetector {
public:
    virtual ~PersonDetector() = default;
    virtual std::vector<Detection> detect(const Frame& frame) = 0;
};

class NullPersonDetector final : public PersonDetector {
public:
    std::vector<Detection> detect(const Frame&) override { return {}; }
};

class MockPersonDetector final : public PersonDetector {
public:
    void set_enabled(bool enabled) { enabled_ = enabled; }
    std::vector<Detection> detect(const Frame& frame) override;

private:
    bool enabled_ = true;
};

} // namespace autoframing
