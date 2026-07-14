#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
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
