#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <mcraw/core/config.hpp>
#include <mcraw/core/metadata.hpp>
#include <mcraw/core/timing.hpp>
#include <mcraw/processing/color.hpp>

namespace mcraw {

struct AvSyncReport {
    bool audio_present{};
    std::size_t audio_chunks{};
    double start_delta_ms{};
    double end_delta_ms{};
};

struct PipelineBackendReport {
    std::string requested_backend{"cpu"};
    std::string backend{"prores_ks"};
    std::size_t async_depth{1};
    bool used_fallback{};
    std::string fallback_reason;
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
    std::string gpu_name;
    std::string gpu_uuid;
    std::string gpu_driver;
    std::string ffmpeg_version;
    std::string ffmpeg_configuration;
    std::string pipeline_entry{"uninitialized"};
    std::string pipeline_precision{"not_applicable"};
    std::string demosaic_location{"not_applicable"};
    std::string color_solution_location{"not_applicable"};
    std::uint64_t target_log_fp32_upload_bytes{};
    std::uint64_t camera_rgb_fp32_upload_bytes{};
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
    std::uint64_t control_status_read_bytes{};
    std::uint64_t control_status_failures{};
};

void write_sidecar(const std::filesystem::path& path,
                   const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   const EffectiveConfig& config,
                   const NormalizedCameraMetadata& first_frame_metadata,
                   const CameraColorSolution& color_solution,
                   const StageTimings& timings,
                   std::size_t frames_written,
                   const AvSyncReport& av_sync,
                   const PipelineBackendReport& pipeline,
                   const std::vector<std::string>& runtime_warnings);

} // namespace mcraw
