#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include <mcraw/output/ffmpeg_raii.hpp>

struct AVHWDeviceContext;
struct AVVulkanDeviceContext;

namespace mcraw {

struct VulkanQueueFamilyInfo {
    std::uint32_t index{};
    std::uint32_t queue_count{};
    VkQueueFlags flags{};
};

struct VulkanDeviceInfo {
    std::uint32_t enumeration_index{};
    std::string name;
    VkPhysicalDeviceType type{VK_PHYSICAL_DEVICE_TYPE_OTHER};
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t api_version{};
    std::uint32_t driver_version{};
    std::string driver_name;
    std::string driver_info;
    std::string uuid;
    bool software{};
    std::vector<VulkanQueueFamilyInfo> queue_families;
};

struct VulkanRuntimeConfig {
    std::string selector{"auto"};
    bool enable_validation{};
};

class VulkanRuntime {
public:
    explicit VulkanRuntime(VulkanRuntimeConfig config = {});
    ~VulkanRuntime();
    VulkanRuntime(VulkanRuntime&&) noexcept;
    VulkanRuntime& operator=(VulkanRuntime&&) noexcept;
    VulkanRuntime(const VulkanRuntime&) = delete;
    VulkanRuntime& operator=(const VulkanRuntime&) = delete;

    [[nodiscard]] static std::vector<VulkanDeviceInfo> enumerate_devices();
    [[nodiscard]] static VulkanDeviceInfo select_device(
        const std::vector<VulkanDeviceInfo>& devices, std::string_view selector);

    [[nodiscard]] const VulkanDeviceInfo& device() const noexcept;
    [[nodiscard]] std::uint32_t compute_queue_family() const noexcept;
    [[nodiscard]] std::uint32_t compute_queue_count() const noexcept;
    [[nodiscard]] const std::vector<std::string>& instance_extensions() const noexcept;
    [[nodiscard]] const std::vector<std::string>& device_extensions() const noexcept;
    [[nodiscard]] AVHWDeviceContext* ffmpeg_device_context() const noexcept;
    [[nodiscard]] AVVulkanDeviceContext* ffmpeg_vulkan_context() const noexcept;
    [[nodiscard]] AvBufferRefPtr reference_device_context() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view vulkan_device_type_name(VkPhysicalDeviceType type) noexcept;

} // namespace mcraw
