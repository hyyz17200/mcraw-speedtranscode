#include <mcraw/vulkan/vulkan_raw_pipeline_resources.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
#include <mcraw/vulkan/calibrate_raw_spv.hpp>
#include <mcraw/vulkan/rcd_demosaic_spv.hpp>

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

struct alignas(16) CalibrationPushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pixel_count{};
    std::uint32_t reserved{};
    std::array<float, 4> black{};
    std::array<float, 4> white{};
};
static_assert(sizeof(CalibrationPushConstants) == 48U);
static_assert(offsetof(CalibrationPushConstants, black) == 16U);
static_assert(offsetof(CalibrationPushConstants, white) == 32U);

struct RcdPushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t cfa_pattern{};
    std::uint32_t pass_index{};
};
static_assert(sizeof(RcdPushConstants) == 16U);

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double index = p * static_cast<double>(sorted.size() - 1U);
    const auto lo = static_cast<std::size_t>(std::floor(index));
    const auto hi = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lo);
    return sorted[lo] * (1.0 - fraction) + sorted[hi] * fraction;
}

std::uint64_t timestamp_delta(std::uint64_t start, std::uint64_t end,
                              std::uint32_t valid_bits) {
    if (valid_bits >= 64U) return end - start;
    const auto mask = (std::uint64_t{1} << valid_bits) - 1U;
    return (end - start) & mask;
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
        Buffer calibrated_readback;
        std::vector<Buffer> camera_readback;
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        VkDescriptorSet calibration_descriptor{VK_NULL_HANDLE};
        VkDescriptorSet rcd_descriptor{VK_NULL_HANDLE};
        std::uint32_t timestamp_query_base{};
        std::uint32_t rcd_timestamp_query_base{};
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
        counters.calibrated_test_readback_capacity_bytes = config.enable_test_readback
            ? checked_capacity(float_plane_bytes, 1U, slots.size()) : 0U;
        counters.camera_rgb_test_readback_capacity_bytes = config.enable_test_readback
            ? checked_capacity(float_plane_bytes, 3U, slots.size()) : 0U;
        try {
            create_buffers();
            create_calibration_pipeline();
            create_rcd_pipeline();
            create_timestamp_queries();
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
                slot.calibrated_readback = create_buffer(
                    float_plane_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
                slot.camera_readback.reserve(3U);
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    slot.camera_readback.push_back(create_buffer(
                        float_plane_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host));
                }
            }
        }
    }

    void create_calibration_pipeline() {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        require_raw_vulkan(vkCreateDescriptorSetLayout(
            device, &layout_info, allocator, &calibration_descriptor_layout),
            "create RAW calibration descriptor layout");

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       static_cast<std::uint32_t>(slots.size() * 2U)};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = static_cast<std::uint32_t>(slots.size());
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        require_raw_vulkan(vkCreateDescriptorPool(device, &pool_info, allocator,
                                                  &descriptor_pool),
                           "create RAW calibration descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(slots.size(),
                                                   calibration_descriptor_layout);
        std::vector<VkDescriptorSet> descriptors(slots.size());
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        require_raw_vulkan(vkAllocateDescriptorSets(device, &allocate, descriptors.data()),
                           "allocate RAW calibration descriptor sets");
        for (std::size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
            auto& slot = slots[slot_index];
            slot.calibration_descriptor = descriptors[slot_index];
            std::array<VkDescriptorBufferInfo, 2> buffers{{
                {slot.upload.handle, 0, raw_bytes},
                {slot.calibrated.handle, 0, float_plane_bytes},
            }};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (std::uint32_t index = 0; index < writes.size(); ++index) {
                writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[index].dstSet = slot.calibration_descriptor;
                writes[index].dstBinding = index;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].pBufferInfo = &buffers[index];
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(CalibrationPushConstants)};
        VkPipelineLayoutCreateInfo pipeline_layout_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &calibration_descriptor_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        require_raw_vulkan(vkCreatePipelineLayout(device, &pipeline_layout_info, allocator,
                                                  &calibration_pipeline_layout),
                           "create RAW calibration pipeline layout");
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = generated::calibrate_raw_spv.size() * sizeof(std::uint32_t);
        shader_info.pCode = generated::calibrate_raw_spv.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        require_raw_vulkan(vkCreateShaderModule(device, &shader_info, allocator, &shader),
                           "create RAW calibration shader");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = stage;
        pipeline_info.layout = calibration_pipeline_layout;
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                     &pipeline_info, allocator,
                                                     &calibration_pipeline);
        vkDestroyShaderModule(device, shader, allocator);
        require_raw_vulkan(result, "create RAW calibration pipeline");
    }

    void create_rcd_pipeline() {
        std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        require_raw_vulkan(vkCreateDescriptorSetLayout(
            device, &layout_info, allocator, &rcd_descriptor_layout),
            "create RCD descriptor layout");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            static_cast<std::uint32_t>(slots.size() * bindings.size())};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = static_cast<std::uint32_t>(slots.size());
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        require_raw_vulkan(vkCreateDescriptorPool(device, &pool_info, allocator,
                                                  &rcd_descriptor_pool),
                           "create RCD descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(slots.size(), rcd_descriptor_layout);
        std::vector<VkDescriptorSet> descriptors(slots.size());
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = rcd_descriptor_pool;
        allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        require_raw_vulkan(vkAllocateDescriptorSets(device, &allocate, descriptors.data()),
                           "allocate RCD descriptor sets");
        for (std::size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
            auto& slot = slots[slot_index];
            slot.rcd_descriptor = descriptors[slot_index];
            std::array<VkDescriptorBufferInfo, 9> buffers{{
                {slot.calibrated.handle, 0, float_plane_bytes},
                {slot.scratch[0].handle, 0, float_plane_bytes},
                {slot.camera_rgb[0].handle, 0, float_plane_bytes},
                {slot.camera_rgb[1].handle, 0, float_plane_bytes},
                {slot.camera_rgb[2].handle, 0, float_plane_bytes},
                {slot.scratch[1].handle, 0, float_plane_bytes},
                {slot.scratch[2].handle, 0, float_plane_bytes},
                {slot.scratch[3].handle, 0, float_plane_bytes},
                {slot.scratch[4].handle, 0, float_plane_bytes},
            }};
            std::array<VkWriteDescriptorSet, 9> writes{};
            for (std::uint32_t index = 0; index < writes.size(); ++index) {
                writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[index].dstSet = slot.rcd_descriptor;
                writes[index].dstBinding = index;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].pBufferInfo = &buffers[index];
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
        VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(RcdPushConstants)};
        VkPipelineLayoutCreateInfo pipeline_layout_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &rcd_descriptor_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        require_raw_vulkan(vkCreatePipelineLayout(device, &pipeline_layout_info, allocator,
                                                  &rcd_pipeline_layout),
                           "create RCD pipeline layout");
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = generated::rcd_demosaic_spv.size() * sizeof(std::uint32_t);
        shader_info.pCode = generated::rcd_demosaic_spv.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        require_raw_vulkan(vkCreateShaderModule(device, &shader_info, allocator, &shader),
                           "create RCD shader");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = stage;
        pipeline_info.layout = rcd_pipeline_layout;
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                     &pipeline_info, allocator,
                                                     &rcd_pipeline);
        vkDestroyShaderModule(device, shader, allocator);
        require_raw_vulkan(result, "create RCD pipeline");
    }

    void create_timestamp_queries() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        timestamp_period_ns = properties.limits.timestampPeriod;
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
        timestamp_valid_bits = queue_family < families.size()
            ? families[queue_family].timestampValidBits : 0U;
        if (timestamp_valid_bits == 0U || timestamp_period_ns <= 0.0F) return;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = static_cast<std::uint32_t>(slots.size() * 4U);
        require_raw_vulkan(vkCreateQueryPool(device, &info, allocator, &timestamp_pool),
                           "create RAW calibration timestamp pool");
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].timestamp_query_base = static_cast<std::uint32_t>(index * 4U);
            slots[index].rcd_timestamp_query_base =
                slots[index].timestamp_query_base + 2U;
        }
        counters.gpu_timestamps_supported = true;
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

    void upload_raw(Slot& slot, const RawMosaicU16& input) {
        void* mapped = nullptr;
        require_raw_vulkan(vkMapMemory(device, slot.upload.memory, 0, raw_bytes, 0, &mapped),
                           "map U16 RAW upload");
        std::memcpy(mapped, input.pixels.data(), raw_bytes);
        vkUnmapMemory(device, slot.upload.memory);
    }

    void update_calibration_timestamp(const Slot& slot) {
        if (timestamp_pool == VK_NULL_HANDLE) return;
        std::array<std::uint64_t, 2> values{};
        require_raw_vulkan(vkGetQueryPoolResults(
            device, timestamp_pool, slot.timestamp_query_base, 2,
            sizeof(values), values.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "read RAW calibration timestamp");
        const double milliseconds = static_cast<double>(timestamp_delta(
            values[0], values[1], timestamp_valid_bits)) *
            static_cast<double>(timestamp_period_ns) / 1.0e6;
        calibration_samples.push_back(milliseconds);
        auto sorted = calibration_samples;
        std::sort(sorted.begin(), sorted.end());
        counters.raw_calibration_timestamp_samples = calibration_samples.size();
        counters.raw_calibration_gpu_total_ms = 0.0;
        for (const double sample : calibration_samples) {
            counters.raw_calibration_gpu_total_ms += sample;
        }
        counters.raw_calibration_gpu_mean_ms = counters.raw_calibration_gpu_total_ms /
            static_cast<double>(calibration_samples.size());
        counters.raw_calibration_gpu_p50_ms = percentile(sorted, 0.50);
        counters.raw_calibration_gpu_p95_ms = percentile(sorted, 0.95);
        counters.raw_calibration_gpu_p99_ms = percentile(sorted, 0.99);
        counters.raw_calibration_gpu_min_ms = sorted.front();
        counters.raw_calibration_gpu_max_ms = sorted.back();
    }

    void update_rcd_timestamp(const Slot& slot) {
        if (timestamp_pool == VK_NULL_HANDLE) return;
        std::array<std::uint64_t, 2> values{};
        require_raw_vulkan(vkGetQueryPoolResults(
            device, timestamp_pool, slot.rcd_timestamp_query_base, 2,
            sizeof(values), values.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "read RCD timestamp");
        const double milliseconds = static_cast<double>(timestamp_delta(
            values[0], values[1], timestamp_valid_bits)) *
            static_cast<double>(timestamp_period_ns) / 1.0e6;
        rcd_samples.push_back(milliseconds);
        auto sorted = rcd_samples;
        std::sort(sorted.begin(), sorted.end());
        counters.rcd_demosaic_timestamp_samples = rcd_samples.size();
        counters.rcd_demosaic_gpu_total_ms = 0.0;
        for (const double sample : rcd_samples) counters.rcd_demosaic_gpu_total_ms += sample;
        counters.rcd_demosaic_gpu_mean_ms = counters.rcd_demosaic_gpu_total_ms /
            static_cast<double>(rcd_samples.size());
        counters.rcd_demosaic_gpu_p50_ms = percentile(sorted, 0.50);
        counters.rcd_demosaic_gpu_p95_ms = percentile(sorted, 0.95);
        counters.rcd_demosaic_gpu_p99_ms = percentile(sorted, 0.99);
        counters.rcd_demosaic_gpu_min_ms = sorted.front();
        counters.rcd_demosaic_gpu_max_ms = sorted.back();
    }

    CalibrationPushConstants calibration_parameters(
        const NormalizedCameraMetadata& metadata) const {
        CalibrationPushConstants push;
        push.width = config.width;
        push.height = config.height;
        push.pixel_count = static_cast<std::uint32_t>(pixels);
        for (std::size_t index = 0; index < 4U; ++index) {
            push.black[index] = static_cast<float>(metadata.black_level[index]);
            push.white[index] = static_cast<float>(metadata.white_level[index]);
        }
        return push;
    }

    void record_calibration(Slot& slot, const CalibrationPushConstants& push) {
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestamp_pool, slot.timestamp_query_base);
        }
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          calibration_pipeline);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                calibration_pipeline_layout, 0, 1,
                                &slot.calibration_descriptor, 0, nullptr);
        vkCmdPushConstants(slot.command, calibration_pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(slot.command,
                      (static_cast<std::uint32_t>(pixels) + 255U) / 256U, 1, 1);
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestamp_pool, slot.timestamp_query_base + 1U);
        }
    }

    void shader_buffer_barrier(VkCommandBuffer command,
                               const std::vector<VkBuffer>& buffers,
                               VkAccessFlags destination_access =
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VkPipelineStageFlags destination_stage =
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {
        std::vector<VkBufferMemoryBarrier> barriers;
        barriers.reserve(buffers.size());
        for (const auto buffer : buffers) {
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = destination_access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.size = VK_WHOLE_SIZE;
            barriers.push_back(barrier);
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             destination_stage, 0, 0, nullptr,
                             static_cast<std::uint32_t>(barriers.size()),
                             barriers.data(), 0, nullptr);
    }

    void record_rcd(Slot& slot, CfaPattern cfa_pattern) {
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestamp_pool, slot.rcd_timestamp_query_base);
        }
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, rcd_pipeline);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                rcd_pipeline_layout, 0, 1, &slot.rcd_descriptor,
                                0, nullptr);
        std::vector<VkBuffer> written_buffers;
        written_buffers.reserve(8U);
        for (const auto& buffer : slot.camera_rgb) written_buffers.push_back(buffer.handle);
        for (const auto& buffer : slot.scratch) written_buffers.push_back(buffer.handle);
        RcdPushConstants push{config.width, config.height,
                              static_cast<std::uint32_t>(cfa_pattern), 0U};
        for (std::uint32_t pass = 0; pass < 8U; ++pass) {
            push.pass_index = pass;
            vkCmdPushConstants(slot.command, rcd_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(slot.command, (config.width + 15U) / 16U,
                          (config.height + 15U) / 16U, 1);
            shader_buffer_barrier(slot.command, written_buffers);
        }
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestamp_pool, slot.rcd_timestamp_query_base + 1U);
        }
    }

    RawDemosaicF32 calibrate(const RawMosaicU16& input,
                             const NormalizedCameraMetadata& metadata) {
        if (!config.enable_test_readback) {
            throw Error(ErrorCode::invalid_argument,
                        "RAW calibration test readback is disabled for production resources");
        }
        input.validate();
        metadata.validate_for_raw();
        if (input.width != config.width || input.height != config.height ||
            input.width != metadata.width || input.height != metadata.height ||
            input.cfa != metadata.cfa) {
            throw Error(ErrorCode::invalid_argument,
                        "RAW calibration input, metadata and Vulkan resources disagree");
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for RAW calibration slot");
        require_raw_vulkan(vkResetFences(device, 1, &slot.fence),
                           "reset RAW calibration slot");
        upload_raw(slot, input);

        const auto push = calibration_parameters(metadata);
        require_raw_vulkan(vkResetCommandBuffer(slot.command, 0),
                           "reset RAW calibration command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_raw_vulkan(vkBeginCommandBuffer(slot.command, &begin),
                           "begin RAW calibration command buffer");
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(slot.command, timestamp_pool, slot.timestamp_query_base, 2);
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestamp_pool, slot.timestamp_query_base);
        }
        record_calibration(slot, push);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = slot.calibrated.handle;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
        VkBufferCopy copy{0, 0, float_plane_bytes};
        vkCmdCopyBuffer(slot.command, slot.calibrated.handle,
                        slot.calibrated_readback.handle, 1, &copy);
        require_raw_vulkan(vkEndCommandBuffer(slot.command),
                           "end RAW calibration command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        lock_queue(*vulkan_device, hw_device, queue_family);
        const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
        unlock_queue(*vulkan_device, hw_device, queue_family);
        require_raw_vulkan(submit_result, "submit RAW calibration");
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for RAW calibration");
        update_calibration_timestamp(slot);

        RawDemosaicF32 output{config.width, config.height, input.cfa,
                              std::vector<float>(pixels)};
        void* mapped = nullptr;
        require_raw_vulkan(vkMapMemory(device, slot.calibrated_readback.memory, 0,
                                       float_plane_bytes, 0, &mapped),
                           "map RAW calibration readback");
        std::memcpy(output.pixels.data(), mapped, float_plane_bytes);
        vkUnmapMemory(device, slot.calibrated_readback.memory);
        output.validate();
        counters.test_upload_bytes += raw_bytes;
        counters.test_readback_bytes += float_plane_bytes;
        return output;
    }

    CameraRgbF32 demosaic_rcd(const RawMosaicU16& input,
                              const NormalizedCameraMetadata& metadata) {
        if (!config.enable_test_readback) {
            throw Error(ErrorCode::invalid_argument,
                        "RCD test readback is disabled for production resources");
        }
        input.validate();
        metadata.validate_for_raw();
        if (config.width < 32U || config.height < 32U) {
            throw Error(ErrorCode::invalid_argument,
                        "precise RCD requires at least a 32x32 frame");
        }
        if (input.width != config.width || input.height != config.height ||
            input.width != metadata.width || input.height != metadata.height ||
            input.cfa != metadata.cfa) {
            throw Error(ErrorCode::invalid_argument,
                        "RCD input, metadata and Vulkan resources disagree");
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for RCD test slot");
        require_raw_vulkan(vkResetFences(device, 1, &slot.fence),
                           "reset RCD test slot");
        upload_raw(slot, input);
        require_raw_vulkan(vkResetCommandBuffer(slot.command, 0),
                           "reset RCD command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_raw_vulkan(vkBeginCommandBuffer(slot.command, &begin),
                           "begin RCD command buffer");
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(slot.command, timestamp_pool,
                                slot.timestamp_query_base, 4);
        }
        const auto calibration_push = calibration_parameters(metadata);
        record_calibration(slot, calibration_push);
        shader_buffer_barrier(slot.command, {slot.calibrated.handle},
                              VK_ACCESS_SHADER_READ_BIT);
        record_rcd(slot, input.cfa);
        std::vector<VkBuffer> camera_buffers;
        for (const auto& buffer : slot.camera_rgb) camera_buffers.push_back(buffer.handle);
        shader_buffer_barrier(slot.command, camera_buffers,
                              VK_ACCESS_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            VkBufferCopy copy{0, 0, float_plane_bytes};
            vkCmdCopyBuffer(slot.command, slot.camera_rgb[channel].handle,
                            slot.camera_readback[channel].handle, 1, &copy);
        }
        require_raw_vulkan(vkEndCommandBuffer(slot.command), "end RCD command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        lock_queue(*vulkan_device, hw_device, queue_family);
        const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
        unlock_queue(*vulkan_device, hw_device, queue_family);
        require_raw_vulkan(submit_result, "submit RCD test");
        require_raw_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
                           "wait for RCD test");
        update_calibration_timestamp(slot);
        update_rcd_timestamp(slot);
        CameraRgbF32 output{config.width, config.height, {}};
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            output.planes[channel].resize(pixels);
            void* mapped = nullptr;
            require_raw_vulkan(vkMapMemory(device, slot.camera_readback[channel].memory,
                                           0, float_plane_bytes, 0, &mapped),
                               "map RCD Camera RGB readback");
            std::memcpy(output.planes[channel].data(), mapped, float_plane_bytes);
            vkUnmapMemory(device, slot.camera_readback[channel].memory);
        }
        output.validate();
        counters.test_upload_bytes += raw_bytes;
        counters.test_readback_bytes += float_plane_bytes * 3U;
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
            destroy_buffer(slot.calibrated_readback);
            for (auto& buffer : slot.camera_readback) destroy_buffer(buffer);
            for (auto& buffer : slot.scratch) destroy_buffer(buffer);
            for (auto& buffer : slot.camera_rgb) destroy_buffer(buffer);
            destroy_buffer(slot.calibrated);
            destroy_buffer(slot.upload);
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, allocator);
            command_pool = VK_NULL_HANDLE;
        }
        if (timestamp_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, timestamp_pool, allocator);
            timestamp_pool = VK_NULL_HANDLE;
        }
        if (calibration_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, calibration_pipeline, allocator);
            calibration_pipeline = VK_NULL_HANDLE;
        }
        if (calibration_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, calibration_pipeline_layout, allocator);
            calibration_pipeline_layout = VK_NULL_HANDLE;
        }
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, allocator);
            descriptor_pool = VK_NULL_HANDLE;
        }
        if (calibration_descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, calibration_descriptor_layout, allocator);
            calibration_descriptor_layout = VK_NULL_HANDLE;
        }
        if (rcd_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, rcd_pipeline, allocator);
            rcd_pipeline = VK_NULL_HANDLE;
        }
        if (rcd_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, rcd_pipeline_layout, allocator);
            rcd_pipeline_layout = VK_NULL_HANDLE;
        }
        if (rcd_descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, rcd_descriptor_pool, allocator);
            rcd_descriptor_pool = VK_NULL_HANDLE;
        }
        if (rcd_descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, rcd_descriptor_layout, allocator);
            rcd_descriptor_layout = VK_NULL_HANDLE;
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
    VkDescriptorSetLayout calibration_descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout calibration_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline calibration_pipeline{VK_NULL_HANDLE};
    VkDescriptorSetLayout rcd_descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool rcd_descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout rcd_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline rcd_pipeline{VK_NULL_HANDLE};
    VkQueryPool timestamp_pool{VK_NULL_HANDLE};
    float timestamp_period_ns{};
    std::uint32_t timestamp_valid_bits{};
    std::vector<double> calibration_samples;
    std::vector<double> rcd_samples;
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

RawDemosaicF32 VulkanRawPipelineResources::calibrate_for_test(
    const RawMosaicU16& input, const NormalizedCameraMetadata& metadata) {
    return impl_->calibrate(input, metadata);
}

CameraRgbF32 VulkanRawPipelineResources::demosaic_rcd_for_test(
    const RawMosaicU16& input, const NormalizedCameraMetadata& metadata) {
    return impl_->demosaic_rcd(input, metadata);
}

VulkanRawPipelineResourceTelemetry VulkanRawPipelineResources::telemetry() const noexcept {
    return impl_->counters;
}

} // namespace mcraw
