#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <mcraw/core/error.hpp>
#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/processing/raw_chroma_denoise.hpp>

namespace {

mcraw::NormalizedCameraMetadata metadata() {
    mcraw::NormalizedCameraMetadata value;
    value.width = 12;
    value.height = 12;
    value.cfa = mcraw::CfaPattern::rggb;
    value.black_level = {0, 0, 0, 0};
    value.white_level = {1, 1, 1, 1};
    value.compression_type = 7;
    std::array<mcraw::NoiseModel, 4> profile{};
    profile.fill(mcraw::NoiseModel{0.02, 0.001});
    value.noise_profile = profile;
    return value;
}

} // namespace

TEST_CASE("RAW chroma denoise leaves green samples unchanged and suppresses red noise") {
    auto info = metadata();
    mcraw::RawNormalizedF32 raw{info.width, info.height, info.cfa, {}};
    raw.pixels.assign(static_cast<std::size_t>(raw.width) * raw.height, 0.5F);
    const std::size_t noisy = static_cast<std::size_t>(4U) * raw.width + 4U;
    REQUIRE(mcraw::cfa_color(raw.cfa, 4, 4) == 0U);
    raw.pixels[noisy] = 0.65F;

    const auto filtered = mcraw::denoise_raw_chroma(raw, info, 1.0, 2);
    REQUIRE(std::abs(filtered.pixels[noisy] - 0.5F) < std::abs(raw.pixels[noisy] - 0.5F));
    for (std::uint32_t y = 0; y < raw.height; ++y) {
        for (std::uint32_t x = 0; x < raw.width; ++x) {
            if (mcraw::cfa_color(raw.cfa, x, y) == 1U) {
                const auto index = static_cast<std::size_t>(y) * raw.width + x;
                REQUIRE(filtered.pixels[index] == raw.pixels[index]);
            }
        }
    }
}

TEST_CASE("RAW chroma denoise requires a noise profile only when enabled") {
    auto info = metadata();
    mcraw::RawNormalizedF32 raw{info.width, info.height, info.cfa, {}};
    raw.pixels.assign(static_cast<std::size_t>(raw.width) * raw.height, 0.5F);
    info.noise_profile.reset();
    REQUIRE(mcraw::denoise_raw_chroma(raw, info, 0.0).pixels == raw.pixels);
    REQUIRE_THROWS_AS(mcraw::denoise_raw_chroma(raw, info, 1.0), mcraw::Error);
}
