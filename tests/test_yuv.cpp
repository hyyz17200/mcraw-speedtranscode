#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <mcraw/processing/color.hpp>
#include <mcraw/processing/log_curve.hpp>
#include <mcraw/processing/yuv.hpp>

TEST_CASE("neutral log RGB maps to neutral legal-range YUV") {
    mcraw::TargetLogRgbF32 image{4, 1, {}};
    for (auto& plane : image.planes) plane.assign(4, 0.5F);
    const auto output = mcraw::pack_dwg_log_to_yuv422p10(
        image, mcraw::ChromaFilter::quality, false, 0);
    REQUIRE(output.image.y == std::vector<std::uint16_t>{502, 502, 502, 502});
    REQUIRE(output.image.cb == std::vector<std::uint16_t>{512, 512});
    REQUIRE(output.image.cr == std::vector<std::uint16_t>{512, 512});
    REQUIRE(output.stats.luma_clipped_low == 0);
    REQUIRE(output.stats.luma_clipped_high == 0);
}

TEST_CASE("legal range quantization clips only at packing boundary") {
    mcraw::TargetLogRgbF32 image{2, 1, {}};
    for (auto& plane : image.planes) plane = {-1.0F, 2.0F};
    const auto output = mcraw::pack_dwg_log_to_yuv422p10(
        image, mcraw::ChromaFilter::fast, false, 0);
    REQUIRE(output.image.y[0] == 64);
    REQUIRE(output.image.y[1] == 940);
    REQUIRE(output.stats.luma_clipped_low == 1);
    REQUIRE(output.stats.luma_clipped_high == 1);
}

TEST_CASE("fused CPU color curve and packing matches the reference path") {
    mcraw::CameraRgbF32 camera{8, 4, {}};
    for (auto& plane : camera.planes) plane.resize(32);
    for (std::size_t i = 0; i < 32; ++i) {
        camera.planes[0][i] = static_cast<float>(i) / 31.0F;
        camera.planes[1][i] = 0.1F + static_cast<float>(i % 7U) * 0.08F;
        camera.planes[2][i] = i % 5U == 0U ? -0.01F : 0.75F - static_cast<float>(i) / 64.0F;
    }
    mcraw::CameraColorSolution solution;
    solution.camera_to_target = mcraw::Matrix3d::identity();
    const auto target_linear = mcraw::camera_to_dwg(camera, solution);
    const auto target_log = mcraw::encode_davinci_intermediate(
        target_linear, mcraw::NegativePolicy::preserve_by_curve);
    const auto reference = mcraw::pack_dwg_log_to_yuv422p10(
        target_log, mcraw::ChromaFilter::quality, false, 3);
    const mcraw::DaVinciIntermediateLut curve;
    const auto fused = mcraw::pack_camera_to_dwg_di_yuv422p10(
        camera, solution, 0.0, mcraw::NegativePolicy::preserve_by_curve,
        curve, mcraw::ChromaFilter::quality, false, 3, 4);
    const auto compare_plane = [](const auto& expected, const auto& actual) {
        REQUIRE(expected.size() == actual.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(std::abs(static_cast<int>(expected[i]) - static_cast<int>(actual[i])) <= 1);
        }
    };
    compare_plane(reference.image.y, fused.image.y);
    compare_plane(reference.image.cb, fused.image.cb);
    compare_plane(reference.image.cr, fused.image.cr);
}

TEST_CASE("fused CPU packing is deterministic across thread counts") {
    mcraw::CameraRgbF32 camera{16, 8, {}};
    for (std::size_t channel = 0; channel < camera.planes.size(); ++channel) {
        camera.planes[channel].resize(128);
        for (std::size_t i = 0; i < 128; ++i) {
            camera.planes[channel][i] = static_cast<float>((i * (channel + 3U)) % 97U) / 80.0F;
        }
    }
    mcraw::CameraColorSolution solution;
    solution.camera_to_target = mcraw::Matrix3d{{
        1.1, -0.1, 0.02, -0.03, 1.04, -0.01, 0.01, -0.08, 1.07
    }};
    const mcraw::DaVinciIntermediateLut curve;
    const auto serial = mcraw::pack_camera_to_dwg_di_yuv422p10(
        camera, solution, 0.25, mcraw::NegativePolicy::preserve_by_curve,
        curve, mcraw::ChromaFilter::quality, true, 19, 1);
    const auto parallel = mcraw::pack_camera_to_dwg_di_yuv422p10(
        camera, solution, 0.25, mcraw::NegativePolicy::preserve_by_curve,
        curve, mcraw::ChromaFilter::quality, true, 19, 8);
    REQUIRE(parallel.image.y == serial.image.y);
    REQUIRE(parallel.image.cb == serial.image.cb);
    REQUIRE(parallel.image.cr == serial.image.cr);
    REQUIRE(parallel.stats.luma_clipped_low == serial.stats.luma_clipped_low);
    REQUIRE(parallel.stats.luma_clipped_high == serial.stats.luma_clipped_high);
    REQUIRE(parallel.stats.chroma_clipped == serial.stats.chroma_clipped);
}

TEST_CASE("capture sharpening increases luma edge contrast without adding chroma") {
    mcraw::CameraRgbF32 camera{8, 4, {}};
    for (auto& plane : camera.planes) {
        plane.resize(32);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                plane[static_cast<std::size_t>(y) * 8U + x] = x < 4U ? 0.2F : 0.8F;
            }
        }
    }
    mcraw::CameraColorSolution solution;
    solution.camera_to_target = mcraw::Matrix3d::identity();
    const mcraw::DaVinciIntermediateLut curve;
    const auto baseline = mcraw::pack_camera_to_dwg_di_yuv422p10(
        camera, solution, 0.0, mcraw::NegativePolicy::preserve_by_curve,
        curve, mcraw::ChromaFilter::quality, false, 0, 2);
    const auto sharpened = mcraw::pack_camera_to_dwg_di_yuv422p10(
        camera, solution, 0.0, mcraw::NegativePolicy::preserve_by_curve,
        curve, mcraw::ChromaFilter::quality, false, 0, 2, 0.25, 0.0);
    REQUIRE(sharpened.image.y[3] < baseline.image.y[3]);
    REQUIRE(sharpened.image.y[4] > baseline.image.y[4]);
    for (const auto value : sharpened.image.cb) REQUIRE(value == 512);
    for (const auto value : sharpened.image.cr) REQUIRE(value == 512);
}
