#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/pixel_types.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct VulkanCameraPipelineResourceConfig {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t slots{1};
    bool enable_test_readback{};
};

struct VulkanCameraPipelineResourceTelemetry {
    std::size_t slot_count{};
    std::uint64_t camera_upload_capacity_bytes{};
    std::uint64_t intermediate_capacity_bytes{};
    std::uint64_t test_readback_capacity_bytes{};
    std::uint64_t test_round_trips{};
    std::uint64_t test_upload_bytes{};
    std::uint64_t test_readback_bytes{};
};

// Stage 1A owns only the Camera RGB upload and two device-local FP32 ping-pong
// sets. Pixel processing pipelines are added in later Stage 1 batches. The
// synchronous round-trip method exists solely to validate the resource and
// queue contract; production must construct with test readback disabled.
class VulkanCameraPipelineResources {
public:
    VulkanCameraPipelineResources(VulkanRuntime& runtime,
                                  VulkanCameraPipelineResourceConfig config);
    ~VulkanCameraPipelineResources();
    VulkanCameraPipelineResources(VulkanCameraPipelineResources&&) noexcept;
    VulkanCameraPipelineResources& operator=(VulkanCameraPipelineResources&&) noexcept;
    VulkanCameraPipelineResources(const VulkanCameraPipelineResources&) = delete;
    VulkanCameraPipelineResources& operator=(const VulkanCameraPipelineResources&) = delete;

    [[nodiscard]] CameraRgbF32 round_trip_for_test(const CameraRgbF32& input);
    [[nodiscard]] VulkanCameraPipelineResourceTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
