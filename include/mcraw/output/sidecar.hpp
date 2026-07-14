#pragma once

#include <cstddef>
#include <filesystem>
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

void write_sidecar(const std::filesystem::path& path,
                   const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   const EffectiveConfig& config,
                   const NormalizedCameraMetadata& first_frame_metadata,
                   const CameraColorSolution& color_solution,
                   const StageTimings& timings,
                   std::size_t frames_written,
                   const AvSyncReport& av_sync,
                   const std::vector<std::string>& runtime_warnings);

} // namespace mcraw
