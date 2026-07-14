#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/error.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

TEST_CASE("Vulkan runtime selects a non-software compute device") {
    const auto devices = mcraw::VulkanRuntime::enumerate_devices();
    const auto selected = mcraw::VulkanRuntime::select_device(devices, "auto");

    CHECK_FALSE(selected.software);
    REQUIRE_FALSE(selected.name.empty());
    REQUIRE_FALSE(selected.uuid.empty());

    mcraw::VulkanRuntime runtime;
    CHECK(runtime.device().uuid == selected.uuid);
    CHECK(runtime.compute_queue_count() > 0U);
    CHECK(runtime.ffmpeg_device_context() != nullptr);
    CHECK(runtime.ffmpeg_vulkan_context() != nullptr);
}

TEST_CASE("Vulkan runtime rejects a missing explicit device") {
    const auto devices = mcraw::VulkanRuntime::enumerate_devices();
    CHECK_THROWS_AS(
        mcraw::VulkanRuntime::select_device(devices, "uuid:ffffffffffffffffffffffffffffffff"),
        mcraw::Error);
}

TEST_CASE("FFmpeg-owned Vulkan runtime tears down deterministically") {
    for (int iteration = 0; iteration < 3; ++iteration) {
        mcraw::VulkanRuntime runtime;
        CHECK(runtime.compute_queue_count() > 0U);
    }
}
