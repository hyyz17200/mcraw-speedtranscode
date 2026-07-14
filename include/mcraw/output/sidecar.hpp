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
    std::uint64_t video_packets{};
    std::string gpu_name;
    std::string gpu_uuid;
    std::string gpu_driver;
    std::string ffmpeg_version;
    std::string ffmpeg_configuration;
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
