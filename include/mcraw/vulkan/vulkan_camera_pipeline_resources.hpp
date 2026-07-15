#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/processing/color.hpp>
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
    bool gpu_timestamps_supported{};
    std::uint64_t camera_to_dwg_timestamp_samples{};
    double camera_to_dwg_gpu_total_ms{};
    double camera_to_dwg_gpu_mean_ms{};
    double camera_to_dwg_gpu_p50_ms{};
    double camera_to_dwg_gpu_p95_ms{};
    double camera_to_dwg_gpu_p99_ms{};
    double camera_to_dwg_gpu_min_ms{};
    double camera_to_dwg_gpu_max_ms{};
    double camera_to_dwg_last_gpu_ms{};
    std::uint64_t capture_sharpening_timestamp_samples{};
    double capture_sharpening_gpu_total_ms{};
    double capture_sharpening_gpu_mean_ms{};
    double capture_sharpening_gpu_p50_ms{};
    double capture_sharpening_gpu_p95_ms{};
    double capture_sharpening_gpu_p99_ms{};
    double capture_sharpening_gpu_min_ms{};
    double capture_sharpening_gpu_max_ms{};
    double capture_sharpening_last_gpu_ms{};
    std::uint64_t davinci_intermediate_timestamp_samples{};
    double davinci_intermediate_gpu_total_ms{};
    double davinci_intermediate_gpu_mean_ms{};
    double davinci_intermediate_gpu_p50_ms{};
    double davinci_intermediate_gpu_p95_ms{};
    double davinci_intermediate_gpu_p99_ms{};
    double davinci_intermediate_gpu_min_ms{};
    double davinci_intermediate_gpu_max_ms{};
    double davinci_intermediate_last_gpu_ms{};
    std::uint64_t control_status_capacity_bytes{};
    std::uint64_t davinci_lut_capacity_bytes{};
    std::uint64_t control_status_read_bytes{};
    std::uint64_t control_status_failures{};
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
    [[nodiscard]] TargetLinearRgbF32 camera_to_dwg_for_test(
        const CameraRgbF32& input,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double input_scale = 1.0 / 65535.0);
    [[nodiscard]] TargetLinearRgbF32 camera_to_dwg_sharpen_for_test(
        const CameraRgbF32& input,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double sharpening_amount,
        double sharpening_threshold,
        double input_scale = 1.0 / 65535.0);
    [[nodiscard]] TargetLogRgbF32 camera_to_dwg_sharpen_di_for_test(
        const CameraRgbF32& input,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double sharpening_amount,
        double sharpening_threshold,
        NegativePolicy negative_policy,
        double input_scale = 1.0 / 65535.0);
    [[nodiscard]] VulkanCameraPipelineResourceTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
