#include <mcraw/core/pixel_types.hpp>

#include <algorithm>
#include <cctype>
#include <string>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

std::string lower(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::size_t checked_pixels(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        throw Error(ErrorCode::invalid_argument, "image dimensions must be non-zero");
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

} // namespace

CfaPattern parse_cfa(std::string_view value) {
    const auto normalized = lower(value);
    if (normalized == "rggb") return CfaPattern::rggb;
    if (normalized == "bggr") return CfaPattern::bggr;
    if (normalized == "grbg") return CfaPattern::grbg;
    if (normalized == "gbrg") return CfaPattern::gbrg;
    throw Error(ErrorCode::unsupported_format, "unsupported CFA arrangement: " + std::string(value));
}

std::string_view to_string(CfaPattern value) noexcept {
    switch (value) {
    case CfaPattern::rggb: return "rggb";
    case CfaPattern::bggr: return "bggr";
    case CfaPattern::grbg: return "grbg";
    case CfaPattern::gbrg: return "gbrg";
    }
    return "unknown";
}

unsigned cfa_color(CfaPattern value, std::uint32_t x, std::uint32_t y) noexcept {
    static constexpr unsigned patterns[4][2][2] = {
        {{0, 1}, {1, 2}},
        {{2, 1}, {1, 0}},
        {{1, 0}, {2, 1}},
        {{1, 2}, {0, 1}},
    };
    return patterns[static_cast<unsigned>(value)][y & 1U][x & 1U];
}

CfaPattern shifted_cfa(CfaPattern value, std::uint32_t x, std::uint32_t y) noexcept {
    const unsigned c00 = cfa_color(value, x, y);
    const unsigned c10 = cfa_color(value, x + 1U, y);
    if (c00 == 0U) return CfaPattern::rggb;
    if (c00 == 2U) return CfaPattern::bggr;
    if (c10 == 0U) return CfaPattern::grbg;
    return CfaPattern::gbrg;
}

void RawMosaicU16::validate() const {
    if (pixels.size() != checked_pixels(width, height)) {
        throw Error(ErrorCode::invalid_argument, "RawMosaicU16 buffer size does not match dimensions");
    }
}

void RawNormalizedF32::validate() const {
    if (pixels.size() != checked_pixels(width, height)) {
        throw Error(ErrorCode::invalid_argument, "RawNormalizedF32 buffer size does not match dimensions");
    }
}

void PlanarRgbF32::validate() const {
    const auto count = checked_pixels(width, height);
    for (const auto& plane : planes) {
        if (plane.size() != count) {
            throw Error(ErrorCode::invalid_argument, "RGB plane size does not match dimensions");
        }
    }
}

void Yuv422P10::validate() const {
    if ((width & 1U) != 0U) {
        throw Error(ErrorCode::invalid_argument, "4:2:2 output width must be even");
    }
    const auto luma_count = checked_pixels(width, height);
    const auto chroma_count = luma_count / 2U;
    if (y.size() != luma_count || cb.size() != chroma_count || cr.size() != chroma_count) {
        throw Error(ErrorCode::invalid_argument, "YUV422 plane size does not match dimensions");
    }
}

} // namespace mcraw

