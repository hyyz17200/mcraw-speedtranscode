#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct VulkanRgbToYuvConfig {
    std::uint32_t width{};
    std::uint32_t height{};
    ChromaFilter chroma_filter{ChromaFilter::quality};
    bool deterministic_dither{true};
    GpuPrecision precision{GpuPrecision::fp32};
    std::size_t slots{1};
};

struct VulkanRgbToYuvTelemetry {
    std::uint64_t dispatches{};
    std::uint64_t upload_bytes{};
    std::uint64_t download_bytes{};
    double last_dispatch_ms{};
};

class VulkanRgbToYuv422 {
public:
    // The runtime must outlive this object. Phase 4 deliberately waits for
    // each dispatch and downloads raw YUV for golden validation; it is not the
    // asynchronous GPU-resident handoff introduced in the following phase.
    VulkanRgbToYuv422(VulkanRuntime& runtime, VulkanRgbToYuvConfig config);
    ~VulkanRgbToYuv422();
    VulkanRgbToYuv422(VulkanRgbToYuv422&&) noexcept;
    VulkanRgbToYuv422& operator=(VulkanRgbToYuv422&&) noexcept;
    VulkanRgbToYuv422(const VulkanRgbToYuv422&) = delete;
    VulkanRgbToYuv422& operator=(const VulkanRgbToYuv422&) = delete;

    [[nodiscard]] Yuv422P10 pack(const TargetLogRgbF32& input,
                                 std::size_t frame_index);
    [[nodiscard]] VulkanRgbToYuvTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct VulkanRgbToYuvFrameTelemetry {
    std::uint64_t dispatches{};
    std::uint64_t rgb_upload_bytes{};
    std::uint64_t output_frames{};
    std::uint64_t yuv_upload_frames{};
    std::uint64_t yuv_readback_frames{};
    std::size_t slot_count{};
    std::size_t in_flight{};
    std::size_t max_in_flight{};
    std::uint64_t backpressure_waits{};
    double backpressure_wait_ms{};
    double last_dispatch_ms{};
};

class VulkanRgbToYuvFrameWriter {
public:
    VulkanRgbToYuvFrameWriter(VulkanRuntime& runtime,
                              FfmpegVulkanFrameContext& frames,
                              VulkanRgbToYuvConfig config);
    ~VulkanRgbToYuvFrameWriter();
    VulkanRgbToYuvFrameWriter(VulkanRgbToYuvFrameWriter&&) noexcept;
    VulkanRgbToYuvFrameWriter& operator=(VulkanRgbToYuvFrameWriter&&) noexcept;
    VulkanRgbToYuvFrameWriter(const VulkanRgbToYuvFrameWriter&) = delete;
    VulkanRgbToYuvFrameWriter& operator=(const VulkanRgbToYuvFrameWriter&) = delete;

    // Returns the exact FFmpeg pool frame written by the compute shader. No
    // uncompressed YUV upload or readback occurs on this production path.
    [[nodiscard]] VulkanVideoFrame pack(const TargetLogRgbF32& input,
                                        std::size_t frame_index,
                                        FrameMetadata metadata);
    void wait();
    [[nodiscard]] VulkanRgbToYuvFrameTelemetry telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
