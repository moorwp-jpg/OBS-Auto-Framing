#include "yuv.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace autoframing;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void i422_chroma_index_reuses_chroma_for_horizontal_pairs()
{
    require(i422_chroma_index(0) == 0, "I422 first pixel uses first chroma sample");
    require(i422_chroma_index(1) == 0, "I422 second pixel reuses first chroma sample");
    require(i422_chroma_index(2) == 1, "I422 third pixel uses second chroma sample");
    require(i422_chroma_index(3) == 1, "I422 fourth pixel reuses second chroma sample");
}

void i444_chroma_index_is_per_pixel()
{
    require(i444_chroma_index(0) == 0, "I444 first pixel uses first chroma sample");
    require(i444_chroma_index(1) == 1, "I444 second pixel uses second chroma sample");
    require(i444_chroma_index(17) == 17, "I444 chroma is not horizontally subsampled");
}

void i010_samples_are_lsb_aligned_10_bit_values()
{
    require(i010_sample_to_u8(0) == 0, "I010 black maps to 8-bit zero");
    require(i010_sample_to_u8(64) == 16, "I010 limited-range luma floor maps near 16");
    require(i010_sample_to_u8(512) == 128, "I010 midpoint maps near 128");
    require(i010_sample_to_u8(940) == 235, "I010 limited-range luma ceiling maps to 235");
    require(i010_sample_to_u8(1023) == 255, "I010 max maps to 8-bit max");
    require(i010_sample_to_u8(1200) == 255, "I010 out-of-range samples clamp");
}

void p010_samples_are_msb_aligned_10_bit_values()
{
    require(p010_sample_to_u8(static_cast<uint16_t>(0 << 6)) == 0, "P010 black maps to 8-bit zero");
    require(p010_sample_to_u8(static_cast<uint16_t>(64 << 6)) == 16, "P010 limited-range luma floor maps near 16");
    require(p010_sample_to_u8(static_cast<uint16_t>(512 << 6)) == 128, "P010 midpoint maps near 128");
    require(p010_sample_to_u8(static_cast<uint16_t>(940 << 6)) == 235, "P010 limited-range luma ceiling maps to 235");
    require(p010_sample_to_u8(static_cast<uint16_t>(1023 << 6)) == 255, "P010 max maps to 8-bit max");
    require(p010_sample_to_u8(0xffff) == 255, "P010 out-of-range samples clamp after downshift");
}

void ayuv_unpack_uses_obs_little_endian_memory_order()
{
    const uint8_t bytes[] = {42, 121, 91, 222};
    const AyuvPixel pixel = unpack_ayuv_pixel(bytes);
    require(pixel.y == 91, "AYUV Y channel is unpacked");
    require(pixel.u == 121, "AYUV U channel is unpacked");
    require(pixel.v == 42, "AYUV V channel is unpacked");
    require(pixel.a == 222, "AYUV alpha channel is unpacked");
}

void little_endian_16_bit_reads_are_explicit()
{
    const uint8_t bytes[] = {0x34, 0x12};
    require(read_le16(bytes) == 0x1234, "read_le16 decodes little-endian sample bytes");
}

} // namespace

int main()
{
    try {
        i422_chroma_index_reuses_chroma_for_horizontal_pairs();
        i444_chroma_index_is_per_pixel();
        i010_samples_are_lsb_aligned_10_bit_values();
        p010_samples_are_msb_aligned_10_bit_values();
        ayuv_unpack_uses_obs_little_endian_memory_order();
        little_endian_16_bit_reads_are_explicit();
    } catch (const std::exception& error) {
        std::cerr << "yuv_tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "yuv_tests passed\n";
    return 0;
}
