#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/metadata.hpp>
#include <mcraw/processing/color.hpp>

TEST_CASE("3x3 inverse composes to identity") {
    const mcraw::Matrix3d matrix{{1.2, 0.1, -0.2, 0.3, 0.8, 0.1, -0.1, 0.2, 1.1}};
    const auto identity = matrix * matrix.inverse();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            REQUIRE(identity.v[row * 3 + column] ==
                    Catch::Approx(row == column ? 1.0 : 0.0).margin(1.0e-10));
        }
    }
}

TEST_CASE("CCT solver locates D65 near its standard temperature") {
    REQUIRE(mcraw::correlated_color_temperature(0.3127, 0.3290) ==
            Catch::Approx(6504.0).margin(150.0));
}

TEST_CASE("identity camera profile converges for a D65 neutral") {
    mcraw::NormalizedCameraMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.black_level = {0, 0, 0, 0};
    metadata.white_level = {1, 1, 1, 1};
    metadata.camera_neutral = {0.9504559, 1.0, 1.0890578};
    metadata.color_matrix1 = mcraw::MatrixMetadata{{1,0,0, 0,1,0, 0,0,1}, mcraw::MetadataSource::container};
    metadata.illuminant1_cct = 6504.0;
    metadata.compression_type = 7;
    const auto white = mcraw::solve_camera_neutral(metadata);
    REQUIRE(white.x == Catch::Approx(0.3127).margin(1.0e-5));
    REQUIRE(white.y == Catch::Approx(0.3290).margin(1.0e-5));
}

