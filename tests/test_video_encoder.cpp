#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include <mcraw/core/error.hpp>
#include <mcraw/output/cpu_prores_encoder.hpp>
#include <mcraw/output/ffmpeg_raii.hpp>
#include <mcraw/output/vulkan_prores_encoder_stub.hpp>

namespace {

mcraw::VideoFrame make_neutral_frame(int width, int height, std::int64_t pts) {
    auto frame = mcraw::make_av_frame();
    frame->format = AV_PIX_FMT_YUV422P10LE;
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    frame->duration = 3'000;
    mcraw::require_ffmpeg(av_frame_get_buffer(frame.get(), 32), "allocate test frame");
    mcraw::require_ffmpeg(av_frame_make_writable(frame.get()), "make test frame writable");
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(frame->data[0] + y * frame->linesize[0]);
        std::fill_n(row, width, static_cast<std::uint16_t>(512));
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<std::uint16_t*>(
                frame->data[plane] + y * frame->linesize[plane]);
            std::fill_n(row, width / 2, static_cast<std::uint16_t>(512));
        }
    }
    mcraw::FrameMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.pts = pts;
    metadata.duration = frame->duration;
    metadata.time_base = {1, 90'000};
    metadata.range = AVCOL_RANGE_MPEG;
    metadata.matrix = AVCOL_SPC_BT2020_NCL;
    return mcraw::CpuVideoFrame{metadata, AV_PIX_FMT_YUV422P10LE, std::move(frame)};
}

} // namespace

TEST_CASE("CPU ProRes adapter accepts owned frames and drains packets") {
    mcraw::CpuProResEncoder encoder({64, 32});
    encoder.send(make_neutral_frame(64, 32, 0));
    auto packets = encoder.drain();
    auto tail = encoder.flush();
    packets.insert(packets.end(), std::make_move_iterator(tail.begin()),
                   std::make_move_iterator(tail.end()));

    REQUIRE(packets.size() == 1);
    CHECK(packets.front().packet->size > 0);
    CHECK(packets.front().time_base.num == 1);
    CHECK(packets.front().time_base.den == 90'000);
}

TEST_CASE("CPU ProRes adapter opens every FFmpeg profile") {
    for (const auto* profile : {"proxy", "lt", "standard", "hq", "4444", "4444xq"}) {
        mcraw::CpuProResEncoder encoder({64, 32, {1, 90'000}, {30, 1}, profile, 1});
        encoder.send(make_neutral_frame(64, 32, 0));
        auto packets = encoder.drain();
        auto tail = encoder.flush();
        packets.insert(packets.end(), std::make_move_iterator(tail.begin()),
                       std::make_move_iterator(tail.end()));
        INFO("profile=" << profile);
        CHECK(packets.size() == 1);
    }
}

TEST_CASE("Vulkan encoder stub fails explicitly") {
    mcraw::VulkanProResEncoderStub encoder("runtime not implemented");
    const auto capabilities = encoder.capabilities();

    CHECK_FALSE(capabilities.available);
    CHECK(capabilities.input_storage == mcraw::FrameStorage::vulkan);
    CHECK_THROWS_AS(encoder.flush(), mcraw::Error);
}
