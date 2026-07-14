#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/io/mcraw_reader.hpp>

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
};

struct FfmpegWriterTelemetry {
    std::string backend{"prores_ks"};
    bool gpu_resident{};
    std::uint64_t upload_frames{};
    std::uint64_t readback_frames{};
    std::uint64_t direct_frames{};
    std::uint64_t rgb_upload_bytes{};
    std::uint64_t video_packets{};
    std::size_t gpu_queue_capacity{};
    std::size_t gpu_queue_max_depth{};
    std::size_t packet_queue_capacity{};
    std::size_t packet_queue_max_depth{};
    std::uint64_t backpressure_waits{};
    double backpressure_wait_ms{};
    std::uint64_t mux_bytes{};
    double mux_megabytes_per_second{};
    std::string gpu_name;
    std::string gpu_uuid;
    std::string gpu_driver;
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
    void write_audio(const AudioChunk& chunk);
    void finish();
    [[nodiscard]] FfmpegWriterTelemetry telemetry() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Reopens a completed temporary MOV and verifies the minimum contract before
// the caller is allowed to rename it to the requested final path.
void validate_prores_mov(const std::filesystem::path& path,
                         std::uint64_t expected_video_packets);

} // namespace mcraw
