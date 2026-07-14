#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include <mcraw/output/ffmpeg_raii.hpp>
#include <mcraw/output/vulkan_prores_encoder.hpp>

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
