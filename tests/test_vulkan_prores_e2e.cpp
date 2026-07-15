#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <mcraw/output/ffmpeg_raii.hpp>
#include <mcraw/output/ffmpeg_writer.hpp>
#include <mcraw/core/error.hpp>

namespace {

struct TemporaryMov {
    std::filesystem::path path;
    ~TemporaryMov() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

mcraw::Yuv422P10 test_frame(std::uint32_t width,
                            std::uint32_t height,
                            std::uint16_t luma) {
    mcraw::Yuv422P10 frame;
    frame.width = width;
    frame.height = height;
    frame.y.assign(static_cast<std::size_t>(width) * height, luma);
    frame.cb.assign(static_cast<std::size_t>(width / 2U) * height, 512U);
    frame.cr.assign(static_cast<std::size_t>(width / 2U) * height, 512U);
    return frame;
}

mcraw::TargetLogRgbF32 test_rgb_frame(std::uint32_t width,
                                      std::uint32_t height,
                                      int frame_index) {
    mcraw::TargetLogRgbF32 frame;
    frame.width = width;
    frame.height = height;
    for (auto& plane : frame.planes) {
        plane.resize(static_cast<std::size_t>(width) * height);
    }
    for (std::size_t pixel = 0; pixel < frame.planes[0].size(); ++pixel) {
        const float gradient = static_cast<float>((pixel + frame_index) % width) /
                               static_cast<float>(width - 1U);
        const float evolution = static_cast<float>(frame_index % 300) / 299.0F;
        frame.planes[0][pixel] = 0.75F * gradient + 0.25F * evolution;
        frame.planes[1][pixel] = 0.5F * gradient + 0.3F * evolution;
        frame.planes[2][pixel] = 0.8F * (1.0F - gradient) + 0.2F * evolution;
    }
    return frame;
}

mcraw::VulkanRawMosaicInput test_raw_frame(std::uint32_t width,
                                           std::uint32_t height,
                                           int frame_index) {
    mcraw::VulkanRawMosaicInput input;
    input.image = {width, height, mcraw::CfaPattern::rggb, {}};
    input.image.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto gradient = static_cast<std::uint16_t>(
                256U + ((x * 31U + y * 17U + static_cast<std::uint32_t>(frame_index) * 13U) %
                         3000U));
            input.image.pixels[static_cast<std::size_t>(y) * width + x] = gradient;
        }
    }
    input.metadata.width = width;
    input.metadata.height = height;
    input.metadata.cfa = input.image.cfa;
    input.metadata.black_level = {64.0, 64.0, 64.0, 64.0};
    input.metadata.white_level = {4095.0, 4095.0, 4095.0, 4095.0};
    input.metadata.compression_type = 7;
    input.camera_to_target = mcraw::Matrix3d::identity();
    input.capture_sharpening = 0.4;
    input.capture_sharpening_threshold = 0.002;
    return input;
}

int decode_video_frames(const std::filesystem::path& path) {
    AVFormatContext* input = nullptr;
    const auto path_string = path.string();
    mcraw::require_ffmpeg(avformat_open_input(&input, path_string.c_str(), nullptr, nullptr),
                          "open generated Vulkan ProRes MOV");
    struct FormatCloser {
        AVFormatContext*& value;
        ~FormatCloser() { avformat_close_input(&value); }
    } closer{input};
    mcraw::require_ffmpeg(avformat_find_stream_info(input, nullptr),
                          "read generated MOV stream info");
    const int stream_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1,
                                                 nullptr, 0);
    mcraw::require_ffmpeg(stream_index, "find generated MOV video stream");
    const auto* parameters = input->streams[stream_index]->codecpar;
    REQUIRE(parameters->codec_id == AV_CODEC_ID_PRORES);
    CHECK(parameters->codec_tag == MKTAG('a', 'p', 'c', 'h'));
    CHECK(parameters->color_range == AVCOL_RANGE_MPEG);
    CHECK(parameters->color_space == AVCOL_SPC_BT2020_NCL);
    CHECK(parameters->color_primaries == AVCOL_PRI_UNSPECIFIED);
    CHECK(parameters->color_trc == AVCOL_TRC_UNSPECIFIED);

    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
    REQUIRE(decoder != nullptr);
    mcraw::AvCodecContextPtr context(avcodec_alloc_context3(decoder));
    REQUIRE(context != nullptr);
    mcraw::require_ffmpeg(avcodec_parameters_to_context(context.get(), parameters),
                          "copy generated ProRes parameters");
    mcraw::require_ffmpeg(avcodec_open2(context.get(), decoder, nullptr),
                          "open generated ProRes decoder");
    auto packet = mcraw::make_av_packet();
    auto frame = mcraw::make_av_frame();
    int decoded = 0;
    const auto receive = [&] {
        while (true) {
            const int result = avcodec_receive_frame(context.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            mcraw::require_ffmpeg(result, "decode generated Vulkan ProRes frame");
            CHECK(frame->width == 64);
            CHECK(frame->height == 32);
            ++decoded;
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(input, packet.get()) >= 0) {
        if (packet->stream_index == stream_index) {
            mcraw::require_ffmpeg(avcodec_send_packet(context.get(), packet.get()),
                                  "send generated ProRes packet");
            receive();
        }
        av_packet_unref(packet.get());
    }
    mcraw::require_ffmpeg(avcodec_send_packet(context.get(), nullptr),
                          "flush generated ProRes decoder");
    receive();
    return decoded;
}

std::size_t stress_iterations() {
    const char* value = std::getenv("MCRAW_VULKAN_STRESS_ITERATIONS");
    if (value == nullptr) return 1U;
    const auto parsed = std::stoul(value);
    return std::clamp<std::size_t>(parsed, 1U, 100U);
}

std::uint64_t private_memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != 0) {
        return counters.PrivateUsage;
    }
#endif
    return 0U;
}

} // namespace

TEST_CASE("Vulkan upload bridge writes a decodable ProRes HQ MOV") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    // 300 frames at 30 fps exercises a complete ten-second synthetic clip,
    // including the encoder's delayed tail and MOV trailer.
    constexpr int frame_count = 300;
    const auto iterations = stress_iterations();
    std::uint64_t warmed_private_bytes = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        CAPTURE(iteration, iterations);
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        TemporaryMov output{std::filesystem::temp_directory_path() /
                            ("mcraw-vulkan-e2e-" + std::to_string(unique) + ".mov")};

        mcraw::FfmpegWriterTelemetry telemetry;
        {
            mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                                       {}, {mcraw::VideoBackend::vulkan, "auto", 8, false});
            for (int index = 0; index < frame_count; ++index) {
                writer.write_video(test_frame(
                                       width, height,
                                       static_cast<std::uint16_t>(64 + (index % 8) * 100)),
                                   1'000'000'000LL + index * 33'333'333LL);
            }
            writer.finish();
            telemetry = writer.telemetry();
        }
        CHECK(telemetry.backend == "prores_ks_vulkan");
        CHECK_FALSE(telemetry.gpu_resident);
        CHECK(telemetry.upload_frames == frame_count);
        CHECK(telemetry.readback_frames == 0U);
        CHECK(telemetry.video_packets == frame_count);
        CHECK_FALSE(telemetry.gpu_uuid.empty());
        mcraw::validate_prores_mov(output.path, frame_count);
        CHECK(decode_video_frames(output.path) == frame_count);
        if (iteration == std::min<std::size_t>(4U, iterations - 1U)) {
            warmed_private_bytes = private_memory_bytes();
        }
    }
    const auto final_private_bytes = private_memory_bytes();
    if (iterations > 5U && warmed_private_bytes != 0U && final_private_bytes != 0U) {
        INFO("warmed private bytes=" << warmed_private_bytes
             << ", final private bytes=" << final_private_bytes);
        CHECK(final_private_bytes <= warmed_private_bytes + 128ULL * 1024ULL * 1024ULL);
    }
}

TEST_CASE("Bounded Vulkan RGB pipeline writes a GPU-resident decodable MOV") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr int frame_count = 300;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-direct-e2e-" + std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriterTelemetry telemetry;
    {
        mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                                   {}, {mcraw::VideoBackend::vulkan, "auto", 8, false});
        for (int index = 0; index < frame_count; ++index) {
            writer.write_video(test_rgb_frame(width, height, index),
                               1'000'000'000LL + index * 33'333'333LL,
                               static_cast<std::size_t>(index));
        }
        writer.finish();
        telemetry = writer.telemetry();
    }
    CHECK(telemetry.backend == "prores_ks_vulkan");
    CHECK(telemetry.gpu_resident);
    CHECK(telemetry.direct_frames == frame_count);
    CHECK(telemetry.upload_frames == 0U);
    CHECK(telemetry.readback_frames == 0U);
    CHECK(telemetry.rgb_upload_bytes ==
          static_cast<std::uint64_t>(width) * height * 3U * sizeof(float) * frame_count);
    CHECK(telemetry.compressed_input_upload_bytes == 0U);
    CHECK(telemetry.u16_raw_upload_bytes == 0U);
    CHECK(telemetry.fp16_rgb_upload_bytes == 0U);
    CHECK(telemetry.fp32_rgb_upload_bytes == telemetry.rgb_upload_bytes);
    CHECK(telemetry.pipeline_entry == "target_log_f32");
    CHECK(telemetry.pipeline_precision == "fp32/precise");
    CHECK(telemetry.demosaic_location == "cpu");
    CHECK(telemetry.color_solution_location == "cpu_fp64");
    CHECK(telemetry.target_log_fp32_upload_bytes == telemetry.rgb_upload_bytes);
    CHECK(telemetry.camera_rgb_fp32_upload_bytes == 0U);
    CHECK(telemetry.compressed_packet_download_bytes == telemetry.mux_bytes);
    CHECK(telemetry.video_packets == frame_count);
    CHECK(telemetry.gpu_queue_capacity >= 4U);
    CHECK(telemetry.gpu_queue_max_depth > 0U);
    CHECK(telemetry.gpu_queue_max_depth <= telemetry.gpu_queue_capacity);
    CHECK(telemetry.resident_slot_count == 2U);
    CHECK(telemetry.prepared_frame_queue_capacity == 2U);
    CHECK(telemetry.prepared_frame_queue_max_depth > 0U);
    CHECK(telemetry.prepared_frame_queue_max_depth <=
          telemetry.prepared_frame_queue_capacity);
    CHECK(telemetry.packet_queue_capacity >= 8U);
    CHECK(telemetry.packet_queue_max_depth > 0U);
    CHECK(telemetry.packet_queue_max_depth <= telemetry.packet_queue_capacity);
    CHECK(telemetry.mux_bytes > 0U);
    CHECK(telemetry.mux_megabytes_per_second > 0.0);
    CHECK(telemetry.gpu_timestamps_supported);
    CHECK(telemetry.rgb_to_yuv_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.rgb_to_yuv_gpu_total_ms > 0.0);
    CHECK(telemetry.rgb_to_yuv_gpu_mean_ms > 0.0);
    CHECK(telemetry.rgb_to_yuv_gpu_p95_ms > 0.0);
    CHECK(telemetry.job_queue_latency_samples == frame_count);
    CHECK(telemetry.job_queue_latency_mean_ms >= 0.0);
    CHECK(telemetry.frame_pack_samples == frame_count);
    CHECK(telemetry.frame_pack_mean_ms >= 0.0);
    CHECK(telemetry.encoder_send_samples == frame_count);
    CHECK(telemetry.encoder_send_mean_ms >= 0.0);
    CHECK(telemetry.encoder_receive_samples == frame_count);
    CHECK(telemetry.encoder_receive_mean_ms >= 0.0);
    CHECK(telemetry.frame_allocation_samples == frame_count);
    CHECK(telemetry.frame_allocation_mean_ms >= 0.0);
    CHECK(telemetry.queue_lock_wait_samples == frame_count);
    CHECK(telemetry.queue_lock_wait_mean_ms >= 0.0);
    CHECK(telemetry.queue_submit_samples == frame_count);
    CHECK(telemetry.queue_submit_mean_ms >= 0.0);
    CHECK(telemetry.backpressure_waits ==
          telemetry.job_queue_backpressure_waits +
              telemetry.packet_queue_backpressure_waits +
              telemetry.slot_backpressure_waits);
    mcraw::validate_prores_mov(output.path, frame_count);
    CHECK(decode_video_frames(output.path) == frame_count);
}

TEST_CASE("Vulkan worker failure cancels bounded queues and reaches the caller") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-cancel-" + std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                               {}, {mcraw::VideoBackend::vulkan, "auto", 4, false});
    writer.write_video(test_rgb_frame(width, height, 0), 1'000'000'000LL, 0);
    writer.write_video(test_rgb_frame(width, height, 1), 1'000'000'000LL, 1);
    CHECK_THROWS_AS(writer.finish(), mcraw::Error);
}

TEST_CASE("Vulkan Camera RGB resident chain writes a decodable MOV") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-camera-stage1a-" + std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                               {}, {mcraw::VideoBackend::vulkan, "auto", 4, false});
    mcraw::VulkanCameraRgbInput input;
    input.image = test_rgb_frame(width, height, 0);
    input.camera_to_target = mcraw::Matrix3d::identity();
    input.input_scale = 1.0;
    input.capture_sharpening = 0.4;
    input.capture_sharpening_threshold = 0.002;
    writer.write_video(std::move(input), 1'000'000'000LL, 0);
    writer.finish();
    const auto telemetry = writer.telemetry();
    CHECK(telemetry.pipeline_entry == "camera_rgb_f32");
    CHECK(telemetry.pipeline_precision == "fp32/precise");
    CHECK(telemetry.camera_rgb_fp32_upload_bytes ==
          static_cast<std::uint64_t>(width) * height * 3U * sizeof(float));
    CHECK(telemetry.target_log_fp32_upload_bytes == 0U);
    CHECK(telemetry.camera_to_dwg_gpu_timestamp_samples == 1U);
    CHECK(telemetry.capture_sharpening_gpu_timestamp_samples == 1U);
    CHECK(telemetry.davinci_intermediate_gpu_timestamp_samples == 1U);
    CHECK(telemetry.rgb_to_yuv_gpu_timestamp_samples == 1U);
    CHECK(telemetry.control_status_read_bytes == sizeof(std::uint32_t));
    CHECK(telemetry.control_status_failures == 0U);
    CHECK(telemetry.resident_slot_count == 2U);
    CHECK(telemetry.prepared_frame_queue_capacity == 2U);
    CHECK(telemetry.job_queue_latency_samples == 1U);
    CHECK(telemetry.frame_pack_samples == 1U);
    CHECK(telemetry.encoder_send_samples == 1U);
    CHECK(telemetry.encoder_receive_samples == 1U);
    CHECK(telemetry.frame_allocation_samples == 1U);
    CHECK(telemetry.queue_lock_wait_samples == 1U);
    CHECK(telemetry.queue_submit_samples == 1U);
    mcraw::validate_prores_mov(output.path, 1);
    CHECK(decode_video_frames(output.path) == 1);
}

TEST_CASE("Vulkan U16 RAW resident chain writes a decodable MOV") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr std::size_t frame_count = 3;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-raw-stage2d-" + std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                               {}, {mcraw::VideoBackend::vulkan, "auto", 4, false,
                                    mcraw::ChromaFilter::quality, true,
                                    mcraw::GpuPrecision::fp32,
                                    mcraw::GpuPerformanceMode::balanced});
    for (std::size_t index = 0; index < frame_count; ++index) {
        writer.write_video(test_raw_frame(width, height, static_cast<int>(index)),
                           1'000'000'000LL + static_cast<std::int64_t>(index) * 33'333'333LL,
                           index);
    }
    writer.finish();
    const auto telemetry = writer.telemetry();
    CHECK(telemetry.pipeline_entry == "raw_mosaic_u16");
    CHECK(telemetry.pipeline_precision == "fp32/precise");
    CHECK(telemetry.demosaic_location == "gpu_rcd_precise");
    CHECK(telemetry.color_solution_location == "cpu_fp64");
    CHECK(telemetry.performance_mode == "balanced");
    CHECK(telemetry.intermediate_storage == "fp32");
    CHECK(telemetry.di_implementation == "fp32_lut");
    CHECK(telemetry.dither_mode == "deterministic");
    CHECK(telemetry.demosaic_implementation == "gpu_rcd_precise");
    CHECK(telemetry.u16_raw_upload_bytes ==
          static_cast<std::uint64_t>(width) * height * sizeof(std::uint16_t) * frame_count);
    CHECK(telemetry.rgb_upload_bytes == 0U);
    CHECK(telemetry.fp32_rgb_upload_bytes == 0U);
    CHECK(telemetry.camera_rgb_fp32_upload_bytes == 0U);
    CHECK(telemetry.target_log_fp32_upload_bytes == 0U);
    CHECK(telemetry.readback_frames == 0U);
    CHECK(telemetry.raw_calibration_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.rcd_demosaic_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.camera_to_dwg_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.capture_sharpening_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.davinci_intermediate_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.rgb_to_yuv_gpu_timestamp_samples == frame_count);
    CHECK(telemetry.control_status_read_bytes == sizeof(std::uint32_t) * frame_count);
    CHECK(telemetry.control_status_failures == 0U);
    mcraw::validate_prores_mov(output.path, frame_count);
    CHECK(decode_video_frames(output.path) == static_cast<int>(frame_count));
}

TEST_CASE("Vulkan Camera RGB resident policy failure reaches the caller") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-camera-policy-" +
                         std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                               {}, {mcraw::VideoBackend::vulkan, "auto", 2, false});
    mcraw::VulkanCameraRgbInput input;
    input.image = test_rgb_frame(width, height, 0);
    input.image.planes[0][0] = -0.25F;
    input.camera_to_target = mcraw::Matrix3d::identity();
    input.input_scale = 1.0;
    input.negative_policy = mcraw::NegativePolicy::error;
    writer.write_video(std::move(input), 1'000'000'000LL, 0);
    CHECK_THROWS_AS(writer.finish(), mcraw::Error);
    CHECK(writer.telemetry().control_status_failures == 1U);
}

TEST_CASE("Vulkan Camera RGB resident non-finite input reaches the caller") {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryMov output{std::filesystem::temp_directory_path() /
                        ("mcraw-vulkan-camera-non-finite-" +
                         std::to_string(unique) + ".mov")};
    mcraw::FfmpegWriter writer(output.path, width, height, 1'000'000'000LL, 0, 0,
                               {}, {mcraw::VideoBackend::vulkan, "auto", 2, false});
    mcraw::VulkanCameraRgbInput input;
    input.image = test_rgb_frame(width, height, 0);
    input.image.planes[0][0] = std::numeric_limits<float>::quiet_NaN();
    input.camera_to_target = mcraw::Matrix3d::identity();
    input.input_scale = 1.0;
    writer.write_video(std::move(input), 1'000'000'000LL, 0);
    CHECK_THROWS_AS(writer.finish(), mcraw::Error);
    CHECK(writer.telemetry().control_status_failures == 1U);
}
