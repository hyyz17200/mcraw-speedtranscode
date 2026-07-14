#include <mcraw/core/metadata.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

const nlohmann::json* find_value(const nlohmann::json& primary,
                                 const nlohmann::json& fallback,
                                 std::string_view key) {
    const std::string owned_key(key);
    if (const auto it = primary.find(owned_key); it != primary.end()) return &*it;
    if (const auto it = fallback.find(owned_key); it != fallback.end()) return &*it;
    return nullptr;
}

std::array<double, 4> read_levels(const nlohmann::json* value,
                                  std::string_view name,
                                  double missing_default) {
    std::array<double, 4> result{};
    result.fill(missing_default);
    if (value == nullptr) return result;
    if (value->is_number()) {
        result.fill(value->get<double>());
    } else if (value->is_array()) {
        if (value->size() != 1U && value->size() != 2U && value->size() != 4U) {
            throw Error(ErrorCode::invalid_metadata,
                        std::string(name) + " must contain 1, 2, or 4 values");
        }
        if (value->size() == 1U) {
            result.fill(value->at(0).get<double>());
        } else if (value->size() == 2U) {
            result = {value->at(0).get<double>(), value->at(1).get<double>(),
                      value->at(1).get<double>(), value->at(0).get<double>()};
        } else {
            std::transform(value->begin(), value->end(), result.begin(),
                           [](const nlohmann::json& item) { return item.get<double>(); });
        }
    } else {
        throw Error(ErrorCode::invalid_metadata, std::string(name) + " must be numeric or an array");
    }
    for (const double item : result) {
        if (!std::isfinite(item)) {
            throw Error(ErrorCode::invalid_metadata, std::string(name) + " contains a non-finite value");
        }
    }
    return result;
}

std::array<double, 3> read_vector3(const nlohmann::json* value, std::string_view name) {
    if (value == nullptr || !value->is_array() || value->size() != 3U) {
        throw Error(ErrorCode::invalid_metadata, std::string(name) + " must contain exactly 3 values");
    }
    std::array<double, 3> result{};
    std::transform(value->begin(), value->end(), result.begin(),
                   [](const nlohmann::json& item) { return item.get<double>(); });
    for (const double item : result) {
        if (!std::isfinite(item) || item <= 0.0) {
            throw Error(ErrorCode::invalid_metadata, std::string(name) + " must contain finite positive values");
        }
    }
    return result;
}

std::optional<std::array<NoiseModel, 4>> read_noise_profile(const nlohmann::json* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    if (!value->is_array() || (value->size() != 2U && value->size() != 8U)) {
        throw Error(ErrorCode::invalid_metadata,
                    "noiseProfile must contain one or four S/O pairs");
    }
    std::array<NoiseModel, 4> result{};
    for (std::size_t position = 0; position < result.size(); ++position) {
        const std::size_t source = value->size() == 2U ? 0U : position * 2U;
        const double scale = value->at(source).get<double>();
        const double offset = value->at(source + 1U).get<double>();
        if (!std::isfinite(scale) || !std::isfinite(offset) || scale <= 0.0 || offset < 0.0) {
            throw Error(ErrorCode::invalid_metadata,
                        "noiseProfile requires finite S > 0 and O >= 0 values");
        }
        result[position] = {scale, offset};
    }
    return result;
}

std::optional<MatrixMetadata> read_matrix(const nlohmann::json& frame,
                                          const nlohmann::json& container,
                                          std::string_view key) {
    const nlohmann::json* value = nullptr;
    MetadataSource source = MetadataSource::container;
    const std::string owned_key(key);
    if (const auto it = frame.find(owned_key); it != frame.end()) {
        value = &*it;
        source = MetadataSource::frame;
    } else if (const auto container_it = container.find(owned_key); container_it != container.end()) {
        value = &*container_it;
    }
    if (value == nullptr || value->is_null()) return std::nullopt;
    if (!value->is_array() || value->size() != 9U) {
        throw Error(ErrorCode::invalid_metadata, std::string(key) + " must contain exactly 9 values");
    }
    MatrixMetadata result;
    result.source = source;
    std::transform(value->begin(), value->end(), result.values.begin(),
                   [](const nlohmann::json& item) { return item.get<double>(); });
    for (const double item : result.values) {
        if (!std::isfinite(item)) {
            throw Error(ErrorCode::invalid_metadata, std::string(key) + " contains a non-finite value");
        }
    }
    const auto& m = result.values;
    const double determinant = m[0] * (m[4] * m[8] - m[5] * m[7])
                             - m[1] * (m[3] * m[8] - m[5] * m[6])
                             + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(determinant) < 1.0e-12) {
        throw Error(ErrorCode::invalid_metadata, std::string(key) + " is singular or nearly singular");
    }
    const double norm = std::max({
        std::abs(m[0]) + std::abs(m[1]) + std::abs(m[2]),
        std::abs(m[3]) + std::abs(m[4]) + std::abs(m[5]),
        std::abs(m[6]) + std::abs(m[7]) + std::abs(m[8])
    });
    const std::array<double, 9> inverse{
        (m[4] * m[8] - m[5] * m[7]) / determinant,
        (m[2] * m[7] - m[1] * m[8]) / determinant,
        (m[1] * m[5] - m[2] * m[4]) / determinant,
        (m[5] * m[6] - m[3] * m[8]) / determinant,
        (m[0] * m[8] - m[2] * m[6]) / determinant,
        (m[2] * m[3] - m[0] * m[5]) / determinant,
        (m[3] * m[7] - m[4] * m[6]) / determinant,
        (m[1] * m[6] - m[0] * m[7]) / determinant,
        (m[0] * m[4] - m[1] * m[3]) / determinant
    };
    const double inverse_norm = std::max({
        std::abs(inverse[0]) + std::abs(inverse[1]) + std::abs(inverse[2]),
        std::abs(inverse[3]) + std::abs(inverse[4]) + std::abs(inverse[5]),
        std::abs(inverse[6]) + std::abs(inverse[7]) + std::abs(inverse[8])
    });
    if (!std::isfinite(norm * inverse_norm) || norm * inverse_norm > 1.0e8) {
        throw Error(ErrorCode::invalid_metadata, std::string(key) + " has an unsafe condition number");
    }
    return result;
}

double illuminant_to_cct(const nlohmann::json* value) {
    if (value == nullptr || value->is_null()) return 0.0;
    if (value->is_string()) {
        std::string name = value->get<std::string>();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](unsigned char item) {
                                      return std::isspace(item) != 0 || item == '_' || item == '-';
                                  }),
                   name.end());
        if (name == "a" || name == "standarda" || name == "standardlighta") return 2856.0;
        if (name == "d50") return 5003.0;
        if (name == "d55") return 5503.0;
        if (name == "d65") return 6504.0;
        if (name == "d75") return 7504.0;
        throw Error(ErrorCode::invalid_metadata,
                    "unsupported color illuminant name: " + value->get<std::string>());
    }
    if (!value->is_number()) {
        throw Error(ErrorCode::invalid_metadata, "color illuminant must be a DNG number or known name");
    }
    const double number = value->get<double>();
    if (number > 1000.0) return number;
    switch (static_cast<int>(number)) {
    case 17: return 2856.0; // Standard light A
    case 20: return 5503.0; // D55
    case 21: return 6504.0; // D65
    case 22: return 7504.0; // D75
    case 23: return 5003.0; // D50
    default: return 0.0;
    }
}

template <typename T>
T required_number(const nlohmann::json& primary,
                  const nlohmann::json& fallback,
                  std::string_view key) {
    const auto* value = find_value(primary, fallback, key);
    if (value == nullptr || !value->is_number()) {
        throw Error(ErrorCode::invalid_metadata, "missing or invalid metadata field: " + std::string(key));
    }
    return value->get<T>();
}

} // namespace

NormalizedCameraMetadata normalize_metadata(const nlohmann::json& container,
                                            const nlohmann::json& frame) {
    NormalizedCameraMetadata result;
    result.width = required_number<std::uint32_t>(frame, container, "width");
    result.height = required_number<std::uint32_t>(frame, container, "height");

    const auto* arrangement = find_value(frame, container, "sensorArrangement");
    if (arrangement == nullptr) {
        arrangement = find_value(frame, container, "sensorArrangment");
        if (arrangement != nullptr) {
            result.warnings.emplace_back("using historical metadata spelling sensorArrangment");
        }
    }
    if (arrangement == nullptr || !arrangement->is_string()) {
        throw Error(ErrorCode::invalid_metadata, "missing sensorArrangement/sensorArrangment");
    }
    result.cfa = parse_cfa(arrangement->get<std::string>());
    result.black_level = read_levels(find_value(frame, container, "blackLevel"), "blackLevel", 0.0);
    result.white_level = read_levels(find_value(frame, container, "whiteLevel"), "whiteLevel", 0.0);
    result.camera_neutral = read_vector3(find_value(frame, container, "asShotNeutral"), "asShotNeutral");
    result.noise_profile = read_noise_profile(find_value(frame, container, "noiseProfile"));
    result.compression_type = required_number<std::int32_t>(frame, container, "compressionType");

    result.color_matrix1 = read_matrix(frame, container, "colorMatrix1");
    result.color_matrix2 = read_matrix(frame, container, "colorMatrix2");
    result.calibration_matrix1 = read_matrix(frame, container, "calibrationMatrix1");
    result.calibration_matrix2 = read_matrix(frame, container, "calibrationMatrix2");
    result.forward_matrix1 = read_matrix(frame, container, "forwardMatrix1");
    result.forward_matrix2 = read_matrix(frame, container, "forwardMatrix2");

    result.illuminant1_cct = illuminant_to_cct(find_value(frame, container, "colorIlluminant1"));
    result.illuminant2_cct = illuminant_to_cct(find_value(frame, container, "colorIlluminant2"));

    // MotionCam's official decoder example explicitly writes Matrix1 as D65 (tag 21)
    // and Matrix2 as Standard Light A (tag 17). Keep the fallback visible and auditable.
    if (result.color_matrix1 && result.illuminant1_cct == 0.0) {
        result.illuminant1_cct = 6504.0;
        result.warnings.emplace_back("colorIlluminant1 absent; applying official MotionCam D65 compatibility convention");
    }
    if (result.color_matrix2 && result.illuminant2_cct == 0.0) {
        result.illuminant2_cct = 2856.0;
        result.warnings.emplace_back("colorIlluminant2 absent; applying official MotionCam Standard Light A compatibility convention");
    }

    result.validate_for_raw();
    return result;
}

void NormalizedCameraMetadata::validate_for_raw() const {
    if (width == 0 || height == 0) {
        throw Error(ErrorCode::invalid_metadata, "frame dimensions must be non-zero");
    }
    if (compression_type != 6 && compression_type != 7) {
        throw Error(ErrorCode::unsupported_format, "only MotionCam compression type 6/7 is supported");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(black_level[i]) || !std::isfinite(white_level[i]) ||
            white_level[i] <= black_level[i]) {
            throw Error(ErrorCode::invalid_metadata, "white level must be finite and greater than black level");
        }
    }
    if (noise_profile) {
        for (const auto& model : *noise_profile) {
            if (!std::isfinite(model.scale) || !std::isfinite(model.offset) ||
                model.scale <= 0.0 || model.offset < 0.0) {
                throw Error(ErrorCode::invalid_metadata,
                            "noiseProfile requires finite S > 0 and O >= 0 values");
            }
        }
    }
}

void NormalizedCameraMetadata::validate_for_color() const {
    validate_for_raw();
    if (!color_matrix1) {
        throw Error(ErrorCode::invalid_metadata, "ColorMatrix1 is required for color output");
    }
    if (color_matrix2 && (illuminant1_cct <= 0.0 || illuminant2_cct <= 0.0)) {
        throw Error(ErrorCode::invalid_metadata, "dual ColorMatrix metadata requires two interpretable illuminants");
    }
    if (forward_matrix1 && !color_matrix1) {
        throw Error(ErrorCode::invalid_metadata, "ForwardMatrix1 cannot exist without ColorMatrix1");
    }
    if (forward_matrix2 && !color_matrix2) {
        throw Error(ErrorCode::invalid_metadata, "ForwardMatrix2 cannot exist without ColorMatrix2");
    }
}

nlohmann::json metadata_to_json(const NormalizedCameraMetadata& metadata) {
    auto matrix_json = [](const std::optional<MatrixMetadata>& matrix) -> nlohmann::json {
        if (!matrix) return nullptr;
        return {{"values", matrix->values}, {"source", std::string(to_string(matrix->source))}};
    };
    nlohmann::json noise_profile = nullptr;
    if (metadata.noise_profile) {
        noise_profile = nlohmann::json::array();
        for (const auto& model : *metadata.noise_profile) {
            noise_profile.push_back({model.scale, model.offset});
        }
    }
    return {
        {"width", metadata.width}, {"height", metadata.height},
        {"cfa", std::string(to_string(metadata.cfa))},
        {"black_level", metadata.black_level}, {"white_level", metadata.white_level},
        {"camera_neutral", metadata.camera_neutral},
        {"noise_profile", noise_profile},
        {"compression_type", metadata.compression_type},
        {"color_matrix1", matrix_json(metadata.color_matrix1)},
        {"color_matrix2", matrix_json(metadata.color_matrix2)},
        {"calibration_matrix1", matrix_json(metadata.calibration_matrix1)},
        {"calibration_matrix2", matrix_json(metadata.calibration_matrix2)},
        {"forward_matrix1", matrix_json(metadata.forward_matrix1)},
        {"forward_matrix2", matrix_json(metadata.forward_matrix2)},
        {"illuminant1_cct", metadata.illuminant1_cct},
        {"illuminant2_cct", metadata.illuminant2_cct},
        {"warnings", metadata.warnings}
    };
}

std::string_view to_string(MetadataSource source) noexcept {
    switch (source) {
    case MetadataSource::container: return "container";
    case MetadataSource::frame: return "frame";
    case MetadataSource::external_override: return "external_override";
    case MetadataSource::compatibility_default: return "compatibility_default";
    }
    return "unknown";
}

} // namespace mcraw
