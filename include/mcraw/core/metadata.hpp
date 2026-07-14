#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

enum class MetadataSource { container, frame, external_override, compatibility_default };

struct MatrixMetadata {
    std::array<double, 9> values{};
    MetadataSource source{MetadataSource::container};
};

struct NoiseModel {
    double scale{};
    double offset{};
};

struct NormalizedCameraMetadata {
    std::uint32_t width{};
    std::uint32_t height{};
    CfaPattern cfa{CfaPattern::rggb};
    std::array<double, 4> black_level{};
    std::array<double, 4> white_level{};
    std::array<double, 3> camera_neutral{1.0, 1.0, 1.0};
    std::optional<std::array<NoiseModel, 4>> noise_profile;
    std::optional<MatrixMetadata> color_matrix1;
    std::optional<MatrixMetadata> color_matrix2;
    std::optional<MatrixMetadata> calibration_matrix1;
    std::optional<MatrixMetadata> calibration_matrix2;
    std::optional<MatrixMetadata> forward_matrix1;
    std::optional<MatrixMetadata> forward_matrix2;
    double illuminant1_cct{};
    double illuminant2_cct{};
    std::int32_t compression_type{};
    std::vector<std::string> warnings;

    void validate_for_raw() const;
    void validate_for_color() const;
};

[[nodiscard]] NormalizedCameraMetadata normalize_metadata(
    const nlohmann::json& container,
    const nlohmann::json& frame);

[[nodiscard]] nlohmann::json metadata_to_json(const NormalizedCameraMetadata& metadata);
[[nodiscard]] std::string_view to_string(MetadataSource source) noexcept;

} // namespace mcraw
