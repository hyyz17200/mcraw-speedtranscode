#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/pixel_types.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct VulkanRawPipelineResourceConfig {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t slots{1};
    bool enable_test_readback{};
};

struct VulkanRawPipelineResourceTelemetry {
    std::size_t slot_count{};
    std::uint64_t u16_upload_capacity_bytes{};
    std::uint64_t calibrated_capacity_bytes{};
    std::uint64_t camera_rgb_capacity_bytes{};
    std::uint64_t rcd_scratch_capacity_bytes{};
    std::uint64_t test_readback_capacity_bytes{};
    std::uint64_t test_round_trips{};
    std::uint64_t test_upload_bytes{};
    std::uint64_t test_readback_bytes{};
};

// Stage 2A freezes slot ownership for one U16 upload, calibrated CFA, precise
// RCD scratch and planar Camera RGB output. Pixel shaders arrive in later
// rollback points. The synchronous round trip is test-only.
class VulkanRawPipelineResources {
public:
    VulkanRawPipelineResources(VulkanRuntime& runtime,
                               VulkanRawPipelineResourceConfig config);
    ~VulkanRawPipelineResources();
    VulkanRawPipelineResources(VulkanRawPipelineResources&&) noexcept;
    VulkanRawPipelineResources& operator=(VulkanRawPipelineResources&&) noexcept;
    VulkanRawPipelineResources(const VulkanRawPipelineResources&) = delete;
    VulkanRawPipelineResources& operator=(const VulkanRawPipelineResources&) = delete;

    [[nodiscard]] RawMosaicU16 round_trip_for_test(const RawMosaicU16& input);
    [[nodiscard]] VulkanRawPipelineResourceTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
