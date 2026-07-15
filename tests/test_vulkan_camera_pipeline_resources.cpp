#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

#include <mcraw/core/error.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/processing/demosaic.hpp>
#include <mcraw/vulkan/vulkan_camera_pipeline_resources.hpp>

namespace {

mcraw::CameraRgbF32 camera_pattern(std::uint32_t width, std::uint32_t height,
                                   float offset) {
    mcraw::CameraRgbF32 image{width, height, {}};
    const auto pixels = static_cast<std::size_t>(width) * height;
    for (std::size_t channel = 0; channel < image.planes.size(); ++channel) {
        auto& plane = image.planes[channel];
        plane.resize(pixels);
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            plane[pixel] = offset + static_cast<float>(channel * pixels + pixel) * 0.25F;
        }
    }
    return image;
}

struct ErrorStats {
    double maximum{};
    double rmse{};
};

ErrorStats compare_rgb(const mcraw::PlanarRgbF32& expected,
                       const mcraw::PlanarRgbF32& actual) {
    expected.validate();
    actual.validate();
    REQUIRE(expected.width == actual.width);
    REQUIRE(expected.height == actual.height);
    double squared = 0.0;
    std::size_t samples = 0;
    ErrorStats result;
    for (std::size_t channel = 0; channel < expected.planes.size(); ++channel) {
        for (std::size_t pixel = 0; pixel < expected.planes[channel].size(); ++pixel) {
            const double error = std::abs(
                static_cast<double>(expected.planes[channel][pixel]) -
                actual.planes[channel][pixel]);
            result.maximum = std::max(result.maximum, error);
            squared += error * error;
            ++samples;
        }
    }
    result.rmse = std::sqrt(squared / static_cast<double>(samples));
    return result;
}

} // namespace

TEST_CASE("Vulkan Camera RGB Stage 1A resources round-trip two slots exactly") {
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 8;
    constexpr std::size_t slots = 2;
    constexpr std::uint64_t plane_bytes =
        static_cast<std::uint64_t>(width) * height * sizeof(float);
    const bool validation = std::getenv("MCRAW_VULKAN_VALIDATION") != nullptr;
    mcraw::VulkanRuntime runtime({"auto", validation});
    mcraw::VulkanCameraPipelineResources resources(
        runtime, {width, height, slots, true});

    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto input = camera_pattern(width, height, static_cast<float>(iteration));
        const auto output = resources.round_trip_for_test(input);
        CHECK(output.width == input.width);
        CHECK(output.height == input.height);
        CHECK(output.planes == input.planes);
    }

    const auto telemetry = resources.telemetry();
    CHECK(telemetry.slot_count == slots);
    CHECK(telemetry.camera_upload_capacity_bytes == plane_bytes * 3U * slots);
    CHECK(telemetry.intermediate_capacity_bytes == plane_bytes * 6U * slots);
    CHECK(telemetry.test_readback_capacity_bytes == plane_bytes * 3U * slots);
    CHECK(telemetry.test_round_trips == 3U);
    CHECK(telemetry.test_upload_bytes == plane_bytes * 3U * 3U);
    CHECK(telemetry.test_readback_bytes == plane_bytes * 3U * 3U);
}

TEST_CASE("Vulkan Camera RGB production resources reject test readback") {
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanCameraPipelineResources resources(runtime, {8, 4, 1, false});
    const auto telemetry = resources.telemetry();
    CHECK(telemetry.test_readback_capacity_bytes == 0U);
    CHECK_THROWS_AS(resources.round_trip_for_test(camera_pattern(8, 4, 0.0F)),
                    mcraw::Error);
}

TEST_CASE("Vulkan Camera RGB color pass matches the FP32 precise CPU boundary") {
    constexpr std::uint32_t width = 128;
    constexpr std::uint32_t height = 36;
    auto camera = camera_pattern(width, height, -512.0F);
    for (std::size_t pixel = 0; pixel < camera.planes[0].size(); pixel += 17U) {
        camera.planes[0][pixel] = 70'000.0F;
        camera.planes[2][pixel] = -128.0F;
    }
    mcraw::CameraColorSolution solution;
    solution.camera_to_target = mcraw::Matrix3d{{
        1.213, -0.184, -0.029,
       -0.092,  1.137, -0.045,
        0.018, -0.311,  1.293}};
    constexpr double exposure_stops = 0.375;
    constexpr double input_scale = 1.0 / 65535.0;
    const auto expected = mcraw::camera_to_dwg(
        camera, solution, exposure_stops, input_scale, 1);

    mcraw::VulkanRuntime runtime;
    mcraw::VulkanCameraPipelineResources resources(
        runtime, {width, height, 2, true});
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto actual = resources.camera_to_dwg_for_test(
            camera, solution.camera_to_target, exposure_stops, input_scale);
        const auto error = compare_rgb(expected, actual);
        INFO("max abs=" << error.maximum << ", rmse=" << error.rmse);
        CHECK(error.maximum <= 2.0e-5);
        CHECK(error.rmse <= 1.0e-6);
    }
    const auto telemetry = resources.telemetry();
    CHECK(telemetry.gpu_timestamps_supported);
    CHECK(telemetry.camera_to_dwg_timestamp_samples == 3U);
    CHECK(telemetry.camera_to_dwg_gpu_total_ms > 0.0);
    CHECK(telemetry.camera_to_dwg_gpu_mean_ms > 0.0);
    CHECK(telemetry.camera_to_dwg_gpu_p95_ms > 0.0);
    CHECK(telemetry.camera_to_dwg_gpu_max_ms >=
          telemetry.camera_to_dwg_gpu_min_ms);
}

TEST_CASE("Vulkan Camera RGB color pass rejects non-finite input") {
    auto camera = camera_pattern(8, 4, 0.0F);
    camera.planes[1][3] = std::numeric_limits<float>::quiet_NaN();
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanCameraPipelineResources resources(runtime, {8, 4, 1, true});
    CHECK_THROWS_AS(resources.camera_to_dwg_for_test(
                        camera, mcraw::Matrix3d::identity(), 0.0),
                    mcraw::Error);
}

TEST_CASE("Vulkan Camera RGB color pass matches a real Stage 0 frame") {
    const char* sample_path = std::getenv("MCRAW_STAGE1_REAL_SAMPLE");
    if (sample_path == nullptr || *sample_path == '\0') {
        SKIP("set MCRAW_STAGE1_REAL_SAMPLE to run the 4K Stage 1B golden");
    }
    mcraw::McrawReader reader{std::filesystem::path(sample_path)};
    const auto decoded = reader.load_reference_frame_with_metadata(0);
    const auto calibrated = mcraw::calibrate_raw_for_demosaic(
        decoded.raw, decoded.metadata, 16);
    const auto camera = mcraw::demosaic_unnormalized(
        calibrated, mcraw::DemosaicAlgorithm::rcd, 16);
    const auto solution = mcraw::build_camera_color_solution(decoded.metadata);
    constexpr double input_scale = 1.0 / 65535.0;
    const auto expected = mcraw::camera_to_dwg(
        camera, solution, 0.0, input_scale, 16);
    const bool validation = std::getenv("MCRAW_VULKAN_VALIDATION") != nullptr;
    mcraw::VulkanRuntime runtime({"auto", validation});
    mcraw::VulkanCameraPipelineResources resources(
        runtime, {camera.width, camera.height, 1, true});
    const auto actual = resources.camera_to_dwg_for_test(
        camera, solution.camera_to_target, 0.0, input_scale);
    const auto error = compare_rgb(expected, actual);
    INFO("real frame max abs=" << error.maximum << ", rmse=" << error.rmse);
    CHECK(error.maximum <= 2.0e-5);
    CHECK(error.rmse <= 1.0e-6);
    const auto telemetry = resources.telemetry();
    INFO("real frame GPU ms=" << telemetry.camera_to_dwg_last_gpu_ms);
    CHECK(telemetry.camera_to_dwg_timestamp_samples == 1U);
    CHECK(telemetry.camera_to_dwg_last_gpu_ms > 0.0);
}
