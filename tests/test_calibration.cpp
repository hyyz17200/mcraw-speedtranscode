#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/processing/calibration.hpp>

TEST_CASE("CFA phase follows odd crops") {
    using mcraw::CfaPattern;
    REQUIRE(mcraw::shifted_cfa(CfaPattern::rggb, 0, 0) == CfaPattern::rggb);
    REQUIRE(mcraw::shifted_cfa(CfaPattern::rggb, 1, 0) == CfaPattern::grbg);
    REQUIRE(mcraw::shifted_cfa(CfaPattern::rggb, 0, 1) == CfaPattern::gbrg);
    REQUIRE(mcraw::shifted_cfa(CfaPattern::rggb, 1, 1) == CfaPattern::bggr);
}

TEST_CASE("black and white calibration preserves negative values and super-white") {
    mcraw::RawMosaicU16 input{2, 2, mcraw::CfaPattern::rggb, {50, 100, 150, 250}};
    mcraw::NormalizedCameraMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.cfa = mcraw::CfaPattern::rggb;
    metadata.black_level = {100, 100, 100, 100};
    metadata.white_level = {200, 200, 200, 200};
    metadata.compression_type = 7;
    const auto output = mcraw::calibrate_raw(input, metadata);
    REQUIRE(output.pixels[0] == Catch::Approx(-0.5));
    REQUIRE(output.pixels[1] == Catch::Approx(0.0));
    REQUIRE(output.pixels[2] == Catch::Approx(0.5));
    REQUIRE(output.pixels[3] == Catch::Approx(1.5));

    const auto demosaic = mcraw::calibrate_raw_for_demosaic(input, metadata);
    for (std::size_t i = 0; i < output.pixels.size(); ++i) {
        REQUIRE(demosaic.pixels[i] == output.pixels[i] * 65535.0F);
    }
}
