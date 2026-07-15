#include <mcraw/vulkan/vulkan_raw_pipeline_resources.hpp>

#include <algorithm>
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

void require_raw_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw Error(result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost
                                                   : ErrorCode::processing_failed,
                    std::string(operation) + " failed with VkResult " +
                    std::to_string(static_cast<int>(result)));
    }
}

std::size_t checked_pixels(std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U || (width & 1U) != 0U) {
        throw Error(ErrorCode::invalid_argument,
                    "U16 RAW Vulkan resources require non-zero dimensions and even width");
    }
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::uint32_t>::max() ||
        pixels > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw Error(ErrorCode::invalid_argument,
                    "U16 RAW Vulkan resource size exceeds the shader domain");
    }
    return static_cast<std::size_t>(pixels);
}

std::uint64_t checked_capacity(std::size_t bytes, std::size_t planes,
                               std::size_t slots) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (planes == 0U || slots == 0U || bytes > maximum / planes ||
        static_cast<std::uint64_t>(bytes) * planes > maximum / slots) {
        throw Error(ErrorCode::invalid_argument,
                    "U16 RAW Vulkan telemetry size overflow");
    }
    return static_cast<std::uint64_t>(bytes) * planes * slots;
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

void lock_queue(AVVulkanDeviceContext& device, AVHWDeviceContext* owner,
                std::uint32_t family) {
    device.lock_queue(owner, family, 0);
}

void unlock_queue(AVVulkanDeviceContext& device, AVHWDeviceContext* owner,
                  std::uint32_t family) {
    device.unlock_queue(owner, family, 0);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace

class VulkanRawPipelineResources::Impl {
public:
    struct Buffer {
        VkBuffer handle{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    struct Slot {
        Buffer upload;
        Buffer calibrated;
        std::vector<Buffer> camera_rgb;
        std::vector<Buffer> scratch;
        Buffer readback;
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
    };

    Impl(VulkanRuntime& runtime_owner, VulkanRawPipelineResourceConfig requested)
        : runtime(runtime_owner), config(requested),
          pixels(checked_pixels(config.width, config.height)),
          raw_bytes(pixels * sizeof(std::uint16_t)),
          float_plane_bytes(pixels * sizeof(float)) {
        if (config.slots == 0U || config.slots > 64U) {
            throw Error(ErrorCode::invalid_argument,
                        "U16 RAW Vulkan resource slots must be between 1 and 64");
        }
        hw_device = runtime.ffmpeg_device_context();
        vulkan_device = runtime.ffmpeg_vulkan_context();
        if (hw_device == nullptr || vulkan_device == nullptr ||
            vulkan_device->act_dev == VK_NULL_HANDLE ||
            vulkan_device->phys_dev == VK_NULL_HANDLE ||
            !has_queue_lock(*vulkan_device)) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg-owned Vulkan device is unavailable to U16 RAW resources");
        }
        device = vulkan_device->act_dev;
        physical_device = vulkan_device->phys_dev;
        allocator = vulkan_device->alloc;
        queue_family = runtime.compute_queue_family();
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (queue == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "cannot obtain Vulkan compute queue for U16 RAW resources");
        }
        slots.resize(config.slots);
        counters.slot_count = slots.size();
        counters.u16_upload_capacity_bytes = checked_capacity(raw_bytes, 1U, slots.size());
        counters.calibrated_capacity_bytes =
            checked_capacity(float_plane_bytes, 1U, slots.size());
        counters.camera_rgb_capacity_bytes =
            checked_capacity(float_plane_bytes, 3U, slots.size());
        // Five full-plane scratch buffers cover the unfused RCD direction,
        // low-pass and diagonal-difference contract. Exact aliasing is deferred
        // until the RCD pass implementation can prove lifetimes.
        counters.rcd_scratch_capacity_bytes =
            checked_capacity(float_plane_bytes, 5U, slots.size());
        counters.test_readback_capacity_bytes = config.enable_test_readback
            ? checked_capacity(raw_bytes, 1U, slots.size()) : 0U;
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
                    "GPU has no memory type required by U16 RAW resources");
    }

    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties) {
        Buffer result;
        result.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_raw_vulkan(vkCreateBuffer(device, &info, allocator, &result.handle),
                           "create U16 RAW pipeline buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, properties);
        try {
            require_raw_vulkan(vkAllocateMemory(device, &allocation, allocator,
                                                &result.memory),
                               "allocate U16 RAW pipeline memory");
            require_raw_vulkan(vkBindBufferMemory(device, result.handle, result.memory, 0),
                               "bind U16 RAW pipeline memory");
        } catch (...) {
            if (result.memory != VK_NULL_HANDLE) vkFreeMemory(device, result.memory, allocator);
            vkDestroyBuffer(device, result.handle, allocator);
            throw;
        }
        return result;
    }

    void create_buffers() {
        constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        constexpr auto device_local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (auto& slot : slots) {
            slot.upload = create_buffer(raw_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
            slot.calibrated = create_buffer(float_plane_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, device_local);
            slot.camera_rgb.reserve(3U);
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                slot.camera_rgb.push_back(create_buffer(float_plane_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, device_local));
            }
            slot.scratch.reserve(5U);
            for (std::size_t index = 0; index < 5U; ++index) {
                slot.scratch.push_back(create_buffer(float_plane_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, device_local));
            }
            if (config.enable_test_readback) {
                slot.readback = create_buffer(raw_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
            }
        }
    }

    void create_commands() {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        require_raw_vulkan(vkCreateCommandPool(device, &pool_info, allocator, &command_pool),
                           "create U16 RAW command pool");
        std::vector<VkCommandBuffer> commands(slots.size());
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        require_raw_vulkan(vkAllocateCommandBuffers(device, &allocate, commands.data()),
                           "allocate U16 RAW command buffers");
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].command = commands[index];
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            require_raw_vulkan(vkCreateFence(device, &fence_info, allocator,
                                             &slots[index].fence),
                               "create U16 RAW slot fence");
        }
    }

    RawMosaicU16 round_trip(const RawMosaicU16& input) {
        if (!config.enable_test_readback) {
            throw Error(ErrorCode::invalid_argument,
                        "U16 RAW test readback is disabled for production resources");
        }
        input.validate();
        if (input.width != config.width || input.height != config.height) {
            throw Error(ErrorCode::invalid_argument,
                        "U16 RAW input dimensions do not match Vulkan resources");
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for U16 RAW test slot");
        require_raw_vulkan(vkResetFences(device, 1, &slot.fence),
                           "reset U16 RAW test slot");
        void* mapped = nullptr;
        require_raw_vulkan(vkMapMemory(device, slot.upload.memory, 0, raw_bytes, 0, &mapped),
                           "map U16 RAW upload");
        std::memcpy(mapped, input.pixels.data(), raw_bytes);
        vkUnmapMemory(device, slot.upload.memory);

        require_raw_vulkan(vkResetCommandBuffer(slot.command, 0),
                           "reset U16 RAW command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_raw_vulkan(vkBeginCommandBuffer(slot.command, &begin),
                           "begin U16 RAW command buffer");
        VkBufferCopy copy{0, 0, raw_bytes};
        vkCmdCopyBuffer(slot.command, slot.upload.handle, slot.readback.handle, 1, &copy);
        require_raw_vulkan(vkEndCommandBuffer(slot.command),
                           "end U16 RAW command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        lock_queue(*vulkan_device, hw_device, queue_family);
        const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
        unlock_queue(*vulkan_device, hw_device, queue_family);
        require_raw_vulkan(submit_result, "submit U16 RAW round trip");
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for U16 RAW round trip");

        RawMosaicU16 output{config.width, config.height, input.cfa,
                            std::vector<std::uint16_t>(pixels)};
        require_raw_vulkan(vkMapMemory(device, slot.readback.memory, 0, raw_bytes, 0, &mapped),
                           "map U16 RAW readback");
        std::memcpy(output.pixels.data(), mapped, raw_bytes);
        vkUnmapMemory(device, slot.readback.memory);
        output.validate();
        ++counters.test_round_trips;
        counters.test_upload_bytes += raw_bytes;
        counters.test_readback_bytes += raw_bytes;
        return output;
    }

    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.handle != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer.handle, allocator);
        if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer.memory, allocator);
        buffer = {};
    }

    void cleanup() noexcept {
        if (device == VK_NULL_HANDLE) return;
        for (auto& slot : slots) {
            if (slot.fence != VK_NULL_HANDLE) {
                vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                vkDestroyFence(device, slot.fence, allocator);
            }
            destroy_buffer(slot.readback);
            for (auto& buffer : slot.scratch) destroy_buffer(buffer);
            for (auto& buffer : slot.camera_rgb) destroy_buffer(buffer);
            destroy_buffer(slot.calibrated);
            destroy_buffer(slot.upload);
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, allocator);
            command_pool = VK_NULL_HANDLE;
        }
    }

    VulkanRuntime& runtime;
    VulkanRawPipelineResourceConfig config;
    std::size_t pixels{};
    std::size_t raw_bytes{};
    std::size_t float_plane_bytes{};
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
    VulkanRawPipelineResourceTelemetry counters;
};

VulkanRawPipelineResources::VulkanRawPipelineResources(
    VulkanRuntime& runtime, VulkanRawPipelineResourceConfig config)
    : impl_(std::make_unique<Impl>(runtime, config)) {}
VulkanRawPipelineResources::~VulkanRawPipelineResources() = default;
VulkanRawPipelineResources::VulkanRawPipelineResources(
    VulkanRawPipelineResources&&) noexcept = default;
VulkanRawPipelineResources& VulkanRawPipelineResources::operator=(
    VulkanRawPipelineResources&&) noexcept = default;

RawMosaicU16 VulkanRawPipelineResources::round_trip_for_test(
    const RawMosaicU16& input) {
    return impl_->round_trip(input);
}

VulkanRawPipelineResourceTelemetry VulkanRawPipelineResources::telemetry() const noexcept {
    return impl_->counters;
}

} // namespace mcraw
