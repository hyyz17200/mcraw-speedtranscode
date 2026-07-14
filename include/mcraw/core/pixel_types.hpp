#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mcraw {

enum class CfaPattern { rggb, bggr, grbg, gbrg };

[[nodiscard]] CfaPattern parse_cfa(std::string_view value);
[[nodiscard]] std::string_view to_string(CfaPattern value) noexcept;
[[nodiscard]] CfaPattern shifted_cfa(CfaPattern value, std::uint32_t x, std::uint32_t y) noexcept;
[[nodiscard]] unsigned cfa_color(CfaPattern value, std::uint32_t x, std::uint32_t y) noexcept;

struct CompressedFrame {
    std::int64_t timestamp_ns{};
    std::vector<std::uint8_t> bytes;
};

struct RawMosaicU16 {
    std::uint32_t width{};
    std::uint32_t height{};
    CfaPattern cfa{CfaPattern::rggb};
    std::vector<std::uint16_t> pixels;

    void validate() const;
};

struct RawNormalizedF32 {
    std::uint32_t width{};
    std::uint32_t height{};
    CfaPattern cfa{CfaPattern::rggb};
    std::vector<float> pixels;

    void validate() const;
};

struct PlanarRgbF32 {
    std::uint32_t width{};
    std::uint32_t height{};
    std::array<std::vector<float>, 3> planes;

    void validate() const;
};

using CameraRgbF32 = PlanarRgbF32;
using XyzD50F32 = PlanarRgbF32;
using TargetLinearRgbF32 = PlanarRgbF32;
using TargetLogRgbF32 = PlanarRgbF32;

struct Yuv422P10 {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> cb;
    std::vector<std::uint16_t> cr;

    void validate() const;
};

} // namespace mcraw

