#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include <mcraw/output/video_frame.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>

namespace mcraw {

struct FfmpegVulkanFrameContextConfig {
    int width{};
    int height{};
    std::size_t pool_size{8};
    AVPixelFormat software_format{AV_PIX_FMT_YUV422P10LE};
};

struct VulkanFrameAllocationInfo {
    std::size_t image_count{};
    std::vector<VkFormat> formats;
    std::vector<VkImageLayout> layouts;
    std::vector<VkAccessFlags> access;
    std::vector<std::uint32_t> queue_families;
    std::vector<std::uint64_t> semaphore_values;
};

class FfmpegVulkanFrameContext {
public:
    FfmpegVulkanFrameContext(VulkanRuntime& runtime,
                             FfmpegVulkanFrameContextConfig config);
    ~FfmpegVulkanFrameContext();
    FfmpegVulkanFrameContext(FfmpegVulkanFrameContext&&) noexcept;
    FfmpegVulkanFrameContext& operator=(FfmpegVulkanFrameContext&&) noexcept;
    FfmpegVulkanFrameContext(const FfmpegVulkanFrameContext&) = delete;
    FfmpegVulkanFrameContext& operator=(const FfmpegVulkanFrameContext&) = delete;

    [[nodiscard]] VulkanVideoFrame allocate_frame(FrameMetadata metadata) const;
    [[nodiscard]] VulkanFrameAllocationInfo inspect_frame(const AVFrame& frame) const;
    [[nodiscard]] AvBufferRefPtr reference_frames_context() const;
    [[nodiscard]] AVPixelFormat software_format() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::size_t pool_size() const noexcept;
    [[nodiscard]] VkImageUsageFlags image_usage() const noexcept;
    [[nodiscard]] bool owns(const AVFrame& frame) const noexcept;
    [[nodiscard]] const std::vector<AVPixelFormat>& valid_software_formats() const noexcept;
    [[nodiscard]] const std::vector<VkFormat>& image_formats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
