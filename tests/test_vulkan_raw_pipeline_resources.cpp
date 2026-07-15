#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>

#include <mcraw/core/error.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/demosaic.hpp>
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

TEST_CASE("Vulkan precise RCD matches librtprocess for all Bayer patterns") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRawPipelineResources resources(runtime, {width, height, 2, true});
    double maximum = 0.0;
    double border_maximum = 0.0;
    double squared = 0.0;
    std::size_t samples = 0;
    for (const auto cfa : {mcraw::CfaPattern::rggb, mcraw::CfaPattern::bggr,
                           mcraw::CfaPattern::grbg, mcraw::CfaPattern::gbrg}) {
        mcraw::RawMosaicU16 input{width, height, cfa, {}};
        input.pixels.resize(static_cast<std::size_t>(width) * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y) * width + x;
                const auto edge = ((x / 8U + y / 8U) & 1U) != 0U ? 9000U : 0U;
                input.pixels[index] = static_cast<std::uint16_t>(
                    1200U + x * 173U + y * 97U + (x * y * 13U) % 4000U + edge);
            }
        }
        mcraw::NormalizedCameraMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.cfa = cfa;
        metadata.black_level = {64.0, 96.0, 128.0, 160.0};
        metadata.white_level = {60'000.0, 60'500.0, 61'000.0, 61'500.0};
        metadata.compression_type = 7;
        const auto calibrated = mcraw::calibrate_raw_for_demosaic(input, metadata, 1);
        const auto expected = mcraw::demosaic_unnormalized(
            calibrated, mcraw::DemosaicAlgorithm::rcd, 1);
        const auto actual = resources.demosaic_rcd_for_test(input, metadata);
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto index = static_cast<std::size_t>(y) * width + x;
                    const double error = std::abs(
                        static_cast<double>(actual.planes[channel][index]) -
                        expected.planes[channel][index]);
                    maximum = std::max(maximum, error);
                    if (x < 9U || x + 9U >= width || y < 9U || y + 9U >= height) {
                        border_maximum = std::max(border_maximum, error);
                    }
                    squared += error * error;
                    ++samples;
                }
            }
        }
    }
    const double rmse = std::sqrt(squared / static_cast<double>(samples));
    INFO("RCD max abs=" << maximum << ", border max=" << border_maximum
                         << ", rmse=" << rmse);
    CHECK(maximum <= 0.02);
    CHECK(border_maximum <= 0.02);
    CHECK(rmse <= 0.005);
    const auto telemetry = resources.telemetry();
    CHECK(telemetry.camera_rgb_test_readback_capacity_bytes ==
          static_cast<std::uint64_t>(width) * height * sizeof(float) * 3U * 2U);
    CHECK(telemetry.rcd_demosaic_timestamp_samples == 4U);
    CHECK(telemetry.rcd_demosaic_gpu_total_ms > 0.0);
}

TEST_CASE("Vulkan precise RCD matches a real Stage 0 frame") {
    const char* sample_path = std::getenv("MCRAW_STAGE2_REAL_SAMPLE");
    if (sample_path == nullptr || *sample_path == '\0') {
        SKIP("set MCRAW_STAGE2_REAL_SAMPLE to run the 4K Stage 2C golden");
    }
    mcraw::McrawReader reader{std::filesystem::path(sample_path)};
    const auto decoded = reader.load_reference_frame_with_metadata(0);
    const auto calibrated = mcraw::calibrate_raw_for_demosaic(
        decoded.raw, decoded.metadata, 16);
    const auto expected = mcraw::demosaic_unnormalized(
        calibrated, mcraw::DemosaicAlgorithm::rcd, 16);
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRawPipelineResources resources(
        runtime, {decoded.raw.width, decoded.raw.height, 1, true});
    const auto actual = resources.demosaic_rcd_for_test(decoded.raw, decoded.metadata);
    double maximum = 0.0;
    double squared = 0.0;
    std::size_t samples = 0;
    std::size_t worst_channel = 0;
    std::size_t worst_index = 0;
    std::size_t above_two = 0;
    std::vector<double> errors;
    errors.reserve(expected.planes[0].size() * 3U);
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        for (std::size_t index = 0; index < expected.planes[channel].size(); ++index) {
            const double error = std::abs(
                static_cast<double>(actual.planes[channel][index]) -
                expected.planes[channel][index]);
            if (error > maximum) {
                maximum = error;
                worst_channel = channel;
                worst_index = index;
            }
            if (error > 2.0) ++above_two;
            errors.push_back(error);
            squared += error * error;
            ++samples;
        }
    }
    const double rmse = std::sqrt(squared / static_cast<double>(samples));
    std::sort(errors.begin(), errors.end());
    const double p99 = errors[static_cast<std::size_t>(
        0.99 * static_cast<double>(errors.size() - 1U))];
    const auto telemetry = resources.telemetry();
    INFO("real RCD max abs=" << maximum << ", rmse=" << rmse
                              << ", p99=" << p99 << ", above2=" << above_two
                              << ", worst channel=" << worst_channel
                              << ", x=" << worst_index % expected.width
                              << ", y=" << worst_index / expected.width
                              << ", expected="
                              << expected.planes[worst_channel][worst_index]
                              << ", actual="
                              << actual.planes[worst_channel][worst_index]
                              << ", GPU ms=" << telemetry.rcd_demosaic_gpu_total_ms);
    CHECK(maximum <= 160.0);
    CHECK(rmse <= 0.05);
    CHECK(p99 <= 0.01);
    CHECK(above_two <= 512U);
    CHECK(telemetry.raw_calibration_timestamp_samples == 1U);
    CHECK(telemetry.rcd_demosaic_timestamp_samples == 1U);
    CHECK(telemetry.rcd_demosaic_gpu_total_ms > 0.0);
}
