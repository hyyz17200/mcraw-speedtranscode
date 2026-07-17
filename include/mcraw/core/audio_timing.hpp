#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <mcraw/io/mcraw_reader.hpp>

namespace mcraw {

inline constexpr const char* audio_timing_mode =
    "sample_clock_from_first_source_anchor";
inline constexpr const char* audio_timing_warning =
    "noteworthy source audio timestamp irregularity; PCM PTS rebuilt from sample count";

struct AudioTimingDiagnostics {
    std::size_t source_non_monotonic_steps{};
    std::int64_t source_max_backward_step_ns{};
    std::int64_t source_max_abs_residual_ns{};
    std::int64_t source_end_residual_ns{};
    std::int64_t nominal_chunk_interval_ns{};
    std::vector<std::string> warning_reasons;
};

struct AudioTimingResult {
    std::vector<AudioChunk> normalized_chunks;
    std::int64_t first_source_anchor_ns{-1};
    std::int64_t cumulative_samples{};
    AudioTimingDiagnostics diagnostics;

    [[nodiscard]] bool should_warn() const noexcept {
        return !diagnostics.warning_reasons.empty();
    }
};

[[nodiscard]] std::int64_t samples_to_ns_nearest(std::int64_t samples,
                                                 int sample_rate);
[[nodiscard]] std::int64_t ns_to_samples_nearest(std::int64_t nanoseconds,
                                                 int sample_rate);
[[nodiscard]] std::int64_t ns_to_samples_floor(std::int64_t nanoseconds,
                                               int sample_rate);

[[nodiscard]] AudioTimingResult analyze_and_normalize_audio(const AudioInfo& audio);
[[nodiscard]] std::int64_t audio_end_timestamp_ns(
    const std::vector<AudioChunk>& normalized_chunks,
    int sample_rate,
    int channels);
[[nodiscard]] nlohmann::json audio_timing_to_json(const AudioTimingResult& timing);

} // namespace mcraw
