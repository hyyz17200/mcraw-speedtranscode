#include <mcraw/vulkan/vulkan_camera_pipeline_resources.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

void require_camera_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw Error(result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost
                                                   : ErrorCode::processing_failed,
                    std::string(operation) + " failed with VkResult " +
                    std::to_string(static_cast<int>(result)));
    }
}

std::size_t checked_plane_bytes(std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U || (width & 1U) != 0U) {
        throw Error(ErrorCode::invalid_argument,
                    "Camera RGB Vulkan resources require non-zero dimensions and even width");
    }
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw Error(ErrorCode::invalid_argument,
                    "Camera RGB Vulkan resource size overflow");
    }
    return static_cast<std::size_t>(pixels * sizeof(float));
}

std::uint64_t checked_capacity(std::size_t plane_bytes,
                               std::size_t planes,
                               std::size_t slots) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (planes == 0U || slots == 0U ||
        plane_bytes > maximum / planes ||
        static_cast<std::uint64_t>(plane_bytes) * planes > maximum / slots) {
        throw Error(ErrorCode::invalid_argument,
                    "Camera RGB Vulkan telemetry size overflow");
    }
    return static_cast<std::uint64_t>(plane_bytes) * planes * slots;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
bool has_queue_lock(const AVVulkanDeviceContext& device) {
    return device.lock_queue != nullptr && device.unlock_queue != nullptr;
}

void lock_queue(AVVulkanDeviceContext& device,
                AVHWDeviceContext* owner,
                std::uint32_t family) {
    device.lock_queue(owner, family, 0);
}

void unlock_queue(AVVulkanDeviceContext& device,
                  AVHWDeviceContext* owner,
                  std::uint32_t family) {
    device.unlock_queue(owner, family, 0);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace

class VulkanCameraPipelineResources::Impl {
public:
    struct Buffer {
        VkBuffer handle{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    struct Slot {
        std::array<Buffer, 3> upload;
        std::array<std::array<Buffer, 3>, 2> intermediate;
        std::array<Buffer, 3> readback;
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        bool in_flight{};
    };

    Impl(VulkanRuntime& runtime_owner,
         VulkanCameraPipelineResourceConfig requested)
        : runtime(runtime_owner), config(requested),
          plane_bytes(checked_plane_bytes(config.width, config.height)) {
        if (config.slots == 0U || config.slots > 64U) {
            throw Error(ErrorCode::invalid_argument,
                        "Camera RGB Vulkan resource slots must be between 1 and 64");
        }
        hw_device = runtime.ffmpeg_device_context();
        vulkan_device = runtime.ffmpeg_vulkan_context();
        if (hw_device == nullptr || vulkan_device == nullptr ||
            vulkan_device->act_dev == VK_NULL_HANDLE ||
            vulkan_device->phys_dev == VK_NULL_HANDLE ||
            !has_queue_lock(*vulkan_device)) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg-owned Vulkan device is unavailable to Camera RGB resources");
        }
        device = vulkan_device->act_dev;
        physical_device = vulkan_device->phys_dev;
        allocator = vulkan_device->alloc;
        queue_family = runtime.compute_queue_family();
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (queue == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "cannot obtain Vulkan compute queue for Camera RGB resources");
        }
        slots.resize(config.slots);
        counters.slot_count = slots.size();
        counters.camera_upload_capacity_bytes = checked_capacity(plane_bytes, 3U, slots.size());
        counters.intermediate_capacity_bytes = checked_capacity(plane_bytes, 6U, slots.size());
        counters.test_readback_capacity_bytes = config.enable_test_readback
            ? checked_capacity(plane_bytes, 3U, slots.size()) : 0U;
        try {
            create_buffers();
            create_commands();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    std::uint32_t memory_type(std::uint32_t mask,
                              VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if ((mask & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        throw Error(ErrorCode::unsupported_format,
                    "GPU has no memory type required by Camera RGB resources");
    }

    Buffer create_buffer(VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties) {
        Buffer result;
        result.size = plane_bytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = result.size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_camera_vulkan(vkCreateBuffer(device, &info, allocator, &result.handle),
                              "create Camera RGB pipeline buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, properties);
        try {
            require_camera_vulkan(vkAllocateMemory(device, &allocation, allocator,
                                                   &result.memory),
                                  "allocate Camera RGB pipeline memory");
            require_camera_vulkan(vkBindBufferMemory(device, result.handle,
                                                     result.memory, 0),
                                  "bind Camera RGB pipeline memory");
        } catch (...) {
            if (result.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, result.memory, allocator);
            }
            vkDestroyBuffer(device, result.handle, allocator);
            throw;
        }
        return result;
    }

    void create_buffers() {
        constexpr auto upload_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        constexpr auto intermediate_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        constexpr auto host_properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (auto& slot : slots) {
            for (auto& buffer : slot.upload) {
                buffer = create_buffer(upload_usage, host_properties);
            }
            for (auto& set : slot.intermediate) {
                for (auto& buffer : set) {
                    buffer = create_buffer(intermediate_usage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                }
            }
            if (config.enable_test_readback) {
                for (auto& buffer : slot.readback) {
                    buffer = create_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           host_properties);
                }
            }
        }
    }

    void create_commands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family;
        require_camera_vulkan(vkCreateCommandPool(device, &pool, allocator,
                                                  &command_pool),
                              "create Camera RGB command pool");
        std::vector<VkCommandBuffer> commands(slots.size());
        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        require_camera_vulkan(vkAllocateCommandBuffers(device, &allocation,
                                                       commands.data()),
                              "allocate Camera RGB command buffers");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].command = commands[index];
            require_camera_vulkan(vkCreateFence(device, &fence, allocator,
                                                &slots[index].fence),
                                  "create Camera RGB slot fence");
        }
    }

    void upload_plane(const Buffer& buffer, const std::vector<float>& values) {
        if (values.size() * sizeof(float) != plane_bytes) {
            throw Error(ErrorCode::invalid_argument,
                        "Camera RGB upload plane size changed");
        }
        void* mapped = nullptr;
        require_camera_vulkan(vkMapMemory(device, buffer.memory, 0, buffer.size,
                                         0, &mapped),
                              "map Camera RGB upload memory");
        std::memcpy(mapped, values.data(), plane_bytes);
        vkUnmapMemory(device, buffer.memory);
    }

    std::vector<float> readback_plane(const Buffer& buffer) {
        std::vector<float> values(plane_bytes / sizeof(float));
        void* mapped = nullptr;
        require_camera_vulkan(vkMapMemory(device, buffer.memory, 0, buffer.size,
                                         0, &mapped),
                              "map Camera RGB test readback memory");
        std::memcpy(values.data(), mapped, plane_bytes);
        vkUnmapMemory(device, buffer.memory);
        return values;
    }

    void wait_slot(Slot& slot) {
        if (!slot.in_flight) return;
        require_camera_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                             UINT64_MAX),
                              "wait for Camera RGB slot");
        slot.in_flight = false;
    }

    CameraRgbF32 round_trip_for_test(const CameraRgbF32& input) {
        if (!config.enable_test_readback) {
            throw Error(ErrorCode::unsupported_format,
                        "Camera RGB test readback was not enabled");
        }
        input.validate();
        if (input.width != config.width || input.height != config.height) {
            throw Error(ErrorCode::invalid_argument,
                        "Camera RGB test input dimensions do not match resources");
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        wait_slot(slot);
        for (std::size_t plane = 0; plane < 3U; ++plane) {
            upload_plane(slot.upload[plane], input.planes[plane]);
        }
        require_camera_vulkan(vkResetFences(device, 1, &slot.fence),
                              "reset Camera RGB slot fence");
        require_camera_vulkan(vkResetCommandBuffer(slot.command, 0),
                              "reset Camera RGB command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_camera_vulkan(vkBeginCommandBuffer(slot.command, &begin),
                              "begin Camera RGB test command buffer");
        VkBufferCopy copy{0, 0, plane_bytes};
        for (std::size_t plane = 0; plane < 3U; ++plane) {
            vkCmdCopyBuffer(slot.command, slot.upload[plane].handle,
                            slot.intermediate[0][plane].handle, 1, &copy);
        }
        std::array<VkBufferMemoryBarrier, 3> barriers{};
        for (std::size_t plane = 0; plane < barriers.size(); ++plane) {
            auto& barrier = barriers[plane];
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = slot.intermediate[0][plane].handle;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             static_cast<std::uint32_t>(barriers.size()),
                             barriers.data(), 0, nullptr);
        for (std::size_t plane = 0; plane < 3U; ++plane) {
            vkCmdCopyBuffer(slot.command, slot.intermediate[0][plane].handle,
                            slot.readback[plane].handle, 1, &copy);
        }
        for (std::size_t plane = 0; plane < barriers.size(); ++plane) {
            auto& barrier = barriers[plane];
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.buffer = slot.readback[plane].handle;
        }
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                             static_cast<std::uint32_t>(barriers.size()),
                             barriers.data(), 0, nullptr);
        require_camera_vulkan(vkEndCommandBuffer(slot.command),
                              "end Camera RGB test command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        lock_queue(*vulkan_device, hw_device, queue_family);
        const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
        unlock_queue(*vulkan_device, hw_device, queue_family);
        require_camera_vulkan(submit_result, "submit Camera RGB test round-trip");
        slot.in_flight = true;
        wait_slot(slot);

        CameraRgbF32 output{config.width, config.height, {}};
        for (std::size_t plane = 0; plane < 3U; ++plane) {
            output.planes[plane] = readback_plane(slot.readback[plane]);
        }
        ++counters.test_round_trips;
        const auto frame_bytes = static_cast<std::uint64_t>(plane_bytes) * 3U;
        counters.test_upload_bytes += frame_bytes;
        counters.test_readback_bytes += frame_bytes;
        return output;
    }

    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.handle != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.handle, allocator);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, allocator);
        }
        buffer = {};
    }

    void cleanup() noexcept {
        for (auto& slot : slots) {
            if (slot.in_flight && slot.fence != VK_NULL_HANDLE) {
                vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                slot.in_flight = false;
            }
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(device, slot.fence, allocator);
                slot.fence = VK_NULL_HANDLE;
            }
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, allocator);
            command_pool = VK_NULL_HANDLE;
        }
        for (auto& slot : slots) {
            for (auto& buffer : slot.upload) destroy_buffer(buffer);
            for (auto& set : slot.intermediate) {
                for (auto& buffer : set) destroy_buffer(buffer);
            }
            for (auto& buffer : slot.readback) destroy_buffer(buffer);
        }
    }

    VulkanRuntime& runtime;
    VulkanCameraPipelineResourceConfig config;
    std::size_t plane_bytes{};
    AVHWDeviceContext* hw_device{};
    AVVulkanDeviceContext* vulkan_device{};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    const VkAllocationCallbacks* allocator{};
    std::uint32_t queue_family{};
    VkQueue queue{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    std::vector<Slot> slots;
    std::size_t next_slot{};
    VulkanCameraPipelineResourceTelemetry counters;
};

VulkanCameraPipelineResources::VulkanCameraPipelineResources(
    VulkanRuntime& runtime, VulkanCameraPipelineResourceConfig config)
    : impl_(std::make_unique<Impl>(runtime, config)) {}
VulkanCameraPipelineResources::~VulkanCameraPipelineResources() = default;
VulkanCameraPipelineResources::VulkanCameraPipelineResources(
    VulkanCameraPipelineResources&&) noexcept = default;
VulkanCameraPipelineResources& VulkanCameraPipelineResources::operator=(
    VulkanCameraPipelineResources&&) noexcept = default;

CameraRgbF32 VulkanCameraPipelineResources::round_trip_for_test(
    const CameraRgbF32& input) {
    return impl_->round_trip_for_test(input);
}

VulkanCameraPipelineResourceTelemetry
VulkanCameraPipelineResources::telemetry() const noexcept {
    return impl_->counters;
}

} // namespace mcraw
