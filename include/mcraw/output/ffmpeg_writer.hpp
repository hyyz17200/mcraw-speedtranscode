#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/processing/color.hpp>

namespace mcraw {

// ProRes is intra-only, so independent frames can encode concurrently on
// separate identically configured encoder contexts and mux in source order.
// contexts scales the number of frames in flight; threads_per_context is the
// FFmpeg slice-thread count inside each encode (0 lets FFmpeg decide).
struct VideoEncodeConcurrency {
    std::size_t contexts{1};
    int threads_per_context{0};
};

struct FfmpegVideoBackendConfig {
    VideoBackend backend{VideoBackend::cpu};
    std::string gpu_selector{"auto"};
    std::size_t async_depth{1};
    bool enable_validation{};
    ChromaFilter chroma_filter{ChromaFilter::quality};
    bool deterministic_dither{true};
    GpuPrecision precision{GpuPrecision::fp32};
    GpuPerformanceMode performance_mode{GpuPerformanceMode::precise};
};

struct FfmpegWriterTelemetry {
    std::string backend{"prores_ks"};
    bool gpu_resident{};
    std::uint64_t upload_frames{};
    std::uint64_t readback_frames{};
    std::uint64_t direct_frames{};
    std::uint64_t rgb_upload_bytes{};
    std::uint64_t compressed_input_upload_bytes{};
    std::uint64_t u16_raw_upload_bytes{};
    std::uint64_t fp16_rgb_upload_bytes{};
    std::uint64_t fp32_rgb_upload_bytes{};
    std::uint64_t compressed_packet_download_bytes{};
    std::uint64_t video_packets{};
    std::size_t gpu_queue_capacity{};
    std::size_t gpu_queue_max_depth{};
    std::size_t packet_queue_capacity{};
    std::size_t packet_queue_max_depth{};
    std::uint64_t backpressure_waits{};
    double backpressure_wait_ms{};
    std::uint64_t mux_bytes{};
    double mux_megabytes_per_second{};
    bool gpu_timestamps_supported{};
    std::uint64_t rgb_to_yuv_gpu_timestamp_samples{};
    double rgb_to_yuv_gpu_total_ms{};
    double rgb_to_yuv_gpu_mean_ms{};
    double rgb_to_yuv_gpu_p50_ms{};
    double rgb_to_yuv_gpu_p95_ms{};
    double rgb_to_yuv_gpu_p99_ms{};
    double rgb_to_yuv_gpu_min_ms{};
    double rgb_to_yuv_gpu_max_ms{};
    std::uint64_t camera_to_dwg_gpu_timestamp_samples{};
    double camera_to_dwg_gpu_total_ms{};
    double camera_to_dwg_gpu_mean_ms{};
    double camera_to_dwg_gpu_p50_ms{};
    double camera_to_dwg_gpu_p95_ms{};
    double camera_to_dwg_gpu_p99_ms{};
    double camera_to_dwg_gpu_min_ms{};
    double camera_to_dwg_gpu_max_ms{};
    std::uint64_t capture_sharpening_gpu_timestamp_samples{};
    double capture_sharpening_gpu_total_ms{};
    double capture_sharpening_gpu_mean_ms{};
    double capture_sharpening_gpu_p50_ms{};
    double capture_sharpening_gpu_p95_ms{};
    double capture_sharpening_gpu_p99_ms{};
    double capture_sharpening_gpu_min_ms{};
    double capture_sharpening_gpu_max_ms{};
    std::uint64_t davinci_intermediate_gpu_timestamp_samples{};
    double davinci_intermediate_gpu_total_ms{};
    double davinci_intermediate_gpu_mean_ms{};
    double davinci_intermediate_gpu_p50_ms{};
    double davinci_intermediate_gpu_p95_ms{};
    double davinci_intermediate_gpu_p99_ms{};
    double davinci_intermediate_gpu_min_ms{};
    double davinci_intermediate_gpu_max_ms{};
    std::uint64_t raw_calibration_gpu_timestamp_samples{};
    double raw_calibration_gpu_total_ms{};
    double raw_calibration_gpu_mean_ms{};
    double raw_calibration_gpu_p50_ms{};
    double raw_calibration_gpu_p95_ms{};
    double raw_calibration_gpu_p99_ms{};
    double raw_calibration_gpu_min_ms{};
    double raw_calibration_gpu_max_ms{};
    std::uint64_t rcd_demosaic_gpu_timestamp_samples{};
    double rcd_demosaic_gpu_total_ms{};
    double rcd_demosaic_gpu_mean_ms{};
    double rcd_demosaic_gpu_p50_ms{};
    double rcd_demosaic_gpu_p95_ms{};
    double rcd_demosaic_gpu_p99_ms{};
    double rcd_demosaic_gpu_min_ms{};
    double rcd_demosaic_gpu_max_ms{};
    std::uint64_t control_status_read_bytes{};
    std::uint64_t control_status_failures{};
    std::string gpu_name;
    std::string gpu_uuid;
    std::string gpu_driver;
    std::string pipeline_entry{"uninitialized"};
    std::string pipeline_precision{"not_applicable"};
    std::string demosaic_location{"not_applicable"};
    std::string color_solution_location{"not_applicable"};
    std::string performance_mode{"not_applicable"};
    std::string intermediate_storage{"not_applicable"};
    std::string di_implementation{"not_applicable"};
    std::string dither_mode{"not_applicable"};
    std::string demosaic_implementation{"not_applicable"};
    std::uint64_t target_log_fp32_upload_bytes{};
    std::uint64_t camera_rgb_fp32_upload_bytes{};
    std::uint64_t job_queue_backpressure_waits{};
    double job_queue_backpressure_wait_ms{};
    std::uint64_t packet_queue_backpressure_waits{};
    double packet_queue_backpressure_wait_ms{};
    std::uint64_t slot_backpressure_waits{};
    double slot_backpressure_wait_ms{};
    std::uint64_t job_queue_latency_samples{};
    double job_queue_latency_total_ms{};
    double job_queue_latency_mean_ms{};
    double job_queue_latency_max_ms{};
    std::uint64_t frame_pack_samples{};
    double frame_pack_total_ms{};
    double frame_pack_mean_ms{};
    double frame_pack_max_ms{};
    std::uint64_t encoder_send_samples{};
    double encoder_send_total_ms{};
    double encoder_send_mean_ms{};
    double encoder_send_max_ms{};
    std::uint64_t encoder_receive_samples{};
    double encoder_receive_total_ms{};
    double encoder_receive_mean_ms{};
    double encoder_receive_max_ms{};
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
    std::size_t resident_slot_count{};
    std::size_t prepared_frame_queue_capacity{};
    std::size_t prepared_frame_queue_max_depth{};
};

// A semantic wrapper is required because CameraRgbF32 and TargetLogRgbF32
// currently share the same storage alias. Stage 1A establishes the job/API
// boundary; Stage 1B supplies the first pixel-processing pass.
struct VulkanCameraRgbInput {
    CameraRgbF32 image;
    Matrix3d camera_to_target;
    double exposure_offset_stops{};
    double input_scale{1.0 / 65535.0};
    double capture_sharpening{};
    double capture_sharpening_threshold{};
    NegativePolicy negative_policy{NegativePolicy::preserve_by_curve};
};

// Stage 2 semantic job boundary. The writer overload is added only when the
// resident RAW chain is connected; defining the payload now prevents U16 RAW
// from being confused with a generic byte upload.
struct VulkanRawMosaicInput {
    RawMosaicU16 image;
    NormalizedCameraMetadata metadata;
    Matrix3d camera_to_target;
    double exposure_offset_stops{};
    double capture_sharpening{};
    double capture_sharpening_threshold{};
    NegativePolicy negative_policy{NegativePolicy::preserve_by_curve};
};

class FfmpegWriter {
public:
    FfmpegWriter(const std::filesystem::path& output,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::int64_t timeline_origin_ns,
                 int audio_sample_rate,
                 int audio_channels,
                 VideoEncodeConcurrency video_concurrency = {},
                 FfmpegVideoBackendConfig backend = {});
    ~FfmpegWriter();
    FfmpegWriter(FfmpegWriter&&) noexcept;
    FfmpegWriter& operator=(FfmpegWriter&&) noexcept;
    FfmpegWriter(const FfmpegWriter&) = delete;
    FfmpegWriter& operator=(const FfmpegWriter&) = delete;

    void write_video(Yuv422P10 frame, std::int64_t timestamp_ns);
    void write_video(TargetLogRgbF32 frame,
                     std::int64_t timestamp_ns,
                     std::size_t frame_index);
    void write_video(VulkanCameraRgbInput frame,
                     std::int64_t timestamp_ns,
                     std::size_t frame_index);
    void write_video(VulkanRawMosaicInput frame,
                     std::int64_t timestamp_ns,
                     std::size_t frame_index);
    void write_audio(const AudioChunk& chunk);
    void finish();
    [[nodiscard]] FfmpegWriterTelemetry telemetry() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Reopens a completed temporary MOV and verifies the minimum contract before
// the caller is allowed to rename it to the requested final path.
void validate_prores_mov_metadata(const std::filesystem::path& path);
void validate_prores_mov(const std::filesystem::path& path,
                         std::uint64_t expected_video_packets);

} // namespace mcraw
