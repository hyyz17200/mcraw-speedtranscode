#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <mcraw/core/error.hpp>
#include <mcraw/output/ffmpeg_raii.hpp>
#include <mcraw/processing/yuv.hpp>
#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>

namespace {

mcraw::TargetLogRgbF32 golden_patterns(std::uint32_t width, std::uint32_t height) {
    mcraw::TargetLogRgbF32 input;
    input.width = width;
    input.height = height;
    for (auto& plane : input.planes) {
        plane.resize(static_cast<std::size_t>(width) * height);
    }
    constexpr float pi = 3.14159265358979323846F;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * width + x;
            const float gradient = static_cast<float>(x) / static_cast<float>(width - 1U);
            std::array<float, 3> rgb{};
            switch (y % 6U) {
            case 0: // grayscale and Log-gradient behavior
                rgb = {gradient, gradient, gradient};
                break;
            case 1: // saturated single-pixel color line
                rgb = x == width / 2U ? std::array<float, 3>{1.0F, 0.0F, 0.0F}
                                      : std::array<float, 3>{0.0F, 0.0F, 1.0F};
                break;
            case 2: // red/blue checkerboard
                rgb = ((x + y) & 1U) == 0U ? std::array<float, 3>{1.0F, 0.0F, 0.0F}
                                            : std::array<float, 3>{0.0F, 0.0F, 1.0F};
                break;
            case 3: { // one-dimensional zone plate
                const float phase = pi * static_cast<float>(x * x) /
                                    static_cast<float>(width);
                rgb = {0.5F + 0.6F * std::sin(phase),
                       0.5F + 0.6F * std::sin(phase + 2.0F),
                       0.5F + 0.6F * std::sin(phase + 4.0F)};
                break;
            }
            case 4: // saturated edge and out-of-range clipping
                rgb = x < width / 2U ? std::array<float, 3>{1.2F, -0.1F, 0.1F}
                                     : std::array<float, 3>{-0.1F, 1.1F, 1.2F};
                break;
            default:
                rgb = {gradient, gradient * gradient, std::sqrt(gradient)};
                break;
            }
            for (std::size_t plane = 0; plane < rgb.size(); ++plane) {
                input.planes[plane][pixel] = rgb[plane];
            }
        }
    }
    input.validate();
    return input;
}

struct DifferenceMetrics {
    int maximum{};
    double rmse{};
    std::size_t differing{};
};

DifferenceMetrics compare_plane(const std::vector<std::uint16_t>& expected,
                                const std::vector<std::uint16_t>& actual) {
    REQUIRE(expected.size() == actual.size());
    DifferenceMetrics result;
    double squared = 0.0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const int difference = std::abs(static_cast<int>(expected[index]) -
                                        static_cast<int>(actual[index]));
        result.maximum = std::max(result.maximum, difference);
        result.differing += difference != 0 ? 1U : 0U;
        squared += static_cast<double>(difference * difference);
    }
    result.rmse = expected.empty() ? 0.0 : std::sqrt(squared / expected.size());
    return result;
}

void check_against_reference(mcraw::ChromaFilter filter, bool dither) {
    constexpr std::uint32_t width = 128;
    constexpr std::uint32_t height = 36;
    constexpr std::size_t frame_index = 17;
    const auto input = golden_patterns(width, height);
    const auto reference = mcraw::pack_dwg_log_to_yuv422p10(
        input, filter, dither, frame_index).image;
    const bool validation = std::getenv("MCRAW_VULKAN_SHADER_VALIDATION") != nullptr;
    mcraw::VulkanRuntime runtime({"auto", validation});
    mcraw::VulkanRgbToYuv422 gpu(
        runtime, {width, height, filter, dither, mcraw::GpuPrecision::fp32});
    const auto output = gpu.pack(input, frame_index);

    const auto y = compare_plane(reference.y, output.y);
    const auto cb = compare_plane(reference.cb, output.cb);
    const auto cr = compare_plane(reference.cr, output.cr);
    CAPTURE(y.maximum, y.rmse, y.differing,
            cb.maximum, cb.rmse, cb.differing,
            cr.maximum, cr.rmse, cr.differing);
    CHECK(y.maximum <= 1);
    CHECK(cb.maximum <= 1);
    CHECK(cr.maximum <= 1);
    CHECK(y.rmse < 0.08);
    CHECK(cb.rmse < 0.08);
    CHECK(cr.rmse < 0.08);
    const auto telemetry = gpu.telemetry();
    CHECK(telemetry.dispatches == 1U);
    CHECK(telemetry.upload_bytes == static_cast<std::uint64_t>(width) * height * 3U * 4U);
    CHECK(telemetry.download_bytes == static_cast<std::uint64_t>(width) * height * 2U * 4U);
    CHECK(telemetry.last_dispatch_wall_ms > 0.0);
    CHECK(telemetry.gpu_timestamps_supported);
    CHECK(telemetry.gpu_timestamp_samples == 1U);
    CHECK(telemetry.gpu_total_ms > 0.0);
    CHECK(telemetry.gpu_mean_ms > 0.0);
    CHECK(telemetry.gpu_p50_ms > 0.0);
    CHECK(telemetry.last_gpu_dispatch_ms > 0.0);
}

} // namespace

TEST_CASE("Vulkan FP32 RGB-to-YUV quality filter matches CPU golden patterns") {
    check_against_reference(mcraw::ChromaFilter::quality, true);
}

TEST_CASE("Vulkan FP32 RGB-to-YUV fast filter matches CPU golden patterns") {
    check_against_reference(mcraw::ChromaFilter::fast, false);
}

TEST_CASE("Vulkan RGB-to-YUV is deterministic and handles the minimum even width") {
    const auto input = golden_patterns(2, 6);
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRgbToYuv422 gpu(
        runtime, {2, 6, mcraw::ChromaFilter::quality, true, mcraw::GpuPrecision::fp32});
    const auto first = gpu.pack(input, 99);
    const auto second = gpu.pack(input, 99);
    CHECK(first.y == second.y);
    CHECK(first.cb == second.cb);
    CHECK(first.cr == second.cr);
    const auto reference = mcraw::pack_dwg_log_to_yuv422p10(
        input, mcraw::ChromaFilter::quality, true, 99).image;
    CHECK(compare_plane(reference.y, first.y).maximum <= 1);
    CHECK(compare_plane(reference.cb, first.cb).maximum <= 1);
    CHECK(compare_plane(reference.cr, first.cr).maximum <= 1);
}

TEST_CASE("Vulkan RGB-to-YUV rejects odd width and unvalidated FP16") {
    mcraw::VulkanRuntime runtime;
    CHECK_THROWS_AS(mcraw::VulkanRgbToYuv422(
        runtime, {63, 32, mcraw::ChromaFilter::quality, true,
                  mcraw::GpuPrecision::fp32}), mcraw::Error);
    CHECK_THROWS_AS(mcraw::VulkanRgbToYuv422(
        runtime, {64, 32, mcraw::ChromaFilter::quality, true,
                  mcraw::GpuPrecision::fp16}), mcraw::Error);
}

TEST_CASE("Vulkan RGB-to-YUV writes the FFmpeg encoder frame pool directly") {
    constexpr std::uint32_t width = 128;
    constexpr std::uint32_t height = 36;
    constexpr std::size_t frame_index = 23;
    const auto input = golden_patterns(width, height);
    const auto reference = mcraw::pack_dwg_log_to_yuv422p10(
        input, mcraw::ChromaFilter::quality, true, frame_index).image;
    mcraw::VulkanRuntime runtime;
    mcraw::FfmpegVulkanFrameContext frames(runtime, {
        static_cast<int>(width), static_cast<int>(height), 4});
    mcraw::VulkanRgbToYuvFrameWriter writer(
        runtime, frames, {width, height, mcraw::ChromaFilter::quality, true,
                          mcraw::GpuPrecision::fp32});
    mcraw::FrameMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.pts = 0;
    metadata.duration = 3'000;
    metadata.time_base = {1, 90'000};
    metadata.range = AVCOL_RANGE_MPEG;
    metadata.matrix = AVCOL_SPC_BT2020_NCL;
    auto output = writer.pack(input, frame_index, metadata);
    const auto allocation = frames.inspect_frame(*output.frame);
    REQUIRE(allocation.image_count == 1U);
    CHECK(allocation.layouts.front() == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(allocation.access.front() == VK_ACCESS_SHADER_WRITE_BIT);
    CHECK(allocation.semaphore_values.front() == 1U);

    auto software = mcraw::make_av_frame();
    software->format = AV_PIX_FMT_YUV422P10LE;
    software->width = width;
    software->height = height;
    mcraw::require_ffmpeg(av_frame_get_buffer(software.get(), 32),
                          "allocate direct-frame golden readback");
    mcraw::require_ffmpeg(av_hwframe_transfer_data(software.get(), output.frame.get(), 0),
                          "read back direct-frame golden output");
    const auto compare_readback = [&](int plane,
                                      const std::vector<std::uint16_t>& expected,
                                      std::uint32_t plane_width) {
        std::vector<std::uint16_t> actual;
        actual.reserve(expected.size());
        for (std::uint32_t row = 0; row < height; ++row) {
            const auto* source = reinterpret_cast<const std::uint16_t*>(
                software->data[plane] + row * software->linesize[plane]);
            actual.insert(actual.end(), source, source + plane_width);
        }
        return compare_plane(expected, actual);
    };
    CHECK(compare_readback(0, reference.y, width).maximum <= 1);
    CHECK(compare_readback(1, reference.cb, width / 2U).maximum <= 1);
    CHECK(compare_readback(2, reference.cr, width / 2U).maximum <= 1);
    writer.wait();
    const auto telemetry = writer.telemetry();
    CHECK(telemetry.output_frames == 1U);
    CHECK(telemetry.yuv_upload_frames == 0U);
    CHECK(telemetry.yuv_readback_frames == 0U);
    CHECK(telemetry.gpu_timestamps_supported);
    CHECK(telemetry.gpu_timestamp_samples == 1U);
    CHECK(telemetry.gpu_mean_ms > 0.0);
}

TEST_CASE("Vulkan RGB-to-YUV packs the 4K sample dimensions") {
    if (std::getenv("MCRAW_VULKAN_4K_TEST") == nullptr) {
        SKIP("set MCRAW_VULKAN_4K_TEST=1 for the high-memory 4K dispatch");
    }
    constexpr std::uint32_t width = 4096;
    constexpr std::uint32_t height = 3072;
    mcraw::TargetLogRgbF32 input;
    input.width = width;
    input.height = height;
    for (auto& plane : input.planes) {
        plane.assign(static_cast<std::size_t>(width) * height, 0.18F);
    }
    mcraw::VulkanRuntime runtime;
    mcraw::VulkanRgbToYuv422 gpu(
        runtime, {width, height, mcraw::ChromaFilter::fast, false,
                  mcraw::GpuPrecision::fp32});
    const auto output = gpu.pack(input, 0);
    REQUIRE(output.y.size() == static_cast<std::size_t>(width) * height);
    REQUIRE(output.cb.size() == output.y.size() / 2U);
    CHECK(output.y.front() == 222U);
    CHECK(output.y.back() == 222U);
    CHECK(output.cb.front() == 512U);
    CHECK(output.cr.back() == 512U);
}
