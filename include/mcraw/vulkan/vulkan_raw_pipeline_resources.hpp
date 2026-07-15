#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/pixel_types.hpp>
#include <mcraw/core/metadata.hpp>
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
    std::uint64_t calibrated_test_readback_capacity_bytes{};
    std::uint64_t test_round_trips{};
    std::uint64_t test_upload_bytes{};
    std::uint64_t test_readback_bytes{};
    bool gpu_timestamps_supported{};
    std::uint64_t raw_calibration_timestamp_samples{};
    double raw_calibration_gpu_total_ms{};
    double raw_calibration_gpu_mean_ms{};
    double raw_calibration_gpu_p50_ms{};
    double raw_calibration_gpu_p95_ms{};
    double raw_calibration_gpu_p99_ms{};
    double raw_calibration_gpu_min_ms{};
    double raw_calibration_gpu_max_ms{};
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
    [[nodiscard]] RawDemosaicF32 calibrate_for_test(
        const RawMosaicU16& input,
        const NormalizedCameraMetadata& metadata);
    [[nodiscard]] VulkanRawPipelineResourceTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
