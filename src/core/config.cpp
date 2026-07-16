#include <mcraw/core/config.hpp>

#include <fstream>
#include <cmath>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

template <typename Enum>
Enum parse_enum(std::string_view value);

template <>
DemosaicAlgorithm parse_enum(std::string_view value) {
    if (value == "rcd") return DemosaicAlgorithm::rcd;
    if (value == "amaze") return DemosaicAlgorithm::amaze;
    if (value == "igv") return DemosaicAlgorithm::igv;
    if (value == "dcb") return DemosaicAlgorithm::dcb;
    if (value == "lmmse") return DemosaicAlgorithm::lmmse;
    throw Error(ErrorCode::invalid_argument, "unknown demosaic algorithm: " + std::string(value));
}

template <>
NegativePolicy parse_enum(std::string_view value) {
    if (value == "preserve_by_curve") return NegativePolicy::preserve_by_curve;
    if (value == "clamp_zero") return NegativePolicy::clamp_zero;
    if (value == "error") return NegativePolicy::error;
    throw Error(ErrorCode::invalid_argument, "unknown negative policy: " + std::string(value));
}

template <>
ChromaFilter parse_enum(std::string_view value) {
    if (value == "fast") return ChromaFilter::fast;
    if (value == "quality") return ChromaFilter::quality;
    throw Error(ErrorCode::invalid_argument, "unknown chroma filter: " + std::string(value));
}

template <>
VideoBackend parse_enum(std::string_view value) {
    if (value == "auto") return VideoBackend::automatic;
    if (value == "cpu") return VideoBackend::cpu;
    if (value == "vulkan") return VideoBackend::vulkan;
    throw Error(ErrorCode::invalid_argument, "unknown video backend: " + std::string(value));
}

template <>
GpuFallback parse_enum(std::string_view value) {
    if (value == "prores_ks") return GpuFallback::prores_ks;
    if (value == "none") return GpuFallback::none;
    throw Error(ErrorCode::invalid_argument, "unknown GPU fallback: " + std::string(value));
}

template <>
GpuPrecision parse_enum(std::string_view value) {
    if (value == "fp32") return GpuPrecision::fp32;
    if (value == "fp16") return GpuPrecision::fp16;
    throw Error(ErrorCode::invalid_argument, "unknown GPU precision: " + std::string(value));
}

template <>
GpuPerformanceMode parse_enum(std::string_view value) {
    if (value == "precise") return GpuPerformanceMode::precise;
    if (value == "fast") return GpuPerformanceMode::fast;
    throw Error(ErrorCode::invalid_argument,
                "unknown GPU performance mode: " + std::string(value));
}

} // namespace

void EffectiveConfig::validate() const {
    if (schema_version != 1) {
        throw Error(ErrorCode::invalid_argument, "only configuration schema version 1 is supported");
    }
    if (target_profile != "DaVinciIntermediate_DWG") {
        throw Error(ErrorCode::unsupported_format, "v0.1 supports only DaVinciIntermediate_DWG");
    }
    static const std::set<std::string_view> prores_profiles{
        "proxy", "lt", "standard", "hq", "4444", "4444xq"
    };
    if (!prores_profiles.contains(prores_profile)) {
        throw Error(ErrorCode::invalid_argument,
                    "prores_profile must be one of: proxy, lt, standard, hq, 4444, 4444xq");
    }
    if (!preserve_source_timestamps) {
        throw Error(ErrorCode::unsupported_format, "v0.1 requires source timestamp preservation");
    }
    if (cpu_threads > 256U) {
        throw Error(ErrorCode::invalid_argument, "cpu_threads must be between 0 (auto) and 256");
    }
    if (max_parallel_frames > 64U) {
        throw Error(ErrorCode::invalid_argument,
                    "max_parallel_frames must be between 0 (auto) and 64");
    }
    if (gpu_selector.empty()) {
        throw Error(ErrorCode::invalid_argument, "gpu_selector must not be empty");
    }
    if (async_depth == 0U || async_depth > 64U) {
        throw Error(ErrorCode::invalid_argument, "async_depth must be between 1 and 64");
    }
    if (!std::isfinite(capture_sharpening) || capture_sharpening < 0.0 ||
        capture_sharpening > 2.0) {
        throw Error(ErrorCode::invalid_argument,
                    "capture_sharpening must be between 0 and 2");
    }
    if (!std::isfinite(capture_sharpening_threshold) ||
        capture_sharpening_threshold < 0.0 || capture_sharpening_threshold > 0.1) {
        throw Error(ErrorCode::invalid_argument,
                    "capture_sharpening_threshold must be between 0 and 0.1");
    }
}

EffectiveConfig load_config(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw Error(ErrorCode::io_failed, "cannot open configuration: " + path.string());
    }
    const nlohmann::json value = nlohmann::json::parse(stream, nullptr, true, true);
    if (!value.is_object()) {
        throw Error(ErrorCode::invalid_argument, "configuration root must be an object");
    }
    static const std::set<std::string, std::less<>> allowed{
        "schema_version", "demosaic", "exposure_offset_stops",
        "negative_policy", "chroma_filter", "capture_sharpening",
        "capture_sharpening_threshold", "deterministic_dither",
        "preserve_source_timestamps", "preserve_audio", "max_frames",
        "cpu_threads", "max_parallel_frames",
        "target_profile", "prores_profile", "backend", "gpu_selector",
        "async_depth", "fallback", "gpu_performance_mode"
    };
    for (const auto& [key, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (!allowed.contains(key)) {
            throw Error(ErrorCode::invalid_argument, "unknown configuration key: " + key);
        }
    }
    EffectiveConfig config;
    config.schema_version = value.value("schema_version", config.schema_version);
    config.demosaic = parse_enum<DemosaicAlgorithm>(value.value("demosaic", std::string(to_string(config.demosaic))));
    config.exposure_offset_stops = value.value("exposure_offset_stops", config.exposure_offset_stops);
    config.negative_policy = parse_enum<NegativePolicy>(value.value("negative_policy", std::string(to_string(config.negative_policy))));
    config.chroma_filter = parse_enum<ChromaFilter>(value.value("chroma_filter", std::string(to_string(config.chroma_filter))));
    config.capture_sharpening = value.value("capture_sharpening", config.capture_sharpening);
    config.capture_sharpening_threshold = value.value(
        "capture_sharpening_threshold", config.capture_sharpening_threshold);
    config.deterministic_dither = value.value("deterministic_dither", config.deterministic_dither);
    config.preserve_source_timestamps = value.value("preserve_source_timestamps", config.preserve_source_timestamps);
    config.preserve_audio = value.value("preserve_audio", config.preserve_audio);
    config.max_frames = value.value("max_frames", config.max_frames);
    config.cpu_threads = value.value("cpu_threads", config.cpu_threads);
    config.max_parallel_frames = value.value("max_parallel_frames", config.max_parallel_frames);
    config.target_profile = value.value("target_profile", config.target_profile);
    config.prores_profile = value.value("prores_profile", config.prores_profile);
    config.backend = parse_enum<VideoBackend>(value.value(
        "backend", std::string(to_string(config.backend))));
    config.gpu_selector = value.value("gpu_selector", config.gpu_selector);
    config.async_depth = value.value("async_depth", config.async_depth);
    config.fallback = parse_enum<GpuFallback>(value.value(
        "fallback", std::string(to_string(config.fallback))));
    config.gpu_performance_mode = parse_enum<GpuPerformanceMode>(value.value(
        "gpu_performance_mode",
        std::string(to_string(config.gpu_performance_mode))));
    config.validate();
    return config;
}

nlohmann::json config_to_json(const EffectiveConfig& config) {
    config.validate();
    return {
        {"schema_version", config.schema_version},
        {"demosaic", to_string(config.demosaic)},
        {"exposure_offset_stops", config.exposure_offset_stops},
        {"negative_policy", to_string(config.negative_policy)},
        {"chroma_filter", to_string(config.chroma_filter)},
        {"capture_sharpening", config.capture_sharpening},
        {"capture_sharpening_threshold", config.capture_sharpening_threshold},
        {"deterministic_dither", config.deterministic_dither},
        {"preserve_source_timestamps", config.preserve_source_timestamps},
        {"preserve_audio", config.preserve_audio},
        {"max_frames", config.max_frames},
        {"cpu_threads", config.cpu_threads},
        {"max_parallel_frames", config.max_parallel_frames},
        {"target_profile", config.target_profile},
        {"prores_profile", config.prores_profile},
        {"backend", to_string(config.backend)},
        {"gpu_selector", config.gpu_selector},
        {"async_depth", config.async_depth},
        {"fallback", to_string(config.fallback)},
        {"gpu_performance_mode", to_string(config.gpu_performance_mode)},
        {"packing", {
            {"pixel_format", "yuv422p10le"},
            {"range", "video"},
            {"matrix", "bt2020_ncl"},
            {"chroma_siting", "left"}
        }}
    };
}

std::string_view to_string(DemosaicAlgorithm value) noexcept {
    switch (value) {
    case DemosaicAlgorithm::rcd: return "rcd";
    case DemosaicAlgorithm::amaze: return "amaze";
    case DemosaicAlgorithm::igv: return "igv";
    case DemosaicAlgorithm::dcb: return "dcb";
    case DemosaicAlgorithm::lmmse: return "lmmse";
    }
    return "unknown";
}

std::string_view to_string(NegativePolicy value) noexcept {
    switch (value) {
    case NegativePolicy::preserve_by_curve: return "preserve_by_curve";
    case NegativePolicy::clamp_zero: return "clamp_zero";
    case NegativePolicy::error: return "error";
    }
    return "unknown";
}

std::string_view to_string(ChromaFilter value) noexcept {
    switch (value) {
    case ChromaFilter::fast: return "fast";
    case ChromaFilter::quality: return "quality";
    }
    return "unknown";
}

std::string_view to_string(VideoBackend value) noexcept {
    switch (value) {
    case VideoBackend::automatic: return "auto";
    case VideoBackend::cpu: return "cpu";
    case VideoBackend::vulkan: return "vulkan";
    }
    return "unknown";
}

std::string_view to_string(GpuFallback value) noexcept {
    switch (value) {
    case GpuFallback::prores_ks: return "prores_ks";
    case GpuFallback::none: return "none";
    }
    return "unknown";
}

std::string_view to_string(GpuPrecision value) noexcept {
    switch (value) {
    case GpuPrecision::fp32: return "fp32";
    case GpuPrecision::fp16: return "fp16";
    }
    return "unknown";
}

std::string_view to_string(GpuPerformanceMode value) noexcept {
    switch (value) {
    case GpuPerformanceMode::precise: return "precise";
    case GpuPerformanceMode::fast: return "fast";
    }
    return "unknown";
}

} // namespace mcraw
