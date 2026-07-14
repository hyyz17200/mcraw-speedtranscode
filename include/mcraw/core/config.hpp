#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace mcraw {

enum class DemosaicAlgorithm { rcd, amaze, igv, dcb, lmmse };
enum class NegativePolicy { preserve_by_curve, clamp_zero, error };
enum class ChromaFilter { fast, quality };

struct EffectiveConfig {
    std::uint32_t schema_version{1};
    DemosaicAlgorithm demosaic{DemosaicAlgorithm::rcd};
    double exposure_offset_stops{0.0};
    NegativePolicy negative_policy{NegativePolicy::preserve_by_curve};
    ChromaFilter chroma_filter{ChromaFilter::quality};
    double capture_sharpening{0.4};
    double capture_sharpening_threshold{0.002};
    bool deterministic_dither{true};
    bool preserve_source_timestamps{true};
    bool preserve_audio{true};
    std::size_t max_frames{0};
    // Zero selects an automatic value. cpu_threads is the total process budget;
    // max_parallel_frames caps the number of frames allowed in flight.
    std::size_t cpu_threads{0};
    std::size_t max_parallel_frames{0};
    std::string target_profile{"DaVinciIntermediate_DWG"};
    std::string prores_profile{"hq"};

    void validate() const;
};

[[nodiscard]] EffectiveConfig load_config(const std::filesystem::path& path);
[[nodiscard]] nlohmann::json config_to_json(const EffectiveConfig& config);
[[nodiscard]] std::string_view to_string(DemosaicAlgorithm value) noexcept;
[[nodiscard]] std::string_view to_string(NegativePolicy value) noexcept;
[[nodiscard]] std::string_view to_string(ChromaFilter value) noexcept;

} // namespace mcraw
