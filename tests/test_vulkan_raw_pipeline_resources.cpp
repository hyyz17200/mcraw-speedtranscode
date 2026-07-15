#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>

#include <catch2/catch_approx.hpp>

#include <mcraw/core/error.hpp>
#include <mcraw/processing/calibration.hpp>
#include <mcraw/vulkan/vulkan_raw_pipeline_resources.hpp>

namespace {

mcraw::RawMosaicU16 raw_pattern(std::uint32_t width, std::uint32_t height,
                                mcraw::CfaPattern cfa, std::uint16_t offset) {
    mcraw::RawMosaicU16 raw{width, height, cfa, {}};
    raw.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < raw.pixels.size(); ++index) {
        raw.pixels[index] = static_cast<std::uint16_t>(offset + index * 17U);
    }
    return raw;
}

} // namespace

TEST_CASE("Vulkan U16 RAW Stage 2A resources round-trip two slots exactly") {
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 8;
    constexpr std::size_t slots = 2;
    constexpr std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const bool validation = std::getenv("MCRAW_VULKAN_VALIDATION") != nullptr;
    mcraw::VulkanRuntime runtime({"auto", validation});
    mcraw::VulkanRawPipelineResources resources(runtime, {width, height, slots, true});

    for (const auto cfa : {mcraw::CfaPattern::rggb, mcraw::CfaPattern::bggr,
                           mcraw::CfaPattern::grbg, mcraw::CfaPattern::gbrg}) {
        const auto input = raw_pattern(width, height, cfa, 23U);
        const auto output = resources.round_trip_for_test(input);
        CHECK(output.width == input.width);
        CHECK(output.height == input.height);
        CHECK(output.cfa == input.cfa);
        CHECK(output.pixels == input.pixels);
    }

    const auto telemetry = resources.telemetry();
    CHECK(telemetry.slot_count == slots);
    CHECK(telemetry.u16_upload_capacity_bytes == pixels * 2U * slots);
    CHECK(telemetry.calibrated_capacity_bytes == pixels * 4U * slots);
    CHECK(telemetry.camera_rgb_capacity_bytes == pixels * 4U * 3U * slots);
    CHECK(telemetry.rcd_scratch_capacity_bytes == pixels * 4U * 5U * slots);
    CHECK(telemetry.test_readback_capacity_bytes == pixels * 2U * slots);
    CHECK(telemetry.test_round_trips == 4U);
    CHECK(telemetry.test_upload_bytes == pixels * 2U * 4U);
    CHECK(telemetry.test_readback_bytes == pixels * 2U * 4U);
}

TEST_CASE("Vulkan U16 RAW production resources reject test readback") {
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRawPipelineResources resources(runtime, {8, 4, 1, false});
    CHECK(resources.telemetry().test_readback_capacity_bytes == 0U);
    CHECK_THROWS_AS(resources.round_trip_for_test(
                        raw_pattern(8, 4, mcraw::CfaPattern::rggb, 0U)),
                    mcraw::Error);
}

TEST_CASE("Vulkan U16 RAW resources reject odd width") {
    mcraw::VulkanRuntime runtime;
    CHECK_THROWS_AS(mcraw::VulkanRawPipelineResources(runtime, {7, 4, 1, false}),
                    mcraw::Error);
}

TEST_CASE("Vulkan U16 RAW calibration matches all CFA positions without clipping") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRawPipelineResources resources(runtime, {width, height, 2, true});
    double maximum = 0.0;
    double squared = 0.0;
    std::size_t samples = 0;
    for (const auto cfa : {mcraw::CfaPattern::rggb, mcraw::CfaPattern::bggr,
                           mcraw::CfaPattern::grbg, mcraw::CfaPattern::gbrg}) {
        auto input = raw_pattern(width, height, cfa, 0U);
        input.pixels[0] = 0U;
        input.pixels[1] = 65'535U;
        mcraw::NormalizedCameraMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.cfa = cfa;
        metadata.black_level = {64.25, 128.5, 256.75, 512.125};
        metadata.white_level = {60'000.5, 61'000.25, 62'000.75, 63'000.5};
        metadata.compression_type = 7;
        const auto expected = mcraw::calibrate_raw_for_demosaic(input, metadata, 1);
        const auto actual = resources.calibrate_for_test(input, metadata);
        REQUIRE(actual.cfa == cfa);
        REQUIRE(actual.pixels.size() == expected.pixels.size());
        for (std::size_t index = 0; index < expected.pixels.size(); ++index) {
            const double error = std::abs(static_cast<double>(actual.pixels[index]) -
                                          expected.pixels[index]);
            maximum = std::max(maximum, error);
            squared += error * error;
            ++samples;
        }
        CHECK(actual.pixels[0] < 0.0F);
        CHECK(actual.pixels[1] > 65'535.0F);
    }
    const double rmse = std::sqrt(squared / static_cast<double>(samples));
    INFO("calibration max abs=" << maximum << ", rmse=" << rmse);
    CHECK(maximum <= 0.02);
    CHECK(rmse <= 0.004);
    const auto telemetry = resources.telemetry();
    CHECK(telemetry.calibrated_test_readback_capacity_bytes ==
          static_cast<std::uint64_t>(width) * height * sizeof(float) * 2U);
    CHECK(telemetry.raw_calibration_timestamp_samples == 4U);
    CHECK(telemetry.raw_calibration_gpu_total_ms > 0.0);
    CHECK(telemetry.raw_calibration_gpu_max_ms >= telemetry.raw_calibration_gpu_min_ms);
}
