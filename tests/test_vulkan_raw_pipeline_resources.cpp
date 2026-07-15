#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include <mcraw/core/error.hpp>
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
