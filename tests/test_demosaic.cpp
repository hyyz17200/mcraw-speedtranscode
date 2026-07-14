#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/processing/demosaic.hpp>

TEST_CASE("all configured Bayer demosaic algorithms produce finite RGB") {
    mcraw::RawNormalizedF32 raw{40, 40, mcraw::CfaPattern::rggb, {}};
    raw.pixels.resize(static_cast<std::size_t>(raw.width) * raw.height);
    for (std::uint32_t y = 0; y < raw.height; ++y) {
        for (std::uint32_t x = 0; x < raw.width; ++x) {
            raw.pixels[static_cast<std::size_t>(y) * raw.width + x] =
                0.1F + 0.8F * static_cast<float>(x + y) /
                    static_cast<float>(raw.width + raw.height - 2U);
        }
    }

    const std::array algorithms{
        mcraw::DemosaicAlgorithm::rcd,
        mcraw::DemosaicAlgorithm::amaze,
        mcraw::DemosaicAlgorithm::igv,
        mcraw::DemosaicAlgorithm::dcb,
        mcraw::DemosaicAlgorithm::lmmse
    };
    for (const auto algorithm : algorithms) {
        const auto rgb = mcraw::demosaic(raw, algorithm, 2);
        rgb.validate();
        for (const auto& plane : rgb.planes) {
            for (const float sample : plane) REQUIRE(std::isfinite(sample));
        }
    }
}
