#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <deque>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <mcraw/core/config.hpp>
#include <mcraw/core/error.hpp>
#include <mcraw/core/timing.hpp>
#include <mcraw/core/worker_pool.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/output/sidecar.hpp>
#include <mcraw/output/backend_selection.hpp>
#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/processing/demosaic.hpp>
#include <mcraw/processing/log_curve.hpp>
#include <mcraw/processing/pipeline.hpp>
#include <mcraw/processing/yuv.hpp>

#if MCRAW_HAS_FFMPEG
#include <mcraw/output/ffmpeg_writer.hpp>
#endif
#if MCRAW_HAS_VULKAN
#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>
#endif

namespace {

using mcraw::Error;
using mcraw::ErrorCode;

class Arguments {
public:
    Arguments(int argc, char** argv) {
        values_.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) values_.emplace_back(argv[i]);
    }

    [[nodiscard]] std::string_view at(std::size_t index) const {
        if (index >= values_.size()) throw Error(ErrorCode::invalid_argument, "missing command argument");
        return values_[index];
    }

    [[nodiscard]] std::optional<std::string_view> option(std::string_view name) const {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            if (values_[i] == name) {
                if (i + 1U >= values_.size()) {
                    throw Error(ErrorCode::invalid_argument, "option requires a value: " + std::string(name));
                }
                return values_[i + 1U];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool flag(std::string_view name) const {
        return std::find(values_.begin(), values_.end(), name) != values_.end();
    }

private:
    std::vector<std::string_view> values_;
};

std::size_t parse_size(std::string_view value, std::string_view name) {
    std::size_t consumed = 0;
    unsigned long long result = 0;
    try {
        result = std::stoull(std::string(value), &consumed);
    } catch (const std::exception&) {
        throw Error(ErrorCode::invalid_argument, "invalid integer for " + std::string(name));
    }
    if (consumed != value.size() || result > std::numeric_limits<std::size_t>::max()) {
        throw Error(ErrorCode::invalid_argument, "invalid integer for " + std::string(name));
    }
    return static_cast<std::size_t>(result);
}

mcraw::EffectiveConfig effective_config(const Arguments& args) {
    mcraw::EffectiveConfig config;
    if (const auto path = args.option("--config")) {
        config = mcraw::load_config(std::filesystem::path(std::string(*path)));
    }
    if (const auto frames = args.option("--frames")) config.max_frames = parse_size(*frames, "--frames");
    config.validate();
    return config;
}

struct CpuExecutionPlan {
    std::size_t total_threads{};
    std::size_t parallel_frames{};
    std::size_t threads_per_frame{};
    std::size_t encode_contexts{};
    std::size_t encode_threads_per_context{};
};

std::uint64_t available_memory_bytes() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) return status.ullAvailPhys;
#elif defined(__linux__)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
    }
#endif
    return 0;
}

CpuExecutionPlan resolve_execution_plan(mcraw::EffectiveConfig& config,
                                        std::size_t selected_frames) {
    const auto detected = std::max(1U, std::thread::hardware_concurrency());
    const auto total_threads = config.cpu_threads == 0U
        ? static_cast<std::size_t>(detected) : config.cpu_threads;
    // The fused path peaks around 300 MiB per 4K frame on the reference sample.
    // Auto mode uses at most a quarter of currently available RAM and keeps at
    // least two worker threads per frame; explicit configuration may override it.
    constexpr std::uint64_t estimated_frame_bytes = 384ULL * 1024ULL * 1024ULL;
    const auto available_memory = available_memory_bytes();
    const auto memory_frames = available_memory == 0U
        ? std::numeric_limits<std::size_t>::max()
        : std::max<std::size_t>(1U, static_cast<std::size_t>(
            available_memory / 4U / estimated_frame_bytes));
    const auto automatic_frames = std::max<std::size_t>(1U, std::min({
        static_cast<std::size_t>(6U), std::max<std::size_t>(1U, total_threads / 2U),
        memory_frames
    }));
    const auto requested_frames = config.max_parallel_frames == 0U
        ? automatic_frames : config.max_parallel_frames;
    const auto parallel_frames = std::max<std::size_t>(1U, std::min({
        requested_frames, selected_frames, total_threads
    }));
    const auto threads_per_frame = std::max<std::size_t>(1U, total_threads / parallel_frames);
    // ProRes encoding overlaps with pipeline compute on a pool of
    // single-threaded intra-frame encoder contexts. Frame-parallel contexts
    // are the most CPU-efficient shape for prores_ks (its slice threading
    // measured ~2x on 4 threads; a lone context therefore caps throughput),
    // and a 12.6 MP HQ encode costs about as much CPU as the RAW pipeline
    // itself. The context count scales with the thread budget instead of a
    // fixed compute/encode split: on machines where compute dominates, idle
    // encoder workers sleep without consuming cores, so the balance adapts
    // to the CPU architecture without hand-tuned constants.
    const auto encode_contexts = std::clamp<std::size_t>(
        total_threads, 1U, 16U);
    const auto encode_threads_per_context = std::size_t{1U};
    config.cpu_threads = total_threads;
    config.max_parallel_frames = parallel_frames;
    return {total_threads, parallel_frames, threads_per_frame,
            encode_contexts, encode_threads_per_context};
}

struct FrameTaskResult {
    mcraw::ProcessedFrame frame;
    mcraw::StageTimings timings;
};

std::future<FrameTaskResult> submit_frame_task(mcraw::PersistentWorkerPool& workers,
                                               const mcraw::McrawReader& reader,
                                               const mcraw::CpuPipeline& pipeline,
                                               std::size_t frame_index) {
    return workers.submit([&reader, &pipeline, frame_index] {
        FrameTaskResult result;
        result.frame = pipeline.process(reader, frame_index, result.timings);
        return result;
    });
}

nlohmann::json inspect_document(mcraw::McrawReader& reader, bool include_raw) {
    if (reader.frames().empty()) throw Error(ErrorCode::invalid_container, "MCRAW contains no video frames");
    double interval_total_ms = 0.0;
    for (std::size_t i = 1; i < reader.frames().size(); ++i) {
        interval_total_ms += static_cast<double>(reader.frames()[i].timestamp_ns -
                                                 reader.frames()[i - 1U].timestamp_ns) / 1.0e6;
    }
    const double mean_interval = reader.frames().size() > 1U
        ? interval_total_ms / static_cast<double>(reader.frames().size() - 1U) : 0.0;
    const auto first_raw = reader.frame_metadata(0);
    const auto first = mcraw::normalize_metadata(reader.container_metadata(), first_raw);
    const auto last_raw = reader.frame_metadata(reader.frames().size() - 1U);
    const auto last = mcraw::normalize_metadata(reader.container_metadata(), last_raw);
    const auto audio = reader.load_audio();

    nlohmann::json result = {
        {"path", reader.path().string()},
        {"container_version", reader.container_version()},
        {"frame_count", reader.frames().size()},
        {"first_timestamp_ns", reader.frames().front().timestamp_ns},
        {"last_timestamp_ns", reader.frames().back().timestamp_ns},
        {"duration_seconds", static_cast<double>(reader.frames().back().timestamp_ns -
                                                  reader.frames().front().timestamp_ns) / 1.0e9},
        {"mean_frame_interval_ms", mean_interval},
        {"estimated_fps", mean_interval > 0.0 ? 1000.0 / mean_interval : 0.0},
        {"first_frame", mcraw::metadata_to_json(first)},
        {"last_frame", mcraw::metadata_to_json(last)},
        {"audio", {{"sample_rate", audio.sample_rate}, {"channels", audio.channels},
                    {"chunks", audio.chunks.size()}}}
    };
    if (include_raw) {
        result["raw_container_metadata"] = reader.container_metadata();
        result["raw_first_frame_metadata"] = first_raw;
        result["raw_last_frame_metadata"] = last_raw;
    }
    return result;
}

void write_bytes(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw Error(ErrorCode::io_failed, "cannot create output: " + path.string());
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!stream) throw Error(ErrorCode::io_failed, "failed while writing output: " + path.string());
}

void write_pfm(const std::filesystem::path& path, const mcraw::PlanarRgbF32& image) {
    image.validate();
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw Error(ErrorCode::io_failed, "cannot create PFM: " + path.string());
    stream << "PF\n" << image.width << ' ' << image.height << "\n-1.0\n";
    std::array<float, 3> pixel{};
    for (std::uint32_t y = image.height; y-- > 0U;) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto p = static_cast<std::size_t>(y) * image.width + x;
            for (std::size_t c = 0; c < 3; ++c) pixel[c] = image.planes[c][p];
            stream.write(reinterpret_cast<const char*>(pixel.data()), sizeof(pixel));
        }
    }
    if (!stream) throw Error(ErrorCode::io_failed, "failed while writing PFM: " + path.string());
}

std::uint64_t fnv1a(const void* input, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int command_inspect(const Arguments& args) {
    mcraw::McrawReader reader(std::filesystem::path(std::string(args.at(2))));
    std::cout << inspect_document(reader, args.flag("--raw-json")).dump(2) << '\n';
    return 0;
}

#if MCRAW_HAS_VULKAN
nlohmann::json vulkan_device_json(const mcraw::VulkanDeviceInfo& device) {
    nlohmann::json queues = nlohmann::json::array();
    for (const auto& family : device.queue_families) {
        queues.push_back({
            {"index", family.index}, {"count", family.queue_count},
            {"flags", family.flags},
            {"compute", (family.flags & VK_QUEUE_COMPUTE_BIT) != 0U},
            {"graphics", (family.flags & VK_QUEUE_GRAPHICS_BIT) != 0U},
            {"transfer", (family.flags & VK_QUEUE_TRANSFER_BIT) != 0U}
        });
    }
    return {
        {"index", device.enumeration_index}, {"name", device.name},
        {"type", mcraw::vulkan_device_type_name(device.type)},
        {"vendor_id", device.vendor_id}, {"device_id", device.device_id},
        {"uuid", device.uuid},
        {"api_version", {
            {"major", VK_VERSION_MAJOR(device.api_version)},
            {"minor", VK_VERSION_MINOR(device.api_version)},
            {"patch", VK_VERSION_PATCH(device.api_version)}
        }},
        {"driver_version_raw", device.driver_version},
        {"driver_name", device.driver_name}, {"driver_info", device.driver_info},
        {"software", device.software}, {"queue_families", std::move(queues)}
    };
}

nlohmann::json vulkan_runtime_report() {
    nlohmann::json devices = nlohmann::json::array();
    try {
        for (const auto& device : mcraw::VulkanRuntime::enumerate_devices()) {
            devices.push_back(vulkan_device_json(device));
        }
        mcraw::VulkanRuntime runtime;
        nlohmann::json frame_context;
        try {
            mcraw::FfmpegVulkanFrameContext frames(runtime, {64, 32, 4});
            mcraw::FrameMetadata metadata;
            metadata.width = 64;
            metadata.height = 32;
            metadata.time_base = {1, 90'000};
            auto frame = frames.allocate_frame(metadata);
            const auto allocation = frames.inspect_frame(*frame.frame);
            frame_context = {
                {"available", true}, {"software_format", "yuv422p10le"},
                {"pool_size", frames.pool_size()}, {"image_count", allocation.image_count},
                {"image_formats", allocation.formats}, {"image_usage", frames.image_usage()}
            };
        } catch (const std::exception& error) {
            frame_context = {{"available", false}, {"reason", error.what()}};
        }
        return {
            {"available", true}, {"selected", vulkan_device_json(runtime.device())},
            {"compute_queue_family", runtime.compute_queue_family()},
            {"compute_queue_count", runtime.compute_queue_count()},
            {"instance_extensions", runtime.instance_extensions()},
            {"device_extensions", runtime.device_extensions()},
            {"frame_context", std::move(frame_context)},
            {"devices", std::move(devices)}
        };
    } catch (const std::exception& error) {
        return {{"available", false}, {"reason", error.what()}, {"devices", std::move(devices)}};
    }
}
#endif

int command_list_capabilities() {
    const auto capabilities = mcraw::probe_backend_capabilities("auto", 64, 32, 4);
    nlohmann::json vulkan_runtime = {{"available", false}, {"reason", "not compiled"}};
#if MCRAW_HAS_VULKAN
    vulkan_runtime = vulkan_runtime_report();
#endif
    nlohmann::json result = {
        {"version", "0.1.0"}, {"license", "GPL-3.0-or-later"},
        {"platforms", {"Windows 10/11", "Linux (build-compatible, not yet validated)"}},
        {"backends", {
            {"cpu", {{"available", capabilities.cpu_available}, {"encoder", "prores_ks"}}},
            {"vulkan", {
                {"compiled", capabilities.vulkan_compiled},
                {"available", capabilities.vulkan_backend_available},
                {"encoder_available", capabilities.prores_ks_vulkan_available},
                {"encoder", "prores_ks_vulkan"},
                {"reason", capabilities.vulkan_unavailable_reason},
                {"runtime", std::move(vulkan_runtime)}
            }}
        }}, {"cfa", {"rggb", "bggr", "grbg", "gbrg"}},
        {"demosaic", {"rcd", "amaze", "igv", "dcb", "lmmse"}},
        {"optional_processing", {"capture_sharpening"}},
        {"color_profiles", {"DaVinciIntermediate_DWG"}},
        {"packing", {"ProRes422HQ", "yuv422p10le", "video_range", "bt2020_ncl_provisional"}},
        {"ffmpeg", {
            {"enabled", static_cast<bool>(MCRAW_HAS_FFMPEG)},
            {"version", capabilities.ffmpeg_version},
            {"configuration", capabilities.ffmpeg_configuration}
        }},
        {"librtprocess", static_cast<bool>(MCRAW_HAS_RTPROCESS)}
    };
    std::cout << result.dump(2) << '\n';
    return 0;
}

int command_vulkan_smoke(const Arguments& args) {
#if !MCRAW_HAS_VULKAN
    static_cast<void>(args);
    throw Error(ErrorCode::unsupported_format, "this build has no Vulkan support");
#else
    const auto iterations = parse_size(args.option("--iterations").value_or("1"), "--iterations");
    if (iterations == 0U || iterations > 10'000U) {
        throw Error(ErrorCode::invalid_argument, "--iterations must be between 1 and 10000");
    }
    const std::string selector(args.option("--gpu").value_or("auto"));
    const auto start = std::chrono::steady_clock::now();
    nlohmann::json selected;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        mcraw::VulkanRuntime runtime({selector, args.flag("--validation")});
        if (iteration == 0U) selected = vulkan_device_json(runtime.device());
    }
    const auto wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << nlohmann::json{
        {"ok", true}, {"iterations", iterations}, {"wall_ms", wall_ms},
        {"mean_ms", wall_ms / static_cast<double>(iterations)},
        {"selected", std::move(selected)}}.dump(2) << '\n';
    return 0;
#endif
}

int command_print_effective_config(const Arguments& args) {
    std::cout << mcraw::config_to_json(effective_config(args)).dump(2) << '\n';
    return 0;
}

int command_extract(const Arguments& args) {
    mcraw::McrawReader reader(std::filesystem::path(std::string(args.at(2))));
    const auto config = effective_config(args);
    const auto frame = parse_size(args.option("--frame").value_or("0"), "--frame");
    const auto stage = args.option("--stage").value_or("raw-u16");
    const auto output_option = args.option("--output");
    if (!output_option) throw Error(ErrorCode::invalid_argument, "extract-frame requires --output");
    const std::filesystem::path output{std::string(*output_option)};
    if (stage == "compressed") {
        const auto value = reader.load_compressed_frame(frame);
        write_bytes(output, value.bytes.data(), value.bytes.size());
        return 0;
    }
    const auto metadata = reader.normalized_metadata(frame);
    const auto raw = reader.load_reference_frame(frame);
    if (stage == "raw-u16") {
        write_bytes(output, raw.pixels.data(), raw.pixels.size() * sizeof(std::uint16_t));
        return 0;
    }
    if (stage == "calibrated-mosaic-f32" || stage == "camera-rgb-f32" ||
        stage == "target-linear-production-f32" ||
        stage == "target-log-production-f32" ||
        stage == "yuv422p10-production") {
        const auto calibrated = mcraw::calibrate_raw_for_demosaic(raw, metadata, 1);
        if (stage == "calibrated-mosaic-f32") {
            write_bytes(output, calibrated.pixels.data(),
                        calibrated.pixels.size() * sizeof(float));
            return 0;
        }
        const auto camera = mcraw::demosaic_unnormalized(calibrated, config.demosaic, 1);
        if (stage == "camera-rgb-f32") {
            write_pfm(output, camera);
            return 0;
        }
        const auto solution = mcraw::build_camera_color_solution(metadata);
        auto target_linear = mcraw::camera_to_dwg(
            camera, solution, config.exposure_offset_stops, 1.0 / 65535.0, 1);
        target_linear = mcraw::sharpen_target_linear(
            std::move(target_linear), config.capture_sharpening,
            config.capture_sharpening_threshold, 1);
        if (stage == "target-linear-production-f32") {
            write_pfm(output, target_linear);
            return 0;
        }
        mcraw::DaVinciIntermediateLut curve;
        const auto target_log = mcraw::encode_davinci_intermediate_lut(
            std::move(target_linear), config.negative_policy, curve, 1);
        if (stage == "target-log-production-f32") {
            write_pfm(output, target_log);
            return 0;
        }
        const auto packed = mcraw::pack_dwg_log_to_yuv422p10(
            target_log, config.chroma_filter, config.deterministic_dither, frame);
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) throw Error(ErrorCode::io_failed, "cannot create production YUV output");
        for (const auto* plane : {&packed.image.y, &packed.image.cb, &packed.image.cr}) {
            stream.write(reinterpret_cast<const char*>(plane->data()),
                         static_cast<std::streamsize>(plane->size() * sizeof(std::uint16_t)));
        }
        if (!stream) {
            throw Error(ErrorCode::io_failed,
                        "failed while writing production YUV output");
        }
        return 0;
    }
    const auto normalized = mcraw::calibrate_raw(raw, metadata);
    if (stage == "normalized-bayer") {
        write_bytes(output, normalized.pixels.data(), normalized.pixels.size() * sizeof(float));
        return 0;
    }
    const auto camera = mcraw::demosaic(normalized, mcraw::DemosaicAlgorithm::rcd);
    if (stage == "linear-rgb") {
        write_pfm(output, camera);
        return 0;
    }
    const auto solution = mcraw::build_camera_color_solution(metadata);
    const auto target_linear = mcraw::camera_to_dwg(camera, solution);
    if (stage == "target-linear-rgb") {
        write_pfm(output, target_linear);
        return 0;
    }
    const auto target_log = mcraw::encode_davinci_intermediate(target_linear,
        mcraw::NegativePolicy::preserve_by_curve);
    if (stage == "target-log-rgb") {
        write_pfm(output, target_log);
        return 0;
    }
    if (stage == "yuv422p10") {
        const auto packed = mcraw::pack_dwg_log_to_yuv422p10(target_log,
            mcraw::ChromaFilter::quality, true, frame);
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) throw Error(ErrorCode::io_failed, "cannot create YUV output");
        for (const auto* plane : {&packed.image.y, &packed.image.cb, &packed.image.cr}) {
            stream.write(reinterpret_cast<const char*>(plane->data()),
                         static_cast<std::streamsize>(plane->size() * sizeof(std::uint16_t)));
        }
        if (!stream) throw Error(ErrorCode::io_failed, "failed while writing YUV output");
        return 0;
    }
    throw Error(ErrorCode::invalid_argument, "unknown extract stage: " + std::string(stage));
}

int command_validate(const Arguments& args) {
    mcraw::McrawReader reader(std::filesystem::path(std::string(args.at(2))));
    const auto frame = parse_size(args.option("--frame").value_or("0"), "--frame");
    const auto compressed = reader.load_compressed_frame(frame);
    const auto raw = reader.load_reference_frame(frame);
    const auto metadata = reader.normalized_metadata(frame);
    const auto normalized = mcraw::calibrate_raw(raw, metadata);
    const auto solution = mcraw::build_camera_color_solution(metadata);
    const auto raw_hash = fnv1a(raw.pixels.data(), raw.pixels.size() * sizeof(std::uint16_t));
    nlohmann::json result = {
        {"ok", true}, {"frame", frame}, {"timestamp_ns", compressed.timestamp_ns},
        {"compressed_bytes", compressed.bytes.size()}, {"raw_pixels", raw.pixels.size()},
        {"raw_fnv1a64", raw_hash},
        {"normalized_min", *std::min_element(normalized.pixels.begin(), normalized.pixels.end())},
        {"normalized_max", *std::max_element(normalized.pixels.begin(), normalized.pixels.end())},
        {"white_point_xy", {solution.white_point.x, solution.white_point.y}},
        {"white_point_cct", solution.white_point.cct},
        {"matrix_weight", solution.white_point.interpolation_weight},
        {"used_forward_matrix", solution.used_forward_matrix},
        {"di_18_percent", mcraw::davinci_intermediate_oetf(0.18)},
        {"notes", "RAW hash is a reproducible golden candidate; GPU/custom decoder comparison is not available in v0.1"}
    };
    if (args.flag("--compare-fused")) {
        auto config = effective_config(args);
        const auto execution = resolve_execution_plan(config, 1U);
        const auto camera = mcraw::demosaic(normalized, config.demosaic,
                                             execution.threads_per_frame);
        const auto unsharpened_target_linear = mcraw::camera_to_dwg(
            camera, solution, config.exposure_offset_stops);
        const auto target_linear = mcraw::sharpen_target_linear(
            unsharpened_target_linear, config.capture_sharpening,
            config.capture_sharpening_threshold);
        const auto target_log = mcraw::encode_davinci_intermediate(
            target_linear, config.negative_policy);
        const auto reference = mcraw::pack_dwg_log_to_yuv422p10(
            target_log, config.chroma_filter, config.deterministic_dither, frame);
        const mcraw::DaVinciIntermediateLut curve;
        const auto fused = mcraw::pack_camera_to_dwg_di_yuv422p10(
            camera, solution, config.exposure_offset_stops, config.negative_policy,
            curve, config.chroma_filter, config.deterministic_dither, frame,
            execution.threads_per_frame, config.capture_sharpening,
            config.capture_sharpening_threshold);
        std::size_t compared = 0;
        std::size_t differing = 0;
        int maximum_difference = 0;
        const auto compare = [&](const auto& expected, const auto& actual) {
            if (expected.size() != actual.size()) {
                throw Error(ErrorCode::processing_failed,
                            "fused/reference comparison plane size mismatch");
            }
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const int difference = std::abs(
                    static_cast<int>(expected[i]) - static_cast<int>(actual[i]));
                maximum_difference = std::max(maximum_difference, difference);
                differing += difference != 0 ? 1U : 0U;
            }
            compared += expected.size();
        };
        compare(reference.image.y, fused.image.y);
        compare(reference.image.cb, fused.image.cb);
        compare(reference.image.cr, fused.image.cr);
        result["fused_reference_comparison"] = {
            {"samples", compared},
            {"differing_samples", differing},
            {"differing_fraction", compared == 0U ? 0.0 :
                static_cast<double>(differing) / static_cast<double>(compared)},
            {"maximum_10bit_code_difference", maximum_difference}
        };
    }
    std::cout << result.dump(2) << '\n';
    return 0;
}

int command_benchmark(const Arguments& args) {
    mcraw::McrawReader reader(std::filesystem::path(std::string(args.at(2))));
    auto config = effective_config(args);
    const auto frames = std::min(reader.frames().size(), config.max_frames == 0U
        ? reader.frames().size() : config.max_frames);
    if (frames == 0U) throw Error(ErrorCode::invalid_argument, "benchmark selected zero frames");
    const auto execution = resolve_execution_plan(config, frames);
    mcraw::CpuPipeline pipeline(config, execution.threads_per_frame);
    mcraw::StageTimings timings;
    mcraw::PersistentWorkerPool workers(execution.parallel_frames,
                                        execution.parallel_frames);
    std::deque<std::future<FrameTaskResult>> pending;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < frames; ++i) {
        pending.push_back(submit_frame_task(workers, reader, pipeline, i));
        if (pending.size() >= execution.parallel_frames) {
            auto completed = pending.front().get();
            pending.pop_front();
            timings.merge(completed.timings);
        }
    }
    while (!pending.empty()) {
        auto completed = pending.front().get();
        pending.pop_front();
        timings.merge(completed.timings);
    }
    const auto wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const auto worker_telemetry = workers.telemetry();
    std::cout << nlohmann::json{{"mode", "compute-only"}, {"frames", frames},
                                {"wall_ms", wall_ms},
                                {"throughput_fps", static_cast<double>(frames) * 1000.0 / wall_ms},
                                {"execution", {
                                    {"cpu_threads", execution.total_threads},
                                    {"parallel_frames", execution.parallel_frames},
                                    {"threads_per_frame", execution.threads_per_frame},
                                    {"encode_contexts", execution.encode_contexts},
                                    {"encode_threads_per_context", execution.encode_threads_per_context},
                                    {"worker_queue_capacity", worker_telemetry.queue_capacity},
                                    {"worker_queue_max_depth", worker_telemetry.max_queue_depth},
                                    {"worker_submit_waits", worker_telemetry.submit_waits},
                                    {"worker_submit_wait_ms", worker_telemetry.submit_wait_ms},
                                    {"worker_tasks_started", worker_telemetry.tasks_started},
                                    {"worker_tasks_completed", worker_telemetry.tasks_completed}
                                }},
                                {"stages", timings.to_json()}}.dump(2) << '\n';
    return 0;
}

std::vector<mcraw::AudioChunk> normalize_audio_timestamps(const mcraw::AudioInfo& audio,
                                                           std::vector<std::string>& warnings) {
    auto chunks = audio.chunks;
    if (chunks.empty()) return chunks;
    std::int64_t previous = -1;
    std::int64_t cursor = chunks.front().timestamp_ns;
    std::int64_t maximum_correction_ns = 0;
    for (auto& chunk : chunks) {
        if (chunk.timestamp_ns < 0) {
            throw Error(ErrorCode::invalid_container,
                        "audio source timestamp is required by the v0.1 contract");
        }
        if (previous >= 0 && chunk.timestamp_ns <= previous) {
            throw Error(ErrorCode::invalid_container, "audio source timestamps are not strictly increasing");
        }
        previous = chunk.timestamp_ns;
        maximum_correction_ns = std::max(maximum_correction_ns,
                                         std::abs(chunk.timestamp_ns - cursor));
        chunk.timestamp_ns = cursor;
        const auto samples_per_channel = chunk.interleaved_samples.size() /
                                         static_cast<std::size_t>(audio.channels);
        cursor += static_cast<std::int64_t>(std::llround(
            static_cast<double>(samples_per_channel) * 1.0e9 / audio.sample_rate));
    }
    if (maximum_correction_ns > 0) {
        warnings.emplace_back("audio capture timestamp jitter normalized to a continuous PCM clock; "
                              "maximum correction ms=" +
                              std::to_string(static_cast<double>(maximum_correction_ns) / 1.0e6));
    }
    return chunks;
}

mcraw::AvSyncReport av_sync_report(const std::vector<mcraw::AudioChunk>& chunks,
                                   int sample_rate,
                                   int channels,
                                   std::int64_t video_start_ns,
                                   std::int64_t video_end_ns) {
    mcraw::AvSyncReport result;
    result.audio_present = !chunks.empty();
    result.audio_chunks = chunks.size();
    if (chunks.empty()) return result;
    const auto& last = chunks.back();
    const auto samples = last.interleaved_samples.size() / static_cast<std::size_t>(channels);
    const auto audio_end_ns = last.timestamp_ns + static_cast<std::int64_t>(std::llround(
        static_cast<double>(samples) * 1.0e9 / sample_rate));
    result.start_delta_ms = static_cast<double>(chunks.front().timestamp_ns - video_start_ns) / 1.0e6;
    result.end_delta_ms = static_cast<double>(audio_end_ns - video_end_ns) / 1.0e6;
    return result;
}

int command_convert(const Arguments& args) {
#if !MCRAW_HAS_FFMPEG
    static_cast<void>(args);
    throw Error(ErrorCode::unsupported_format, "this build has no FFmpeg support");
#else
    const auto process_start = std::chrono::steady_clock::now();
    const std::filesystem::path input{std::string(args.at(2))};
    const std::filesystem::path output{std::string(args.at(3))};
    if (std::filesystem::exists(output) && !args.flag("--overwrite")) {
        throw Error(ErrorCode::io_failed, "output already exists; pass --overwrite to replace it");
    }
    mcraw::McrawReader reader(input);
    if (reader.frames().empty()) throw Error(ErrorCode::invalid_container, "MCRAW contains no video frames");
    auto config = effective_config(args);
    const auto first_metadata = reader.normalized_metadata(0);
    const auto capabilities = config.backend == mcraw::VideoBackend::cpu
        ? mcraw::probe_backend_capabilities()
        : mcraw::probe_backend_capabilities(
            config.gpu_selector, static_cast<int>(first_metadata.width),
            static_cast<int>(first_metadata.height),
            std::clamp<std::size_t>(config.async_depth * 2U + 4U, 8U, 64U),
            config.chroma_filter, config.deterministic_dither,
            mcraw::GpuPrecision::fp32);
    const auto backend = mcraw::select_backend(config, capabilities);
    const auto startup_preflight_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - process_start).count();
    const auto frame_limit = std::min(reader.frames().size(), config.max_frames == 0U
        ? reader.frames().size() : config.max_frames);
    if (frame_limit == 0U) throw Error(ErrorCode::invalid_argument, "conversion selected zero frames");
    const auto execution = resolve_execution_plan(config, frame_limit);

    std::vector<std::string> warnings{
        "v0.1 BT.2020 NCL packing matrix and left chroma siting remain provisional until Resolve chart validation"
    };
    if (backend.used_fallback) {
        warnings.emplace_back("Vulkan pipeline not selected; using CPU fallback: " + backend.reason);
    }
    mcraw::AudioInfo audio;
    if (config.preserve_audio) audio = reader.load_audio();
    if (config.preserve_audio && (audio.channels == 0 || audio.chunks.empty())) {
        throw Error(ErrorCode::invalid_container,
                    "audio preservation is required but the source has no readable audio chunks");
    }
    auto audio_chunks = audio.channels > 0
        ? normalize_audio_timestamps(audio, warnings)
        : std::vector<mcraw::AudioChunk>{};
    const auto video_end_ns = frame_limit < reader.frames().size()
        ? reader.frames()[frame_limit].timestamp_ns
        : reader.frames().back().timestamp_ns + (reader.frames().size() > 1U
            ? reader.frames().back().timestamp_ns - reader.frames()[reader.frames().size() - 2U].timestamp_ns
            : 0);
    if (frame_limit < reader.frames().size() && !audio_chunks.empty()) {
        audio_chunks.erase(std::remove_if(audio_chunks.begin(), audio_chunks.end(),
            [video_end_ns](const mcraw::AudioChunk& chunk) {
                return chunk.timestamp_ns >= video_end_ns;
            }), audio_chunks.end());
        if (!audio_chunks.empty()) {
            auto& last = audio_chunks.back();
            const auto available_ns = video_end_ns - last.timestamp_ns;
            const auto available_samples = available_ns > 0
                ? static_cast<std::size_t>(std::floor(
                    static_cast<double>(available_ns) * audio.sample_rate / 1.0e9))
                : 0U;
            const auto current_samples = last.interleaved_samples.size() /
                                         static_cast<std::size_t>(audio.channels);
            if (available_samples < current_samples) {
                last.interleaved_samples.resize(available_samples *
                                                static_cast<std::size_t>(audio.channels));
            }
            if (last.interleaved_samples.empty()) audio_chunks.pop_back();
        }
        warnings.emplace_back("audio cropped to the interval selected by --frames");
    }
    std::int64_t origin = reader.frames().front().timestamp_ns;
    if (!audio_chunks.empty()) origin = std::min(origin, audio_chunks.front().timestamp_ns);

    auto partial = output;
    partial += ".partial.mov";
    std::error_code remove_error;
    std::filesystem::remove(partial, remove_error);
    mcraw::StageTimings timings;
    const bool direct_vulkan_pipeline = backend.backend == mcraw::VideoBackend::vulkan;
    mcraw::CpuPipeline pipeline(config, execution.threads_per_frame,
        direct_vulkan_pipeline ? mcraw::CpuPipelineOutput::raw_mosaic
                               : mcraw::CpuPipelineOutput::packed_yuv);
    auto first_solution = mcraw::build_camera_color_solution(first_metadata);
    const auto sync_report = av_sync_report(audio_chunks, audio.sample_rate, audio.channels,
        reader.frames().front().timestamp_ns, video_end_ns);
    std::size_t audio_index = 0;
    mcraw::FfmpegWriterTelemetry writer_telemetry;
    mcraw::WorkerPoolTelemetry worker_telemetry;
    const auto conversion_start = std::chrono::steady_clock::now();
    {
        mcraw::FfmpegWriter writer(partial, first_metadata.width, first_metadata.height, origin,
                                   audio.sample_rate, audio.channels,
                                   mcraw::VideoEncodeConcurrency{
                                       execution.encode_contexts,
                                       static_cast<int>(execution.encode_threads_per_context)},
                                   mcraw::FfmpegVideoBackendConfig{
                                       backend.backend, config.gpu_selector,
                                       config.async_depth, args.flag("--validation"),
                                       config.chroma_filter,
                                       config.deterministic_dither,
                                       config.gpu_performance_mode ==
                                               mcraw::GpuPerformanceMode::precise
                                           ? mcraw::GpuPrecision::fp32
                                           : mcraw::GpuPrecision::fp16,
                                       config.gpu_performance_mode,
                                       config.prores_profile});
        mcraw::PersistentWorkerPool workers(execution.parallel_frames,
                                            execution.parallel_frames);
        std::deque<std::future<FrameTaskResult>> pending;
        std::size_t frames_completed = 0;
        const auto consume_front = [&] {
            auto completed = pending.front().get();
            pending.pop_front();
            timings.merge(completed.timings);
            auto& processed = completed.frame;
            while (audio_index < audio_chunks.size() &&
                   audio_chunks[audio_index].timestamp_ns <= processed.timestamp_ns) {
                mcraw::StageTimer timer(timings, "audio_encode_mux");
                writer.write_audio(audio_chunks[audio_index++]);
            }
            warnings.insert(warnings.end(), processed.metadata.warnings.begin(),
                            processed.metadata.warnings.end());
            {
                // write_video now hands the frame to the encode pipeline; this
                // measures submission plus any backpressure wait, not the
                // encode itself, which overlaps with frame compute.
                mcraw::StageTimer timer(timings, "prores_submit_wait");
                if (direct_vulkan_pipeline) {
                    mcraw::VulkanRawMosaicInput input;
                    input.image = std::move(processed.raw_mosaic);
                    input.metadata = std::move(processed.metadata);
                    input.camera_to_target = processed.color_solution.camera_to_target;
                    input.exposure_offset_stops = config.exposure_offset_stops;
                    input.capture_sharpening = config.capture_sharpening;
                    input.capture_sharpening_threshold =
                        config.capture_sharpening_threshold;
                    input.negative_policy = config.negative_policy;
                    writer.write_video(std::move(input), processed.timestamp_ns,
                                       frames_completed);
                } else {
                    writer.write_video(std::move(processed.packed.image),
                                       processed.timestamp_ns);
                }
            }
            if (frames_completed == 0U) first_solution = processed.color_solution;
            ++frames_completed;
            std::cerr << "frame " << frames_completed << '/' << frame_limit << "\r" << std::flush;
        };
        for (std::size_t i = 0; i < frame_limit; ++i) {
            pending.push_back(submit_frame_task(workers, reader, pipeline, i));
            if (pending.size() >= execution.parallel_frames) consume_front();
        }
        while (!pending.empty()) consume_front();
        while (audio_index < audio_chunks.size()) {
            mcraw::StageTimer timer(timings, "audio_encode_mux");
            writer.write_audio(audio_chunks[audio_index++]);
        }
        writer.finish();
        writer_telemetry = writer.telemetry();
        worker_telemetry = workers.telemetry();
    }
    const auto conversion_core_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - conversion_start).count();
    timings.add("startup_preflight", startup_preflight_ms);
    timings.add("conversion_core", conversion_core_ms);
    {
        mcraw::StageTimer timer(timings, "output_validation");
        mcraw::validate_prores_mov_metadata(partial, config.prores_profile);
        if (args.flag("--verify-output")) {
            mcraw::validate_prores_mov(partial, frame_limit, config.prores_profile);
        } else if (writer_telemetry.video_packets != frame_limit) {
            throw Error(ErrorCode::encode_failed,
                        "writer video packet count does not match submitted frames");
        }
    }
    std::cerr << '\n';
    if (std::filesystem::exists(output)) std::filesystem::remove(output);
    std::filesystem::rename(partial, output);
    auto sidecar = output;
    sidecar += ".json";
    std::sort(warnings.begin(), warnings.end());
    warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());
    for (const auto& warning : warnings) std::cerr << "warning: " << warning << '\n';
    mcraw::PipelineBackendReport pipeline_report{
        std::string(mcraw::to_string(config.backend)), writer_telemetry.backend,
        config.async_depth, backend.used_fallback, backend.reason,
        writer_telemetry.gpu_resident,
        writer_telemetry.upload_frames, writer_telemetry.readback_frames,
        writer_telemetry.direct_frames, writer_telemetry.rgb_upload_bytes,
        writer_telemetry.compressed_input_upload_bytes,
        writer_telemetry.u16_raw_upload_bytes,
        writer_telemetry.fp16_rgb_upload_bytes,
        writer_telemetry.fp32_rgb_upload_bytes,
        writer_telemetry.compressed_packet_download_bytes,
        writer_telemetry.video_packets,
        writer_telemetry.gpu_queue_capacity, writer_telemetry.gpu_queue_max_depth,
        writer_telemetry.packet_queue_capacity, writer_telemetry.packet_queue_max_depth,
        writer_telemetry.backpressure_waits, writer_telemetry.backpressure_wait_ms,
        writer_telemetry.mux_bytes, writer_telemetry.mux_megabytes_per_second,
        writer_telemetry.gpu_timestamps_supported,
        writer_telemetry.rgb_to_yuv_gpu_timestamp_samples,
        writer_telemetry.rgb_to_yuv_gpu_total_ms,
        writer_telemetry.rgb_to_yuv_gpu_mean_ms,
        writer_telemetry.rgb_to_yuv_gpu_p50_ms,
        writer_telemetry.rgb_to_yuv_gpu_p95_ms,
        writer_telemetry.rgb_to_yuv_gpu_p99_ms,
        writer_telemetry.rgb_to_yuv_gpu_min_ms,
        writer_telemetry.rgb_to_yuv_gpu_max_ms,
        writer_telemetry.gpu_name,
        writer_telemetry.gpu_uuid, writer_telemetry.gpu_driver,
        capabilities.ffmpeg_version, capabilities.ffmpeg_configuration,
        writer_telemetry.pipeline_entry, writer_telemetry.pipeline_precision,
        writer_telemetry.demosaic_location,
        writer_telemetry.color_solution_location,
        writer_telemetry.target_log_fp32_upload_bytes,
        writer_telemetry.camera_rgb_fp32_upload_bytes};
    pipeline_report.performance_mode = writer_telemetry.performance_mode;
    pipeline_report.intermediate_storage = writer_telemetry.intermediate_storage;
    pipeline_report.di_implementation = writer_telemetry.di_implementation;
    pipeline_report.dither_mode = writer_telemetry.dither_mode;
    pipeline_report.demosaic_implementation =
        writer_telemetry.demosaic_implementation;
    pipeline_report.camera_to_dwg_gpu_timestamp_samples =
        writer_telemetry.camera_to_dwg_gpu_timestamp_samples;
    pipeline_report.camera_to_dwg_gpu_total_ms =
        writer_telemetry.camera_to_dwg_gpu_total_ms;
    pipeline_report.camera_to_dwg_gpu_mean_ms =
        writer_telemetry.camera_to_dwg_gpu_mean_ms;
    pipeline_report.camera_to_dwg_gpu_p50_ms =
        writer_telemetry.camera_to_dwg_gpu_p50_ms;
    pipeline_report.camera_to_dwg_gpu_p95_ms =
        writer_telemetry.camera_to_dwg_gpu_p95_ms;
    pipeline_report.camera_to_dwg_gpu_p99_ms =
        writer_telemetry.camera_to_dwg_gpu_p99_ms;
    pipeline_report.camera_to_dwg_gpu_min_ms =
        writer_telemetry.camera_to_dwg_gpu_min_ms;
    pipeline_report.camera_to_dwg_gpu_max_ms =
        writer_telemetry.camera_to_dwg_gpu_max_ms;
    pipeline_report.capture_sharpening_gpu_timestamp_samples =
        writer_telemetry.capture_sharpening_gpu_timestamp_samples;
    pipeline_report.capture_sharpening_gpu_total_ms =
        writer_telemetry.capture_sharpening_gpu_total_ms;
    pipeline_report.capture_sharpening_gpu_mean_ms =
        writer_telemetry.capture_sharpening_gpu_mean_ms;
    pipeline_report.capture_sharpening_gpu_p50_ms =
        writer_telemetry.capture_sharpening_gpu_p50_ms;
    pipeline_report.capture_sharpening_gpu_p95_ms =
        writer_telemetry.capture_sharpening_gpu_p95_ms;
    pipeline_report.capture_sharpening_gpu_p99_ms =
        writer_telemetry.capture_sharpening_gpu_p99_ms;
    pipeline_report.capture_sharpening_gpu_min_ms =
        writer_telemetry.capture_sharpening_gpu_min_ms;
    pipeline_report.capture_sharpening_gpu_max_ms =
        writer_telemetry.capture_sharpening_gpu_max_ms;
    pipeline_report.davinci_intermediate_gpu_timestamp_samples =
        writer_telemetry.davinci_intermediate_gpu_timestamp_samples;
    pipeline_report.davinci_intermediate_gpu_total_ms =
        writer_telemetry.davinci_intermediate_gpu_total_ms;
    pipeline_report.davinci_intermediate_gpu_mean_ms =
        writer_telemetry.davinci_intermediate_gpu_mean_ms;
    pipeline_report.davinci_intermediate_gpu_p50_ms =
        writer_telemetry.davinci_intermediate_gpu_p50_ms;
    pipeline_report.davinci_intermediate_gpu_p95_ms =
        writer_telemetry.davinci_intermediate_gpu_p95_ms;
    pipeline_report.davinci_intermediate_gpu_p99_ms =
        writer_telemetry.davinci_intermediate_gpu_p99_ms;
    pipeline_report.davinci_intermediate_gpu_min_ms =
        writer_telemetry.davinci_intermediate_gpu_min_ms;
    pipeline_report.davinci_intermediate_gpu_max_ms =
        writer_telemetry.davinci_intermediate_gpu_max_ms;
    pipeline_report.raw_calibration_gpu_timestamp_samples =
        writer_telemetry.raw_calibration_gpu_timestamp_samples;
    pipeline_report.raw_calibration_gpu_total_ms =
        writer_telemetry.raw_calibration_gpu_total_ms;
    pipeline_report.raw_calibration_gpu_mean_ms =
        writer_telemetry.raw_calibration_gpu_mean_ms;
    pipeline_report.raw_calibration_gpu_p50_ms =
        writer_telemetry.raw_calibration_gpu_p50_ms;
    pipeline_report.raw_calibration_gpu_p95_ms =
        writer_telemetry.raw_calibration_gpu_p95_ms;
    pipeline_report.raw_calibration_gpu_p99_ms =
        writer_telemetry.raw_calibration_gpu_p99_ms;
    pipeline_report.raw_calibration_gpu_min_ms =
        writer_telemetry.raw_calibration_gpu_min_ms;
    pipeline_report.raw_calibration_gpu_max_ms =
        writer_telemetry.raw_calibration_gpu_max_ms;
    pipeline_report.rcd_demosaic_gpu_timestamp_samples =
        writer_telemetry.rcd_demosaic_gpu_timestamp_samples;
    pipeline_report.rcd_demosaic_gpu_total_ms =
        writer_telemetry.rcd_demosaic_gpu_total_ms;
    pipeline_report.rcd_demosaic_gpu_mean_ms =
        writer_telemetry.rcd_demosaic_gpu_mean_ms;
    pipeline_report.rcd_demosaic_gpu_p50_ms =
        writer_telemetry.rcd_demosaic_gpu_p50_ms;
    pipeline_report.rcd_demosaic_gpu_p95_ms =
        writer_telemetry.rcd_demosaic_gpu_p95_ms;
    pipeline_report.rcd_demosaic_gpu_p99_ms =
        writer_telemetry.rcd_demosaic_gpu_p99_ms;
    pipeline_report.rcd_demosaic_gpu_min_ms =
        writer_telemetry.rcd_demosaic_gpu_min_ms;
    pipeline_report.rcd_demosaic_gpu_max_ms =
        writer_telemetry.rcd_demosaic_gpu_max_ms;
    pipeline_report.control_status_read_bytes =
        writer_telemetry.control_status_read_bytes;
    pipeline_report.control_status_failures =
        writer_telemetry.control_status_failures;
    pipeline_report.job_queue_backpressure_waits =
        writer_telemetry.job_queue_backpressure_waits;
    pipeline_report.job_queue_backpressure_wait_ms =
        writer_telemetry.job_queue_backpressure_wait_ms;
    pipeline_report.packet_queue_backpressure_waits =
        writer_telemetry.packet_queue_backpressure_waits;
    pipeline_report.packet_queue_backpressure_wait_ms =
        writer_telemetry.packet_queue_backpressure_wait_ms;
    pipeline_report.slot_backpressure_waits =
        writer_telemetry.slot_backpressure_waits;
    pipeline_report.slot_backpressure_wait_ms =
        writer_telemetry.slot_backpressure_wait_ms;
    pipeline_report.job_queue_latency_samples =
        writer_telemetry.job_queue_latency_samples;
    pipeline_report.job_queue_latency_total_ms =
        writer_telemetry.job_queue_latency_total_ms;
    pipeline_report.job_queue_latency_mean_ms =
        writer_telemetry.job_queue_latency_mean_ms;
    pipeline_report.job_queue_latency_max_ms =
        writer_telemetry.job_queue_latency_max_ms;
    pipeline_report.frame_pack_samples = writer_telemetry.frame_pack_samples;
    pipeline_report.frame_pack_total_ms = writer_telemetry.frame_pack_total_ms;
    pipeline_report.frame_pack_mean_ms = writer_telemetry.frame_pack_mean_ms;
    pipeline_report.frame_pack_max_ms = writer_telemetry.frame_pack_max_ms;
    pipeline_report.encoder_send_samples = writer_telemetry.encoder_send_samples;
    pipeline_report.encoder_send_total_ms = writer_telemetry.encoder_send_total_ms;
    pipeline_report.encoder_send_mean_ms = writer_telemetry.encoder_send_mean_ms;
    pipeline_report.encoder_send_max_ms = writer_telemetry.encoder_send_max_ms;
    pipeline_report.encoder_receive_samples = writer_telemetry.encoder_receive_samples;
    pipeline_report.encoder_receive_total_ms = writer_telemetry.encoder_receive_total_ms;
    pipeline_report.encoder_receive_mean_ms = writer_telemetry.encoder_receive_mean_ms;
    pipeline_report.encoder_receive_max_ms = writer_telemetry.encoder_receive_max_ms;
    pipeline_report.frame_allocation_samples =
        writer_telemetry.frame_allocation_samples;
    pipeline_report.frame_allocation_total_ms =
        writer_telemetry.frame_allocation_total_ms;
    pipeline_report.frame_allocation_mean_ms =
        writer_telemetry.frame_allocation_mean_ms;
    pipeline_report.frame_allocation_max_ms =
        writer_telemetry.frame_allocation_max_ms;
    pipeline_report.queue_lock_wait_samples =
        writer_telemetry.queue_lock_wait_samples;
    pipeline_report.queue_lock_wait_total_ms =
        writer_telemetry.queue_lock_wait_total_ms;
    pipeline_report.queue_lock_wait_mean_ms =
        writer_telemetry.queue_lock_wait_mean_ms;
    pipeline_report.queue_lock_wait_max_ms =
        writer_telemetry.queue_lock_wait_max_ms;
    pipeline_report.queue_submit_samples = writer_telemetry.queue_submit_samples;
    pipeline_report.queue_submit_total_ms = writer_telemetry.queue_submit_total_ms;
    pipeline_report.queue_submit_mean_ms = writer_telemetry.queue_submit_mean_ms;
    pipeline_report.queue_submit_max_ms = writer_telemetry.queue_submit_max_ms;
    pipeline_report.resident_slot_count = writer_telemetry.resident_slot_count;
    pipeline_report.prepared_frame_queue_capacity =
        writer_telemetry.prepared_frame_queue_capacity;
    pipeline_report.prepared_frame_queue_max_depth =
        writer_telemetry.prepared_frame_queue_max_depth;
    pipeline_report.effective_async_depth = writer_telemetry.effective_async_depth;
    pipeline_report.compute_pool_size = writer_telemetry.compute_pool_size;
    pipeline_report.compute_queue_family = writer_telemetry.compute_queue_family;
    pipeline_report.compute_queue_index = writer_telemetry.compute_queue_index;
    const auto process_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - process_start).count();
    timings.add("process_wall", process_wall_ms);
    mcraw::write_sidecar(sidecar, input, output, config, first_metadata, first_solution,
                         timings, frame_limit, sync_report, pipeline_report,
                         worker_telemetry, warnings);
    std::cout << nlohmann::json{{"ok", true}, {"output", output.string()},
                                {"sidecar", sidecar.string()}, {"frames", frame_limit},
                                {"wall_ms", process_wall_ms},
                                {"throughput_fps", static_cast<double>(frame_limit) *
                                    1000.0 / process_wall_ms},
                                {"pipeline", {
                                    {"backend", pipeline_report.backend},
                                    {"entry", pipeline_report.pipeline_entry},
                                    {"precision", pipeline_report.pipeline_precision},
                                    {"demosaic_location", pipeline_report.demosaic_location},
                                    {"color_solution_location", pipeline_report.color_solution_location},
                                    {"performance_mode", pipeline_report.performance_mode},
                                    {"intermediate_storage", pipeline_report.intermediate_storage},
                                    {"di_implementation", pipeline_report.di_implementation},
                                    {"dither_mode", pipeline_report.dither_mode},
                                    {"demosaic_implementation", pipeline_report.demosaic_implementation},
                                    {"requested_backend", pipeline_report.requested_backend},
                                    {"async_depth", pipeline_report.async_depth},
                                    {"used_fallback", pipeline_report.used_fallback},
                                    {"fallback_reason", pipeline_report.fallback_reason},
                                    {"gpu_resident", pipeline_report.gpu_resident},
                                    {"upload_frames", pipeline_report.upload_frames},
                                    {"readback_frames", pipeline_report.readback_frames},
                                    {"direct_frames", pipeline_report.direct_frames},
                                    {"rgb_upload_bytes", pipeline_report.rgb_upload_bytes},
                                    {"transfers", {
                                        {"compressed_input_upload_bytes", pipeline_report.compressed_input_upload_bytes},
                                        {"u16_raw_upload_bytes", pipeline_report.u16_raw_upload_bytes},
                                        {"fp16_rgb_upload_bytes", pipeline_report.fp16_rgb_upload_bytes},
                                        {"fp32_rgb_upload_bytes", pipeline_report.fp32_rgb_upload_bytes},
                                        {"target_log_fp32_upload_bytes", pipeline_report.target_log_fp32_upload_bytes},
                                        {"camera_rgb_fp32_upload_bytes", pipeline_report.camera_rgb_fp32_upload_bytes},
                                        {"control_status_read_bytes", pipeline_report.control_status_read_bytes},
                                        {"compressed_packet_download_bytes", pipeline_report.compressed_packet_download_bytes},
                                        {"gpu_image_to_image_counted_as_pcie", false}
                                    }},
                                    {"video_packets", pipeline_report.video_packets},
                                    {"gpu_queue_max_depth", pipeline_report.gpu_queue_max_depth},
                                    {"packet_queue_max_depth", pipeline_report.packet_queue_max_depth},
                                    {"backpressure_waits", pipeline_report.backpressure_waits},
                                    {"backpressure_wait_ms", pipeline_report.backpressure_wait_ms},
                                    {"job_queue_backpressure_waits", pipeline_report.job_queue_backpressure_waits},
                                    {"job_queue_backpressure_wait_ms", pipeline_report.job_queue_backpressure_wait_ms},
                                    {"slot_backpressure_waits", pipeline_report.slot_backpressure_waits},
                                    {"slot_backpressure_wait_ms", pipeline_report.slot_backpressure_wait_ms},
                                    {"resident_slot_count", pipeline_report.resident_slot_count},
                                    {"prepared_frame_queue_max_depth", pipeline_report.prepared_frame_queue_max_depth},
                                    {"frame_pack_mean_ms", pipeline_report.frame_pack_mean_ms},
                                    {"encoder_send_mean_ms", pipeline_report.encoder_send_mean_ms},
                                    {"encoder_receive_mean_ms", pipeline_report.encoder_receive_mean_ms},
                                    {"mux_megabytes_per_second", pipeline_report.mux_megabytes_per_second},
                                    {"gpu_name", pipeline_report.gpu_name},
                                    {"gpu_uuid", pipeline_report.gpu_uuid},
                                    {"gpu_timestamps_supported", pipeline_report.gpu_timestamps_supported},
                                    {"control_status_failures", pipeline_report.control_status_failures},
                                    {"gpu_stages", {
                                        {"raw_calibration", {
                                            {"samples", pipeline_report.raw_calibration_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.raw_calibration_gpu_total_ms},
                                            {"mean_ms", pipeline_report.raw_calibration_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.raw_calibration_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.raw_calibration_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.raw_calibration_gpu_p99_ms},
                                            {"min_ms", pipeline_report.raw_calibration_gpu_min_ms},
                                            {"max_ms", pipeline_report.raw_calibration_gpu_max_ms}
                                        }},
                                        {"rcd_demosaic", {
                                            {"samples", pipeline_report.rcd_demosaic_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.rcd_demosaic_gpu_total_ms},
                                            {"mean_ms", pipeline_report.rcd_demosaic_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.rcd_demosaic_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.rcd_demosaic_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.rcd_demosaic_gpu_p99_ms},
                                            {"min_ms", pipeline_report.rcd_demosaic_gpu_min_ms},
                                            {"max_ms", pipeline_report.rcd_demosaic_gpu_max_ms}
                                        }},
                                        {"camera_to_dwg", {
                                            {"samples", pipeline_report.camera_to_dwg_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.camera_to_dwg_gpu_total_ms},
                                            {"mean_ms", pipeline_report.camera_to_dwg_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.camera_to_dwg_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.camera_to_dwg_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.camera_to_dwg_gpu_p99_ms},
                                            {"min_ms", pipeline_report.camera_to_dwg_gpu_min_ms},
                                            {"max_ms", pipeline_report.camera_to_dwg_gpu_max_ms}
                                        }},
                                        {"capture_sharpening", {
                                            {"samples", pipeline_report.capture_sharpening_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.capture_sharpening_gpu_total_ms},
                                            {"mean_ms", pipeline_report.capture_sharpening_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.capture_sharpening_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.capture_sharpening_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.capture_sharpening_gpu_p99_ms},
                                            {"min_ms", pipeline_report.capture_sharpening_gpu_min_ms},
                                            {"max_ms", pipeline_report.capture_sharpening_gpu_max_ms}
                                        }},
                                        {"davinci_intermediate", {
                                            {"samples", pipeline_report.davinci_intermediate_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.davinci_intermediate_gpu_total_ms},
                                            {"mean_ms", pipeline_report.davinci_intermediate_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.davinci_intermediate_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.davinci_intermediate_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.davinci_intermediate_gpu_p99_ms},
                                            {"min_ms", pipeline_report.davinci_intermediate_gpu_min_ms},
                                            {"max_ms", pipeline_report.davinci_intermediate_gpu_max_ms}
                                        }},
                                        {"rgb_to_yuv_422", {
                                            {"samples", pipeline_report.rgb_to_yuv_gpu_timestamp_samples},
                                            {"total_ms", pipeline_report.rgb_to_yuv_gpu_total_ms},
                                            {"mean_ms", pipeline_report.rgb_to_yuv_gpu_mean_ms},
                                            {"p50_ms", pipeline_report.rgb_to_yuv_gpu_p50_ms},
                                            {"p95_ms", pipeline_report.rgb_to_yuv_gpu_p95_ms},
                                            {"p99_ms", pipeline_report.rgb_to_yuv_gpu_p99_ms},
                                            {"min_ms", pipeline_report.rgb_to_yuv_gpu_min_ms},
                                            {"max_ms", pipeline_report.rgb_to_yuv_gpu_max_ms}
                                        }}
                                    }}
                                }},
                                {"execution", {
                                    {"cpu_threads", execution.total_threads},
                                    {"parallel_frames", execution.parallel_frames},
                                    {"threads_per_frame", execution.threads_per_frame},
                                    {"encode_contexts", execution.encode_contexts},
                                    {"encode_threads_per_context", execution.encode_threads_per_context},
                                    {"worker_queue_capacity", worker_telemetry.queue_capacity},
                                    {"worker_queue_max_depth", worker_telemetry.max_queue_depth},
                                    {"worker_submit_waits", worker_telemetry.submit_waits},
                                    {"worker_submit_wait_ms", worker_telemetry.submit_wait_ms},
                                    {"worker_tasks_started", worker_telemetry.tasks_started},
                                    {"worker_tasks_completed", worker_telemetry.tasks_completed},
                                    {"worker_tasks_cancelled", worker_telemetry.tasks_cancelled}
                                }},
                                {"timings", timings.to_json()}}.dump(2) << '\n';
    return 0;
#endif
}

void print_help() {
    std::cout <<
        "mcraw-transcoder 0.1.0\n"
        "Usage:\n"
        "  mcraw-transcoder inspect <input.mcraw> [--raw-json]\n"
        "  mcraw-transcoder convert <input.mcraw> <output.mov> [--config file.json] [--frames N] [--overwrite] [--validation] [--verify-output]\n"
        "  mcraw-transcoder extract-frame <input.mcraw> --frame N --stage STAGE --output PATH\n"
        "  mcraw-transcoder validate <input.mcraw> [--frame N] [--compare-fused] [--config file.json]\n"
        "  mcraw-transcoder benchmark <input.mcraw> [--frames N] [--config file.json]\n"
        "  mcraw-transcoder print-effective-config [--config file.json]\n"
        "  mcraw-transcoder list-capabilities\n"
        "  mcraw-transcoder vulkan-smoke [--gpu SELECTOR] [--iterations N] [--validation]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Arguments args(argc, argv);
        if (argc < 2 || args.flag("--help") || args.flag("-h")) {
            print_help();
            return argc < 2 ? 2 : 0;
        }
        const auto command = args.at(1);
        if (command == "inspect") return command_inspect(args);
        if (command == "convert") return command_convert(args);
        if (command == "extract-frame") return command_extract(args);
        if (command == "validate") return command_validate(args);
        if (command == "benchmark") return command_benchmark(args);
        if (command == "print-effective-config") return command_print_effective_config(args);
        if (command == "list-capabilities") return command_list_capabilities();
        if (command == "vulkan-smoke") return command_vulkan_smoke(args);
        throw Error(ErrorCode::invalid_argument, "unknown command: " + std::string(command));
    } catch (const Error& error) {
        std::cerr << "error[" << mcraw::error_code_name(error.code()) << "]: "
                  << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
