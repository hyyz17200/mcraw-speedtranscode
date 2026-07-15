#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/error.hpp>
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

} // namespace

TEST_CASE("Vulkan Camera RGB Stage 1A resources round-trip two slots exactly") {
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 8;
    constexpr std::size_t slots = 2;
    constexpr std::uint64_t plane_bytes =
        static_cast<std::uint64_t>(width) * height * sizeof(float);
    mcraw::VulkanRuntime runtime;
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
