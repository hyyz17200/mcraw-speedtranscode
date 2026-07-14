#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct VulkanRgbToYuvConfig {
    std::uint32_t width{};
    std::uint32_t height{};
    ChromaFilter chroma_filter{ChromaFilter::quality};
    bool deterministic_dither{true};
    GpuPrecision precision{GpuPrecision::fp32};
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

} // namespace mcraw
