#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

#include <mcraw/processing/log_curve.hpp>

TEST_CASE("DaVinci Intermediate matches official reference points") {
    REQUIRE(mcraw::davinci_intermediate_oetf(0.18) == Catch::Approx(0.336043).margin(1.0e-6));
    REQUIRE(mcraw::davinci_intermediate_oetf(1.0) == Catch::Approx(0.513837).margin(1.0e-6));
    REQUIRE(mcraw::davinci_intermediate_oetf(100.0) == Catch::Approx(1.0).margin(1.0e-6));
}

TEST_CASE("DaVinci Intermediate analytic forward and inverse round trip") {
    constexpr std::array<double, 8> values{-0.01, 0.0, 0.001, 0.00262409, 0.18, 1.0, 10.0, 100.0};
    for (const double value : values) {
        REQUIRE(mcraw::davinci_intermediate_eotf(mcraw::davinci_intermediate_oetf(value)) ==
                Catch::Approx(value).margin(1.0e-9));
    }
}

TEST_CASE("DaVinci Intermediate per-pipeline LUT tracks the analytic curve") {
    const mcraw::DaVinciIntermediateLut curve;
    constexpr std::array<float, 11> values{
        -0.25F, 0.0F, 0.001F, 0.00262409F, 0.01F, 0.18F,
        0.999F, 1.0F, 12.5F, 100.0F, 150.0F
    };
    for (const float value : values) {
        REQUIRE(curve.encode(value) == Catch::Approx(
            mcraw::davinci_intermediate_oetf(value)).margin(2.0e-6));
    }
    REQUIRE(curve.entries_per_segment() == 65'536U);
}
