#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iterator>
#include <unordered_set>
#include <vector>

#include <mcraw/output/ffmpeg_raii.hpp>
#include <mcraw/output/vulkan_prores_encoder.hpp>
#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>

namespace {

mcraw::VideoFrame make_upload_frame(int width, int height, std::int64_t pts) {
    auto frame = mcraw::make_av_frame();
    frame->format = AV_PIX_FMT_YUV422P10LE;
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    frame->duration = 3'000;
    mcraw::require_ffmpeg(av_frame_get_buffer(frame.get(), 32), "allocate upload bridge frame");
    mcraw::require_ffmpeg(av_frame_make_writable(frame.get()), "make upload bridge frame writable");
    for (int plane = 0; plane < 3; ++plane) {
        const int plane_width = plane == 0 ? width : width / 2;
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<std::uint16_t*>(
                frame->data[plane] + y * frame->linesize[plane]);
            std::fill_n(row, plane_width, static_cast<std::uint16_t>(
                plane == 0 ? 64 + ((pts / 3'000) % 800) : 512));
        }
    }
    mcraw::FrameMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.pts = pts;
    metadata.duration = 3'000;
    metadata.time_base = {1, 90'000};
    metadata.range = AVCOL_RANGE_MPEG;
    metadata.matrix = AVCOL_SPC_BT2020_NCL;
    return mcraw::CpuVideoFrame{metadata, AV_PIX_FMT_YUV422P10LE, std::move(frame)};
}

struct DecodeResult {
    std::size_t frames{};
    std::size_t unique_luma_hashes{};
};

int direct_stress_frames() {
    const char* value = std::getenv("MCRAW_VULKAN_DIRECT_STRESS_FRAMES");
    if (value == nullptr) return 300;
    return std::clamp(std::stoi(value), 300, 30'000);
}

DecodeResult decode_packets(const std::vector<mcraw::EncodedPacket>& packets,
                            int width,
                            int height) {
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_PRORES);
    REQUIRE(decoder != nullptr);
    mcraw::AvCodecContextPtr context(avcodec_alloc_context3(decoder));
    REQUIRE(context != nullptr);
    context->width = width;
    context->height = height;
    mcraw::require_ffmpeg(avcodec_open2(context.get(), decoder, nullptr),
                          "open direct-frame ProRes decoder");
    auto frame = mcraw::make_av_frame();
    DecodeResult result;
    std::unordered_set<std::uint64_t> hashes;
    std::int64_t previous_pts = AV_NOPTS_VALUE;
    const auto receive = [&] {
        while (true) {
            const int status = avcodec_receive_frame(context.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            mcraw::require_ffmpeg(status, "decode direct-frame ProRes packet");
            REQUIRE(frame->format == AV_PIX_FMT_YUV422P10LE);
            REQUIRE(frame->width == width);
            REQUIRE(frame->height == height);
            if (previous_pts != AV_NOPTS_VALUE) CHECK(frame->pts > previous_pts);
            previous_pts = frame->pts;
            std::uint64_t hash = 1469598103934665603ULL;
            for (int row = 0; row < height; ++row) {
                const auto* values = reinterpret_cast<const std::uint16_t*>(
                    frame->data[0] + row * frame->linesize[0]);
                for (int column = 0; column < width; ++column) {
                    hash ^= values[column];
                    hash *= 1099511628211ULL;
                }
            }
            hashes.insert(hash);
            ++result.frames;
            av_frame_unref(frame.get());
        }
    };
    for (const auto& packet : packets) {
        mcraw::require_ffmpeg(avcodec_send_packet(context.get(), packet.packet.get()),
                              "send direct-frame ProRes packet to decoder");
        receive();
    }
    mcraw::require_ffmpeg(avcodec_send_packet(context.get(), nullptr),
                          "flush direct-frame ProRes decoder");
    receive();
    result.unique_luma_hashes = hashes.size();
    return result;
}

} // namespace

TEST_CASE("Vulkan ProRes upload bridge drains delayed packets and flushes") {
    constexpr int width = 64;
    constexpr int height = 32;
    constexpr int frame_count = 4;
    for (const std::size_t async_depth : {1U, 2U, 4U, 8U}) {
        CAPTURE(async_depth);
        mcraw::VulkanRuntime runtime;
        mcraw::FfmpegVulkanFrameContext frames(runtime, {width, height, 10});
        mcraw::VulkanProResEncoder encoder(
            frames, {width, height, {1, 90'000}, {30, 1}, "hq", async_depth});
        std::vector<mcraw::EncodedPacket> packets;

        for (int frame = 0; frame < frame_count; ++frame) {
            encoder.send(make_upload_frame(width, height, frame * 3'000));
            auto available = encoder.drain();
            std::move(available.begin(), available.end(), std::back_inserter(packets));
        }
        auto tail = encoder.flush();
        std::move(tail.begin(), tail.end(), std::back_inserter(packets));

        REQUIRE(packets.size() == frame_count);
        for (std::size_t index = 0; index < packets.size(); ++index) {
            CHECK(packets[index].packet->size > 0);
            CHECK(packets[index].packet->pts == static_cast<std::int64_t>(index) * 3'000);
        }
        const auto telemetry = encoder.telemetry();
        CHECK_FALSE(telemetry.gpu_resident);
        CHECK(telemetry.upload_frames == frame_count);
        CHECK(telemetry.readback_frames == 0U);
        CHECK(telemetry.packets == frame_count);
    }
}

TEST_CASE("Vulkan ProRes consumes shader-written pool frames without YUV transfer") {
    constexpr int width = 64;
    constexpr int height = 32;
    // Ten seconds exercises pool reuse, timeline-value increments, delayed
    // encoder packets, and software decoding of the resulting bitstream. The
    // environment override extends the same one-process sequence to 1,000 s.
    const int frame_count = direct_stress_frames();
    mcraw::TargetLogRgbF32 input;
    input.width = width;
    input.height = height;
    for (auto& plane : input.planes) {
        plane.resize(static_cast<std::size_t>(width) * height);
    }
    const bool validation = std::getenv("MCRAW_VULKAN_SHADER_VALIDATION") != nullptr;
    mcraw::VulkanRuntime runtime({"auto", validation});
    mcraw::FfmpegVulkanFrameContext frames(runtime, {width, height, 10});
    mcraw::VulkanRgbToYuvFrameWriter frame_writer(
        runtime, frames, {width, height, mcraw::ChromaFilter::quality, true,
                          mcraw::GpuPrecision::fp32, 8});
    mcraw::VulkanProResEncoder encoder(
        frames, {width, height, {1, 90'000}, {30, 1}, "hq", 8});
    std::vector<mcraw::EncodedPacket> packets;
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        for (std::size_t pixel = 0; pixel < input.planes[0].size(); ++pixel) {
            const float gradient = static_cast<float>((pixel + frame_index) % width) /
                                   static_cast<float>(width - 1);
            const float evolution = static_cast<float>(frame_index) /
                                    static_cast<float>(frame_count - 1);
            input.planes[0][pixel] = 0.75F * gradient + 0.25F * evolution;
            input.planes[1][pixel] = 0.5F * gradient + 0.3F * evolution;
            input.planes[2][pixel] = 0.8F * (1.0F - gradient) + 0.2F * evolution;
        }
        mcraw::FrameMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.pts = frame_index * 3'000;
        metadata.duration = 3'000;
        metadata.time_base = {1, 90'000};
        metadata.range = AVCOL_RANGE_MPEG;
        metadata.matrix = AVCOL_SPC_BT2020_NCL;
        encoder.send(frame_writer.pack(input, frame_index, metadata));
        auto available = encoder.drain();
        std::move(available.begin(), available.end(), std::back_inserter(packets));
    }
    auto tail = encoder.flush();
    std::move(tail.begin(), tail.end(), std::back_inserter(packets));
    frame_writer.wait();
    REQUIRE(packets.size() == frame_count);
    for (std::size_t index = 0; index < packets.size(); ++index) {
        CHECK(packets[index].packet->size > 0);
        CHECK(packets[index].packet->pts == static_cast<std::int64_t>(index) * 3'000);
    }
    const auto decoded = decode_packets(packets, width, height);
    CHECK(decoded.frames == frame_count);
    CHECK(decoded.unique_luma_hashes >= 290U);
    const auto writer_telemetry = frame_writer.telemetry();
    CHECK(writer_telemetry.output_frames == frame_count);
    CHECK(writer_telemetry.yuv_upload_frames == 0U);
    CHECK(writer_telemetry.yuv_readback_frames == 0U);
    CHECK(writer_telemetry.slot_count == 8U);
    CHECK(writer_telemetry.in_flight == 0U);
    CHECK(writer_telemetry.max_in_flight == 8U);
    const auto encoder_telemetry = encoder.telemetry();
    CHECK(encoder_telemetry.gpu_resident);
    CHECK(encoder_telemetry.direct_frames == frame_count);
    CHECK(encoder_telemetry.upload_frames == 0U);
    CHECK(encoder_telemetry.readback_frames == 0U);
}
