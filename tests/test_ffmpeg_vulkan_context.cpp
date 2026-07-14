#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/output/ffmpeg_raii.hpp>

namespace {

void fill_plane(AVFrame& frame, int plane, int width, int height, std::uint16_t seed) {
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(
            frame.data[plane] + y * frame.linesize[plane]);
        for (int x = 0; x < width; ++x) {
            row[x] = static_cast<std::uint16_t>((seed + x * 7 + y * 13) & 1023);
        }
    }
}

void compare_plane(const AVFrame& expected, const AVFrame& actual,
                   int plane, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const auto* expected_row = reinterpret_cast<const std::uint16_t*>(
            expected.data[plane] + y * expected.linesize[plane]);
        const auto* actual_row = reinterpret_cast<const std::uint16_t*>(
            actual.data[plane] + y * actual.linesize[plane]);
        CHECK(std::equal(expected_row, expected_row + width, actual_row));
    }
}

} // namespace

TEST_CASE("FFmpeg Vulkan frames support yuv422p10 upload and exact readback") {
    constexpr int width = 64;
    constexpr int height = 32;
    mcraw::VulkanRuntime runtime;
    mcraw::FfmpegVulkanFrameContext frames(runtime, {width, height, 4});

    mcraw::FrameMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.pts = 123;
    metadata.time_base = {1, 90'000};
    metadata.range = AVCOL_RANGE_MPEG;
    metadata.matrix = AVCOL_SPC_BT2020_NCL;
    auto hardware = frames.allocate_frame(metadata);
    const auto allocation = frames.inspect_frame(*hardware.frame);
    CHECK(allocation.image_count > 0U);
    CHECK((frames.image_usage() & VK_IMAGE_USAGE_STORAGE_BIT) != 0U);
    CHECK((frames.image_usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U);
    CHECK((frames.image_usage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U);

    auto source = mcraw::make_av_frame();
    source->format = AV_PIX_FMT_YUV422P10LE;
    source->width = width;
    source->height = height;
    mcraw::require_ffmpeg(av_frame_get_buffer(source.get(), 32), "allocate upload source");
    mcraw::require_ffmpeg(av_frame_make_writable(source.get()), "make upload source writable");
    fill_plane(*source, 0, width, height, 64);
    fill_plane(*source, 1, width / 2, height, 128);
    fill_plane(*source, 2, width / 2, height, 256);

    mcraw::require_ffmpeg(av_hwframe_transfer_data(hardware.frame.get(), source.get(), 0),
                          "upload yuv422p10 test frame");
    auto readback = mcraw::make_av_frame();
    readback->format = AV_PIX_FMT_YUV422P10LE;
    readback->width = width;
    readback->height = height;
    mcraw::require_ffmpeg(av_hwframe_transfer_data(readback.get(), hardware.frame.get(), 0),
                          "read back yuv422p10 test frame");

    const auto after_transfer = frames.inspect_frame(*hardware.frame);
    REQUIRE(after_transfer.semaphore_values.size() == allocation.semaphore_values.size());
    CHECK(std::equal(after_transfer.semaphore_values.begin(),
                     after_transfer.semaphore_values.end(),
                     allocation.semaphore_values.begin(),
                     std::greater<>{}));

    compare_plane(*source, *readback, 0, width, height);
    compare_plane(*source, *readback, 1, width / 2, height);
    compare_plane(*source, *readback, 2, width / 2, height);
}

TEST_CASE("FFmpeg Vulkan frame pool allocates the 4K sample dimensions") {
    mcraw::VulkanRuntime runtime;
    mcraw::FfmpegVulkanFrameContext frames(runtime, {4096, 3072, 2});
    mcraw::FrameMetadata metadata;
    metadata.width = 4096;
    metadata.height = 3072;
    metadata.time_base = {1, 90'000};

    auto hardware = frames.allocate_frame(metadata);
    const auto allocation = frames.inspect_frame(*hardware.frame);

    CHECK(allocation.image_count > 0U);
    CHECK(frames.software_format() == AV_PIX_FMT_YUV422P10LE);
}

TEST_CASE("FFmpeg Vulkan frame context rejects odd width") {
    mcraw::VulkanRuntime runtime;
    CHECK_THROWS(mcraw::FfmpegVulkanFrameContext(runtime, {63, 32, 2}));
}
