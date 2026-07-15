#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/config.hpp>
#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct VulkanRgbToYuvConfig {
    std::uint32_t width{};
    std::uint32_t height{};
    ChromaFilter chroma_filter{ChromaFilter::quality};
    bool deterministic_dither{true};
    GpuPrecision precision{GpuPrecision::fp32};
    std::size_t slots{1};
    GpuPerformanceMode performance_mode{GpuPerformanceMode::precise};
};

struct VulkanRgbToYuvTelemetry {
    std::uint64_t dispatches{};
    std::uint64_t upload_bytes{};
    std::uint64_t download_bytes{};
    double last_dispatch_wall_ms{};
    bool gpu_timestamps_supported{};
    std::uint64_t gpu_timestamp_samples{};
    double gpu_total_ms{};
    double gpu_mean_ms{};
    double gpu_p50_ms{};
    double gpu_p95_ms{};
    double gpu_p99_ms{};
    double gpu_min_ms{};
    double gpu_max_ms{};
    double last_gpu_dispatch_ms{};
};

class VulkanRgbToYuv422 {
public:
    // The runtime must outlive this object. Phase 4 deliberately waits for
    // each dispatch and downloads raw YUV for golden validation; it is not the
    // asynchronous GPU-resident handoff introduced in the following phase.
    VulkanRgbToYuv422(VulkanRuntime& runtime, VulkanRgbToYuvConfig config);
    ~VulkanRgbToYuv422();
    VulkanRgbToYuv422(VulkanRgbToYuv422&&) noexcept;
    VulkanRgbToYuv422& operator=(VulkanRgbToYuv422&&) noexcept;
    VulkanRgbToYuv422(const VulkanRgbToYuv422&) = delete;
    VulkanRgbToYuv422& operator=(const VulkanRgbToYuv422&) = delete;

    [[nodiscard]] Yuv422P10 pack(const TargetLogRgbF32& input,
                                 std::size_t frame_index);
    [[nodiscard]] VulkanRgbToYuvTelemetry telemetry() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct VulkanRgbToYuvFrameTelemetry {
    std::uint64_t dispatches{};
    std::uint64_t rgb_upload_bytes{};
    std::uint64_t u16_raw_upload_bytes{};
    std::uint64_t output_frames{};
    std::uint64_t yuv_upload_frames{};
    std::uint64_t yuv_readback_frames{};
    std::size_t slot_count{};
    std::size_t in_flight{};
    std::size_t max_in_flight{};
    std::uint64_t backpressure_waits{};
    double backpressure_wait_ms{};
    double last_dispatch_wall_ms{};
    bool gpu_timestamps_supported{};
    std::uint64_t gpu_timestamp_samples{};
    double gpu_total_ms{};
    double gpu_mean_ms{};
    double gpu_p50_ms{};
    double gpu_p95_ms{};
    double gpu_p99_ms{};
    double gpu_min_ms{};
    double gpu_max_ms{};
    double last_gpu_dispatch_ms{};
    std::uint64_t camera_to_dwg_timestamp_samples{};
    double camera_to_dwg_gpu_total_ms{};
    double camera_to_dwg_gpu_mean_ms{};
    double camera_to_dwg_gpu_p50_ms{};
    double camera_to_dwg_gpu_p95_ms{};
    double camera_to_dwg_gpu_p99_ms{};
    double camera_to_dwg_gpu_min_ms{};
    double camera_to_dwg_gpu_max_ms{};
    std::uint64_t capture_sharpening_timestamp_samples{};
    double capture_sharpening_gpu_total_ms{};
    double capture_sharpening_gpu_mean_ms{};
    double capture_sharpening_gpu_p50_ms{};
    double capture_sharpening_gpu_p95_ms{};
    double capture_sharpening_gpu_p99_ms{};
    double capture_sharpening_gpu_min_ms{};
    double capture_sharpening_gpu_max_ms{};
    std::uint64_t davinci_intermediate_timestamp_samples{};
    double davinci_intermediate_gpu_total_ms{};
    double davinci_intermediate_gpu_mean_ms{};
    double davinci_intermediate_gpu_p50_ms{};
    double davinci_intermediate_gpu_p95_ms{};
    double davinci_intermediate_gpu_p99_ms{};
    double davinci_intermediate_gpu_min_ms{};
    double davinci_intermediate_gpu_max_ms{};
    std::uint64_t raw_calibration_timestamp_samples{};
    double raw_calibration_gpu_total_ms{};
    double raw_calibration_gpu_mean_ms{};
    double raw_calibration_gpu_p50_ms{};
    double raw_calibration_gpu_p95_ms{};
    double raw_calibration_gpu_p99_ms{};
    double raw_calibration_gpu_min_ms{};
    double raw_calibration_gpu_max_ms{};
    std::uint64_t rcd_demosaic_timestamp_samples{};
    double rcd_demosaic_gpu_total_ms{};
    double rcd_demosaic_gpu_mean_ms{};
    double rcd_demosaic_gpu_p50_ms{};
    double rcd_demosaic_gpu_p95_ms{};
    double rcd_demosaic_gpu_p99_ms{};
    double rcd_demosaic_gpu_min_ms{};
    double rcd_demosaic_gpu_max_ms{};
    std::uint64_t control_status_read_bytes{};
    std::uint64_t control_status_failures{};
    std::uint64_t frame_allocation_samples{};
    double frame_allocation_total_ms{};
    double frame_allocation_mean_ms{};
    double frame_allocation_max_ms{};
    std::uint64_t queue_lock_wait_samples{};
    double queue_lock_wait_total_ms{};
    double queue_lock_wait_mean_ms{};
    double queue_lock_wait_max_ms{};
    std::uint64_t queue_submit_samples{};
    double queue_submit_total_ms{};
    double queue_submit_mean_ms{};
    double queue_submit_max_ms{};
};

class VulkanRgbToYuvFrameWriter {
public:
    VulkanRgbToYuvFrameWriter(VulkanRuntime& runtime,
                              FfmpegVulkanFrameContext& frames,
                              VulkanRgbToYuvConfig config);
    ~VulkanRgbToYuvFrameWriter();
    VulkanRgbToYuvFrameWriter(VulkanRgbToYuvFrameWriter&&) noexcept;
    VulkanRgbToYuvFrameWriter& operator=(VulkanRgbToYuvFrameWriter&&) noexcept;
    VulkanRgbToYuvFrameWriter(const VulkanRgbToYuvFrameWriter&) = delete;
    VulkanRgbToYuvFrameWriter& operator=(const VulkanRgbToYuvFrameWriter&) = delete;

    // Returns the exact FFmpeg pool frame written by the compute shader. No
    // uncompressed YUV upload or readback occurs on this production path.
    [[nodiscard]] VulkanVideoFrame pack(const TargetLogRgbF32& input,
                                        std::size_t frame_index,
                                        FrameMetadata metadata);
    [[nodiscard]] VulkanVideoFrame pack_camera_rgb(
        const CameraRgbF32& input,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double input_scale,
        double sharpening_amount,
        double sharpening_threshold,
        NegativePolicy negative_policy,
        std::size_t frame_index,
        FrameMetadata metadata);
    [[nodiscard]] VulkanVideoFrame pack_raw_mosaic(
        const RawMosaicU16& input,
        const NormalizedCameraMetadata& metadata,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double sharpening_amount,
        double sharpening_threshold,
        NegativePolicy negative_policy,
        std::size_t frame_index,
        FrameMetadata frame_metadata);
    void wait();
    [[nodiscard]] VulkanRgbToYuvFrameTelemetry telemetry() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
