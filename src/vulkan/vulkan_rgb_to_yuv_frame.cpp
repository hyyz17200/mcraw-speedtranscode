#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <mcraw/core/error.hpp>
#include <mcraw/processing/log_curve.hpp>
#include <mcraw/vulkan/camera_to_dwg_spv.hpp>
#include <mcraw/vulkan/davinci_intermediate_spv.hpp>
#include <mcraw/vulkan/rgb_to_yuv_422_image_spv.hpp>
#include <mcraw/vulkan/sharpen_target_linear_spv.hpp>

namespace mcraw {
namespace {

void require_frame_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw Error(result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost
                                                   : ErrorCode::processing_failed,
                    std::string(operation) + " failed with VkResult " +
                    std::to_string(static_cast<int>(result)));
    }
}

std::size_t frame_checked_bytes(std::size_t values, std::size_t value_size) {
    if (values > std::numeric_limits<std::size_t>::max() / value_size) {
        throw Error(ErrorCode::invalid_argument, "Vulkan frame-writer buffer size overflow");
    }
    return values * value_size;
}

struct FramePushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t quality_filter{};
    std::uint32_t enable_dither{};
    std::uint64_t frame_index{};
};
static_assert(sizeof(FramePushConstants) == 24U);

struct alignas(16) CameraToDwgPushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    float exposure_scale{};
    std::uint32_t reserved{};
    std::array<float, 4> matrix_row_0{};
    std::array<float, 4> matrix_row_1{};
    std::array<float, 4> matrix_row_2{};
};
static_assert(sizeof(CameraToDwgPushConstants) == 64U);
static_assert(offsetof(CameraToDwgPushConstants, matrix_row_0) == 16U);
static_assert(offsetof(CameraToDwgPushConstants, matrix_row_1) == 32U);
static_assert(offsetof(CameraToDwgPushConstants, matrix_row_2) == 48U);

struct SharpenPushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    float amount{};
    float threshold{};
};
static_assert(sizeof(SharpenPushConstants) == 16U);

struct DiPushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t negative_policy{};
    std::uint32_t entries_per_segment{};
};
static_assert(sizeof(DiPushConstants) == 16U);
static_assert(static_cast<std::uint32_t>(NegativePolicy::preserve_by_curve) == 0U);
static_assert(static_cast<std::uint32_t>(NegativePolicy::clamp_zero) == 1U);
static_assert(static_cast<std::uint32_t>(NegativePolicy::error) == 2U);

constexpr std::uint32_t di_status_negative_rejected = 1U;
constexpr std::uint32_t di_status_non_finite = 2U;

std::size_t vk_image_count(const AVVkFrame& frame) {
    std::size_t count = 0;
    while (count < AV_NUM_DATA_POINTERS && frame.img[count] != VK_NULL_HANDLE) ++count;
    return count;
}

VkImageAspectFlags plane_aspect(std::size_t plane, std::size_t image_count) {
    if (image_count > 1U) return VK_IMAGE_ASPECT_COLOR_BIT;
    switch (plane) {
    case 0: return VK_IMAGE_ASPECT_PLANE_0_BIT;
    case 1: return VK_IMAGE_ASPECT_PLANE_1_BIT;
    case 2: return VK_IMAGE_ASPECT_PLANE_2_BIT;
    default: return 0;
    }
}

double timestamp_percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double index = p * static_cast<double>(sorted.size() - 1U);
    const auto lo = static_cast<std::size_t>(std::floor(index));
    const auto hi = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lo);
    return sorted[lo] * (1.0 - fraction) + sorted[hi] * fraction;
}

std::uint64_t timestamp_delta(std::uint64_t start,
                              std::uint64_t end,
                              std::uint32_t valid_bits) {
    if (valid_bits >= 64U) return end - start;
    const auto mask = (std::uint64_t{1} << valid_bits) - 1U;
    return (end - start) & mask;
}

// FFmpeg 8 still exposes these compatibility callbacks for devices that were
// not created with VK_KHR_internally_synchronized_queues. Keeping all queue
// submissions behind the owner-provided mutex is safe for either device kind.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
bool has_ffmpeg_queue_lock(const AVVulkanDeviceContext& device) {
    return device.lock_queue != nullptr && device.unlock_queue != nullptr;
}

void lock_ffmpeg_queue(AVVulkanDeviceContext& device,
                       AVHWDeviceContext* owner,
                       std::uint32_t family) {
    device.lock_queue(owner, family, 0);
}

void unlock_ffmpeg_queue(AVVulkanDeviceContext& device,
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

class VulkanRgbToYuvFrameWriter::Impl {
public:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    struct Slot {
        std::array<Buffer, 3> input_buffers;
        std::array<std::array<Buffer, 3>, 2> intermediate;
        Buffer control_status;
        VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
        VkDescriptorSet camera_yuv_descriptor_set{VK_NULL_HANDLE};
        VkDescriptorSet color_descriptor{VK_NULL_HANDLE};
        VkDescriptorSet sharpen_descriptor{VK_NULL_HANDLE};
        VkDescriptorSet di_descriptor{VK_NULL_HANDLE};
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        std::array<VkImageView, 3> views{};
        std::uint32_t timestamp_query_base{};
        std::uint32_t sharpen_timestamp_query_base{};
        std::uint32_t di_timestamp_query_base{};
        std::uint32_t yuv_timestamp_query_base{};
        bool in_flight{};
        bool camera_chain{};
        std::chrono::steady_clock::time_point submitted_at{};
    };

    Impl(VulkanRuntime& runtime_owner,
         FfmpegVulkanFrameContext& frame_owner,
         VulkanRgbToYuvConfig requested)
        : runtime(runtime_owner), frames(frame_owner), config(requested) {
        if (config.width == 0U || config.height == 0U || (config.width & 1U) != 0U ||
            config.width != static_cast<std::uint32_t>(frames.width()) ||
            config.height != static_cast<std::uint32_t>(frames.height())) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan frame writer requires matching non-zero dimensions and even width");
        }
        if (config.precision != GpuPrecision::fp32) {
            throw Error(ErrorCode::unsupported_format,
                        "FP16 direct frame writing is disabled until validated");
        }
        config.slots = std::clamp<std::size_t>(config.slots, 1U, 64U);
        slots.resize(config.slots);
        counters.slot_count = config.slots;
        if ((frames.image_usage() & VK_IMAGE_USAGE_STORAGE_BIT) == 0U) {
            throw Error(ErrorCode::unsupported_format,
                        "FFmpeg Vulkan frame pool does not allow storage-image writes");
        }
        hw_device = runtime.ffmpeg_device_context();
        vulkan_device = runtime.ffmpeg_vulkan_context();
        if (hw_device == nullptr || vulkan_device == nullptr ||
            vulkan_device->act_dev == VK_NULL_HANDLE ||
            vulkan_device->phys_dev == VK_NULL_HANDLE ||
            !has_ffmpeg_queue_lock(*vulkan_device)) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg-owned Vulkan device is unavailable to frame writer");
        }
        device = vulkan_device->act_dev;
        physical_device = vulkan_device->phys_dev;
        allocator = vulkan_device->alloc;
        queue_family = runtime.compute_queue_family();
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (queue == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "cannot obtain Vulkan compute queue for direct frame writing");
        }
        try {
            create_input_buffers();
            create_descriptor_resources();
            create_pipeline();
            create_camera_pipelines();
            create_timestamp_queries();
            create_commands();
            upload_di_lut();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    std::uint32_t memory_type(std::uint32_t mask, VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if ((mask & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        throw Error(ErrorCode::unsupported_format,
                    "GPU has no memory type required by the frame writer");
    }

    Buffer create_buffer(VkDeviceSize size,
                         VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VkMemoryPropertyFlags properties =
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
        Buffer result;
        result.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_frame_vulkan(vkCreateBuffer(device, &info, allocator, &result.buffer),
                             "create direct-frame RGB buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits,
                                                 properties);
        try {
            require_frame_vulkan(vkAllocateMemory(device, &allocation, allocator,
                                                  &result.memory),
                                 "allocate direct-frame RGB memory");
            require_frame_vulkan(vkBindBufferMemory(device, result.buffer, result.memory, 0),
                                 "bind direct-frame RGB memory");
        } catch (...) {
            if (result.memory != VK_NULL_HANDLE) vkFreeMemory(device, result.memory, allocator);
            vkDestroyBuffer(device, result.buffer, allocator);
            throw;
        }
        return result;
    }

    void create_input_buffers() {
        const auto pixels = static_cast<std::size_t>(config.width) * config.height;
        const auto bytes = frame_checked_bytes(pixels, sizeof(float));
        for (auto& slot : slots) {
            for (auto& buffer : slot.input_buffers) buffer = create_buffer(bytes);
            for (auto& set : slot.intermediate) {
                for (auto& buffer : set) {
                    buffer = create_buffer(
                        bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                }
            }
            slot.control_status = create_buffer(
                sizeof(std::uint32_t),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        const auto lut_bytes = static_cast<VkDeviceSize>(
            (di_curve.low_segment().size() + di_curve.high_segment().size()) *
            sizeof(float));
        di_lut = create_buffer(lut_bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    void create_descriptor_resources() {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (std::uint32_t index = 0; index < 3U; ++index) {
            bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        for (std::uint32_t index = 3U; index < 6U; ++index) {
            bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        require_frame_vulkan(vkCreateDescriptorSetLayout(device, &info, allocator,
                                                         &descriptor_layout),
                             "create direct-frame descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 6> buffer_bindings{};
        for (std::uint32_t index = 0; index < buffer_bindings.size(); ++index) {
            buffer_bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                      VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo buffer_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        buffer_layout_info.bindingCount =
            static_cast<std::uint32_t>(buffer_bindings.size());
        buffer_layout_info.pBindings = buffer_bindings.data();
        require_frame_vulkan(vkCreateDescriptorSetLayout(
                                 device, &buffer_layout_info, allocator,
                                 &camera_descriptor_layout),
                             "create Camera RGB pass descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 8> di_bindings{};
        for (std::uint32_t index = 0; index < di_bindings.size(); ++index) {
            di_bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                  VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo di_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di_layout_info.bindingCount = static_cast<std::uint32_t>(di_bindings.size());
        di_layout_info.pBindings = di_bindings.data();
        require_frame_vulkan(vkCreateDescriptorSetLayout(
                                 device, &di_layout_info, allocator,
                                 &di_descriptor_layout),
                             "create resident DI descriptor layout");

        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             static_cast<std::uint32_t>(config.slots * 26U)},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             static_cast<std::uint32_t>(config.slots * 6U)}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = static_cast<std::uint32_t>(config.slots * 5U);
        pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        require_frame_vulkan(vkCreateDescriptorPool(device, &pool, allocator,
                                                    &descriptor_pool),
                             "create direct-frame descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts;
        layouts.reserve(config.slots * 5U);
        layouts.insert(layouts.end(), config.slots * 2U, descriptor_layout);
        layouts.insert(layouts.end(), config.slots * 2U,
                       camera_descriptor_layout);
        layouts.insert(layouts.end(), config.slots, di_descriptor_layout);
        std::vector<VkDescriptorSet> sets(layouts.size());
        VkDescriptorSetAllocateInfo allocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptor_pool;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(sets.size());
        allocation.pSetLayouts = layouts.data();
        require_frame_vulkan(vkAllocateDescriptorSets(device, &allocation, sets.data()),
                             "allocate direct-frame descriptor sets");
        for (std::size_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            slot.descriptor_set = sets[index];
            slot.camera_yuv_descriptor_set = sets[slots.size() + index];
            slot.color_descriptor = sets[slots.size() * 2U + index];
            slot.sharpen_descriptor = sets[slots.size() * 3U + index];
            slot.di_descriptor = sets[slots.size() * 4U + index];

            std::array<VkDescriptorBufferInfo, 20> buffer_info{};
            std::array<VkWriteDescriptorSet, 20> writes{};
            for (std::size_t plane = 0; plane < 3U; ++plane) {
                buffer_info[plane] = {slot.input_buffers[plane].buffer, 0,
                                      slot.input_buffers[plane].size};
                buffer_info[plane + 3U] = {slot.intermediate[0][plane].buffer, 0,
                                           slot.intermediate[0][plane].size};
                buffer_info[plane + 6U] = {slot.intermediate[0][plane].buffer, 0,
                                           slot.intermediate[0][plane].size};
                buffer_info[plane + 9U] = {slot.intermediate[1][plane].buffer, 0,
                                           slot.intermediate[1][plane].size};
                buffer_info[plane + 12U] = {slot.intermediate[1][plane].buffer, 0,
                                            slot.intermediate[1][plane].size};
                buffer_info[plane + 15U] = {slot.intermediate[0][plane].buffer, 0,
                                            slot.intermediate[0][plane].size};
            }
            buffer_info[18] = {di_lut.buffer, 0, di_lut.size};
            buffer_info[19] = {slot.control_status.buffer, 0,
                               slot.control_status.size};
            for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                auto& write = writes[binding];
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                if (binding < 6U) {
                    write.dstSet = slot.color_descriptor;
                    write.dstBinding = binding;
                } else if (binding < 12U) {
                    write.dstSet = slot.sharpen_descriptor;
                    write.dstBinding = binding - 6U;
                } else {
                    write.dstSet = slot.di_descriptor;
                    write.dstBinding = binding - 12U;
                }
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffer_info[binding];
            }
            vkUpdateDescriptorSets(device,
                                   static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    void create_pipeline() {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(FramePushConstants);
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &descriptor_layout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        require_frame_vulkan(vkCreatePipelineLayout(device, &layout, allocator,
                                                    &pipeline_layout),
                             "create direct-frame pipeline layout");
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = generated::rgb_to_yuv_422_image_spv.size() *
                               sizeof(std::uint32_t);
        module_info.pCode = generated::rgb_to_yuv_422_image_spv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        require_frame_vulkan(vkCreateShaderModule(device, &module_info, allocator, &module),
                             "create direct-frame shader module");
        VkComputePipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.layout = pipeline_layout;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                     &pipeline_info, allocator, &pipeline);
        vkDestroyShaderModule(device, module, allocator);
        require_frame_vulkan(result, "create direct-frame compute pipeline");
    }

    void create_camera_pipeline(const std::uint32_t* code,
                                std::size_t words,
                                VkDescriptorSetLayout set_layout,
                                std::uint32_t push_bytes,
                                VkPipelineLayout& output_layout,
                                VkPipeline& output_pipeline,
                                const char* operation) {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = push_bytes;
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &set_layout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        require_frame_vulkan(vkCreatePipelineLayout(
                                 device, &layout, allocator, &output_layout),
                             operation);
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = words * sizeof(std::uint32_t);
        module_info.pCode = code;
        VkShaderModule module{VK_NULL_HANDLE};
        require_frame_vulkan(vkCreateShaderModule(
                                 device, &module_info, allocator, &module),
                             operation);
        VkComputePipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.layout = output_layout;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        const auto result = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &pipeline_info, allocator,
            &output_pipeline);
        vkDestroyShaderModule(device, module, allocator);
        require_frame_vulkan(result, operation);
    }

    void create_camera_pipelines() {
        create_camera_pipeline(
            generated::camera_to_dwg_spv.data(),
            generated::camera_to_dwg_spv.size(), camera_descriptor_layout,
            sizeof(CameraToDwgPushConstants), color_pipeline_layout,
            color_pipeline, "create resident Camera RGB color pipeline");
        create_camera_pipeline(
            generated::sharpen_target_linear_spv.data(),
            generated::sharpen_target_linear_spv.size(),
            camera_descriptor_layout, sizeof(SharpenPushConstants),
            sharpen_pipeline_layout, sharpen_pipeline,
            "create resident TargetLinear sharpening pipeline");
        create_camera_pipeline(
            generated::davinci_intermediate_spv.data(),
            generated::davinci_intermediate_spv.size(), di_descriptor_layout,
            sizeof(DiPushConstants), di_pipeline_layout, di_pipeline,
            "create resident DaVinci Intermediate pipeline");
    }

    void create_commands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family;
        require_frame_vulkan(vkCreateCommandPool(device, &pool, allocator, &command_pool),
                             "create direct-frame command pool");
        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = static_cast<std::uint32_t>(config.slots);
        std::vector<VkCommandBuffer> commands(config.slots);
        require_frame_vulkan(vkAllocateCommandBuffers(device, &allocation, commands.data()),
                             "allocate direct-frame command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].command_buffer = commands[index];
            slots[index].timestamp_query_base = static_cast<std::uint32_t>(index * 8U);
            slots[index].sharpen_timestamp_query_base =
                slots[index].timestamp_query_base + 2U;
            slots[index].di_timestamp_query_base =
                slots[index].timestamp_query_base + 4U;
            slots[index].yuv_timestamp_query_base =
                slots[index].timestamp_query_base + 6U;
            require_frame_vulkan(vkCreateFence(device, &fence_info, allocator,
                                               &slots[index].fence),
                                 "create direct-frame fence");
        }
    }

    void create_timestamp_queries() {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count,
                                                 families.data());
        if (queue_family >= families.size() ||
            families[queue_family].timestampValidBits == 0U) {
            counters.gpu_timestamps_supported = false;
            return;
        }
        timestamp_valid_bits = families[queue_family].timestampValidBits;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        timestamp_period_ns = properties.limits.timestampPeriod;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = static_cast<std::uint32_t>(slots.size() * 8U);
        require_frame_vulkan(vkCreateQueryPool(device, &info, allocator, &timestamp_query_pool),
                             "create direct-frame timestamp query pool");
        counters.gpu_timestamps_supported = true;
    }

    void upload_di_lut() {
        std::vector<float> values;
        values.reserve(di_curve.low_segment().size() +
                       di_curve.high_segment().size());
        values.insert(values.end(), di_curve.low_segment().begin(),
                      di_curve.low_segment().end());
        values.insert(values.end(), di_curve.high_segment().begin(),
                      di_curve.high_segment().end());
        const auto bytes = static_cast<VkDeviceSize>(values.size() * sizeof(float));
        auto staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        try {
            void* mapped = nullptr;
            require_frame_vulkan(vkMapMemory(device, staging.memory, 0,
                                             staging.size, 0, &mapped),
                                 "map resident DI LUT staging");
            std::memcpy(mapped, values.data(), static_cast<std::size_t>(bytes));
            vkUnmapMemory(device, staging.memory);
            auto& slot = slots.front();
            require_frame_vulkan(vkResetFences(device, 1, &slot.fence),
                                 "reset resident DI LUT fence");
            require_frame_vulkan(vkResetCommandBuffer(slot.command_buffer, 0),
                                 "reset resident DI LUT command buffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require_frame_vulkan(vkBeginCommandBuffer(slot.command_buffer, &begin),
                                 "begin resident DI LUT upload");
            VkBufferCopy copy{0, 0, bytes};
            vkCmdCopyBuffer(slot.command_buffer, staging.buffer,
                            di_lut.buffer, 1, &copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = di_lut.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(slot.command_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 1, &barrier, 0, nullptr);
            require_frame_vulkan(vkEndCommandBuffer(slot.command_buffer),
                                 "end resident DI LUT upload");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &slot.command_buffer;
            lock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
            unlock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            require_frame_vulkan(submit_result, "submit resident DI LUT upload");
            require_frame_vulkan(vkWaitForFences(device, 1, &slot.fence,
                                                 VK_TRUE, UINT64_MAX),
                                 "wait for resident DI LUT upload");
        } catch (...) {
            destroy_buffer(staging);
            throw;
        }
        destroy_buffer(staging);
    }

    void upload(Slot& slot, std::size_t plane, const std::vector<float>& values) {
        const auto bytes = frame_checked_bytes(values.size(), sizeof(float));
        if (bytes != slot.input_buffers[plane].size) {
            throw Error(ErrorCode::invalid_argument,
                        "RGB plane size changed during direct frame write");
        }
        void* mapped = nullptr;
        require_frame_vulkan(vkMapMemory(device, slot.input_buffers[plane].memory, 0,
                                         slot.input_buffers[plane].size, 0, &mapped),
                             "map direct-frame RGB input");
        std::memcpy(mapped, values.data(), bytes);
        vkUnmapMemory(device, slot.input_buffers[plane].memory);
        counters.rgb_upload_bytes += bytes;
    }

    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.buffer, allocator);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, allocator);
        }
        buffer = {};
    }

    std::array<VkImageView, 3> create_views(const AVVkFrame& frame,
                                            std::size_t image_count) {
        std::array<VkImageView, 3> result{};
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        try {
            for (std::size_t plane = 0; plane < result.size(); ++plane) {
                VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                info.pNext = &usage;
                info.image = frame.img[std::min(plane, image_count - 1U)];
                info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                info.format = VK_FORMAT_R16_UINT;
                info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY};
                info.subresourceRange.aspectMask = plane_aspect(plane, image_count);
                info.subresourceRange.levelCount = 1;
                info.subresourceRange.layerCount = 1;
                require_frame_vulkan(vkCreateImageView(device, &info, allocator,
                                                       &result[plane]),
                                     "create encoder-frame plane view");
            }
        } catch (...) {
            for (auto view : result) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, allocator);
            }
            throw;
        }
        return result;
    }

    VkDescriptorSet update_descriptors(Slot& slot, bool camera_chain) {
        std::array<VkDescriptorBufferInfo, 3> buffer_info{};
        std::array<VkDescriptorImageInfo, 3> image_info{};
        std::array<VkWriteDescriptorSet, 6> writes{};
        const auto descriptor = camera_chain ? slot.camera_yuv_descriptor_set
                                             : slot.descriptor_set;
        for (std::uint32_t index = 0; index < 3U; ++index) {
            const auto& input = camera_chain
                ? slot.intermediate[0][index] : slot.input_buffers[index];
            buffer_info[index] = {input.buffer, 0, input.size};
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptor;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &buffer_info[index];
            image_info[index] = {VK_NULL_HANDLE, slot.views[index], VK_IMAGE_LAYOUT_GENERAL};
            writes[index + 3U].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index + 3U].dstSet = descriptor;
            writes[index + 3U].dstBinding = index + 3U;
            writes[index + 3U].descriptorCount = 1;
            writes[index + 3U].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[index + 3U].pImageInfo = &image_info[index];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return descriptor;
    }

    double read_timestamp(std::uint32_t base, const char* operation) {
        std::array<std::uint64_t, 2> values{};
        require_frame_vulkan(vkGetQueryPoolResults(
            device, timestamp_query_pool, base, 2,
            sizeof(values), values.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), operation);
        const auto ticks = timestamp_delta(values[0], values[1],
                                           timestamp_valid_bits);
        return static_cast<double>(ticks) *
               static_cast<double>(timestamp_period_ns) / 1.0e6;
    }

    static void append_timestamp(double milliseconds,
                                 std::vector<double>& samples,
                                 std::uint64_t& sample_count,
                                 double& total,
                                 double& mean,
                                 double& minimum,
                                 double& maximum) {
        samples.push_back(milliseconds);
        sample_count = samples.size();
        total += milliseconds;
        mean = total / static_cast<double>(sample_count);
        if (sample_count == 1U) {
            minimum = milliseconds;
            maximum = milliseconds;
        } else {
            minimum = std::min(minimum, milliseconds);
            maximum = std::max(maximum, milliseconds);
        }
    }

    std::uint32_t read_control_status(const Slot& slot) {
        std::uint32_t status = 0;
        void* mapped = nullptr;
        require_frame_vulkan(vkMapMemory(device, slot.control_status.memory,
                                         0, sizeof(status), 0, &mapped),
                             "map resident DI control status");
        std::memcpy(&status, mapped, sizeof(status));
        vkUnmapMemory(device, slot.control_status.memory);
        counters.control_status_read_bytes += sizeof(status);
        return status;
    }

    void recycle_slot(Slot& slot, bool count_backpressure) {
        if (!slot.in_flight) return;
        const auto wait_start = std::chrono::steady_clock::now();
        const auto status = vkGetFenceStatus(device, slot.fence);
        if (status == VK_NOT_READY) {
            if (count_backpressure) ++counters.backpressure_waits;
            require_frame_vulkan(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                                 UINT64_MAX),
                                 "wait for direct-frame slot");
            if (count_backpressure) {
                counters.backpressure_wait_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - wait_start).count();
            }
        } else {
            require_frame_vulkan(status, "query direct-frame slot fence");
        }
        counters.last_dispatch_wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - slot.submitted_at).count();
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            const double milliseconds = read_timestamp(
                slot.yuv_timestamp_query_base,
                "read direct-frame RGB-to-YUV timestamps");
            append_timestamp(milliseconds, gpu_timestamp_ms,
                             counters.gpu_timestamp_samples,
                             counters.gpu_total_ms, counters.gpu_mean_ms,
                             counters.gpu_min_ms, counters.gpu_max_ms);
            counters.last_gpu_dispatch_ms = milliseconds;
            if (slot.camera_chain) {
                append_timestamp(
                    read_timestamp(slot.timestamp_query_base,
                                   "read resident color timestamps"),
                    color_timestamp_ms, counters.camera_to_dwg_timestamp_samples,
                    counters.camera_to_dwg_gpu_total_ms,
                    counters.camera_to_dwg_gpu_mean_ms,
                    counters.camera_to_dwg_gpu_min_ms,
                    counters.camera_to_dwg_gpu_max_ms);
                append_timestamp(
                    read_timestamp(slot.sharpen_timestamp_query_base,
                                   "read resident sharpening timestamps"),
                    sharpen_timestamp_ms,
                    counters.capture_sharpening_timestamp_samples,
                    counters.capture_sharpening_gpu_total_ms,
                    counters.capture_sharpening_gpu_mean_ms,
                    counters.capture_sharpening_gpu_min_ms,
                    counters.capture_sharpening_gpu_max_ms);
                append_timestamp(
                    read_timestamp(slot.di_timestamp_query_base,
                                   "read resident DI timestamps"),
                    di_timestamp_ms,
                    counters.davinci_intermediate_timestamp_samples,
                    counters.davinci_intermediate_gpu_total_ms,
                    counters.davinci_intermediate_gpu_mean_ms,
                    counters.davinci_intermediate_gpu_min_ms,
                    counters.davinci_intermediate_gpu_max_ms);
            }
        }
        const auto control_status = slot.camera_chain
            ? read_control_status(slot) : 0U;
        for (auto& view : slot.views) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, allocator);
            view = VK_NULL_HANDLE;
        }
        slot.in_flight = false;
        slot.camera_chain = false;
        if (counters.in_flight > 0U) --counters.in_flight;
        if (control_status != 0U) {
            ++counters.control_status_failures;
            if ((control_status & di_status_non_finite) != 0U) {
                throw Error(ErrorCode::processing_failed,
                            "resident DaVinci Intermediate received a non-finite value");
            }
            if ((control_status & di_status_negative_rejected) != 0U) {
                throw Error(ErrorCode::processing_failed,
                            "negative target-linear value rejected by resident policy");
            }
            throw Error(ErrorCode::processing_failed,
                        "resident DaVinci Intermediate reported unknown status");
        }
    }

    VulkanVideoFrame pack_impl(const PlanarRgbF32& input,
                               std::size_t frame_index,
                               FrameMetadata metadata,
                               const Matrix3d* camera_to_target,
                               double exposure_offset_stops,
                               double input_scale,
                               double sharpening_amount,
                               double sharpening_threshold,
                               NegativePolicy negative_policy) {
        input.validate();
        const bool camera_chain = camera_to_target != nullptr;
        if (input.width != config.width || input.height != config.height ||
            metadata.width != static_cast<int>(config.width) ||
            metadata.height != static_cast<int>(config.height)) {
            throw Error(ErrorCode::invalid_argument,
                        "direct Vulkan frame dimensions do not match RGB input");
        }
        if (camera_chain) {
            const auto finite_plane = [](const std::vector<float>& plane) {
                return std::all_of(plane.begin(), plane.end(),
                                   [](float value) { return std::isfinite(value); });
            };
            if (!std::all_of(input.planes.begin(), input.planes.end(), finite_plane) ||
                !std::all_of(camera_to_target->v.begin(), camera_to_target->v.end(),
                             [](double value) { return std::isfinite(value); }) ||
                !std::isfinite(exposure_offset_stops) ||
                !std::isfinite(input_scale) || input_scale <= 0.0 ||
                !std::isfinite(sharpening_amount) || sharpening_amount < 0.0 ||
                !std::isfinite(sharpening_threshold) || sharpening_threshold < 0.0) {
                throw Error(ErrorCode::invalid_argument,
                            "resident Camera RGB inputs must be finite and valid");
            }
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        recycle_slot(slot, true);
        for (std::size_t plane = 0; plane < 3; ++plane) {
            upload(slot, plane, input.planes[plane]);
        }
        const auto allocation_start = std::chrono::steady_clock::now();
        auto output = frames.allocate_frame(metadata);
        const auto allocation_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - allocation_start).count();
        ++counters.frame_allocation_samples;
        counters.frame_allocation_total_ms += allocation_ms;
        counters.frame_allocation_mean_ms = counters.frame_allocation_total_ms /
            static_cast<double>(counters.frame_allocation_samples);
        counters.frame_allocation_max_ms = std::max(
            counters.frame_allocation_max_ms, allocation_ms);
        auto* frame = reinterpret_cast<AVVkFrame*>(output.frame->data[0]);
        auto* hw_frames = reinterpret_cast<AVHWFramesContext*>(
            output.frame->hw_frames_ctx->data);
        auto* vk_frames = static_cast<AVVulkanFramesContext*>(hw_frames->hwctx);
        const auto image_count = vk_image_count(*frame);
        if (image_count == 0U || vk_frames->lock_frame == nullptr ||
            vk_frames->unlock_frame == nullptr) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg Vulkan frame lacks images or frame locks");
        }
        slot.views = create_views(*frame, image_count);
        const auto descriptor_set = update_descriptors(slot, camera_chain);
        bool frame_locked = false;
        bool submitted = false;
        try {
            vk_frames->lock_frame(hw_frames, frame);
            frame_locked = true;
            std::array<VkImageMemoryBarrier, AV_NUM_DATA_POINTERS> barriers{};
            std::array<VkPipelineStageFlags, AV_NUM_DATA_POINTERS> wait_stages{};
            std::array<std::uint64_t, AV_NUM_DATA_POINTERS> wait_values{};
            std::array<std::uint64_t, AV_NUM_DATA_POINTERS> signal_values{};
            for (std::size_t image = 0; image < image_count; ++image) {
                if (frame->queue_family[image] != VK_QUEUE_FAMILY_IGNORED &&
                    frame->queue_family[image] != queue_family) {
                    throw Error(ErrorCode::unsupported_format,
                                "FFmpeg frame requires an unsupported queue-family ownership transfer");
                }
                auto& barrier = barriers[image];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = frame->access[image];
                barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.oldLayout = frame->layout[image];
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = frame->img[image];
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                wait_stages[image] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                wait_values[image] = frame->sem_value[image];
                signal_values[image] = frame->sem_value[image] + 1U;
            }
            require_frame_vulkan(vkResetFences(device, 1, &slot.fence),
                                 "reset direct-frame fence");
            require_frame_vulkan(vkResetCommandBuffer(slot.command_buffer, 0),
                                 "reset direct-frame command buffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require_frame_vulkan(vkBeginCommandBuffer(slot.command_buffer, &begin),
                                 "begin direct-frame command buffer");
            if (timestamp_query_pool != VK_NULL_HANDLE) {
                vkCmdResetQueryPool(slot.command_buffer, timestamp_query_pool,
                                    camera_chain ? slot.timestamp_query_base
                                                 : slot.yuv_timestamp_query_base,
                                    camera_chain ? 8U : 2U);
            }
            if (camera_chain) {
                vkCmdFillBuffer(slot.command_buffer,
                                slot.control_status.buffer, 0,
                                sizeof(std::uint32_t), 0U);
                VkBufferMemoryBarrier status_reset{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                status_reset.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                status_reset.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT;
                status_reset.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                status_reset.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                status_reset.buffer = slot.control_status.buffer;
                status_reset.offset = 0;
                status_reset.size = sizeof(std::uint32_t);
                vkCmdPipelineBarrier(slot.command_buffer,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr, 1, &status_reset, 0, nullptr);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        timestamp_query_pool,
                                        slot.timestamp_query_base);
                }
                const double exposure = std::exp2(exposure_offset_stops) * input_scale;
                if (!std::isfinite(exposure)) {
                    throw Error(ErrorCode::invalid_argument,
                                "resident Camera RGB exposure scale is not finite");
                }
                CameraToDwgPushConstants color_push;
                color_push.width = config.width;
                color_push.height = config.height;
                color_push.exposure_scale = static_cast<float>(exposure);
                for (std::size_t column = 0; column < 3U; ++column) {
                    color_push.matrix_row_0[column] = static_cast<float>(
                        camera_to_target->v[column]);
                    color_push.matrix_row_1[column] = static_cast<float>(
                        camera_to_target->v[3U + column]);
                    color_push.matrix_row_2[column] = static_cast<float>(
                        camera_to_target->v[6U + column]);
                }
                vkCmdBindPipeline(slot.command_buffer,
                                  VK_PIPELINE_BIND_POINT_COMPUTE,
                                  color_pipeline);
                vkCmdBindDescriptorSets(slot.command_buffer,
                                        VK_PIPELINE_BIND_POINT_COMPUTE,
                                        color_pipeline_layout, 0, 1,
                                        &slot.color_descriptor, 0, nullptr);
                vkCmdPushConstants(slot.command_buffer, color_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(color_push), &color_push);
                const auto pixels = static_cast<std::uint64_t>(config.width) *
                                    config.height;
                vkCmdDispatch(slot.command_buffer,
                              static_cast<std::uint32_t>((pixels + 255U) / 256U),
                              1, 1);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        timestamp_query_pool,
                                        slot.timestamp_query_base + 1U);
                }
                std::array<VkBufferMemoryBarrier, 3> color_barriers{};
                for (std::size_t plane = 0; plane < 3U; ++plane) {
                    auto& barrier = color_barriers[plane];
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer = slot.intermediate[0][plane].buffer;
                    barrier.offset = 0;
                    barrier.size = VK_WHOLE_SIZE;
                }
                vkCmdPipelineBarrier(slot.command_buffer,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr,
                                     static_cast<std::uint32_t>(color_barriers.size()),
                                     color_barriers.data(), 0, nullptr);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        timestamp_query_pool,
                                        slot.sharpen_timestamp_query_base);
                }
                const SharpenPushConstants sharpen_push{
                    config.width, config.height,
                    static_cast<float>(sharpening_amount),
                    static_cast<float>(sharpening_threshold)};
                vkCmdBindPipeline(slot.command_buffer,
                                  VK_PIPELINE_BIND_POINT_COMPUTE,
                                  sharpen_pipeline);
                vkCmdBindDescriptorSets(slot.command_buffer,
                                        VK_PIPELINE_BIND_POINT_COMPUTE,
                                        sharpen_pipeline_layout, 0, 1,
                                        &slot.sharpen_descriptor, 0, nullptr);
                vkCmdPushConstants(slot.command_buffer, sharpen_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(sharpen_push), &sharpen_push);
                vkCmdDispatch(slot.command_buffer,
                              (config.width + 15U) / 16U,
                              (config.height + 15U) / 16U, 1);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        timestamp_query_pool,
                                        slot.sharpen_timestamp_query_base + 1U);
                }
                std::array<VkBufferMemoryBarrier, 6> di_barriers{};
                for (std::size_t plane = 0; plane < 3U; ++plane) {
                    auto& input_barrier = di_barriers[plane];
                    input_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    input_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    input_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    input_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    input_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    input_barrier.buffer = slot.intermediate[1][plane].buffer;
                    input_barrier.offset = 0;
                    input_barrier.size = VK_WHOLE_SIZE;
                    auto& output_barrier = di_barriers[plane + 3U];
                    output_barrier = input_barrier;
                    output_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                   VK_ACCESS_SHADER_WRITE_BIT;
                    output_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    output_barrier.buffer = slot.intermediate[0][plane].buffer;
                }
                vkCmdPipelineBarrier(slot.command_buffer,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr,
                                     static_cast<std::uint32_t>(di_barriers.size()),
                                     di_barriers.data(), 0, nullptr);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        timestamp_query_pool,
                                        slot.di_timestamp_query_base);
                }
                const DiPushConstants di_push{
                    config.width, config.height,
                    static_cast<std::uint32_t>(negative_policy),
                    static_cast<std::uint32_t>(di_curve.entries_per_segment())};
                vkCmdBindPipeline(slot.command_buffer,
                                  VK_PIPELINE_BIND_POINT_COMPUTE,
                                  di_pipeline);
                vkCmdBindDescriptorSets(slot.command_buffer,
                                        VK_PIPELINE_BIND_POINT_COMPUTE,
                                        di_pipeline_layout, 0, 1,
                                        &slot.di_descriptor, 0, nullptr);
                vkCmdPushConstants(slot.command_buffer, di_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(di_push), &di_push);
                vkCmdDispatch(slot.command_buffer,
                              static_cast<std::uint32_t>((pixels + 255U) / 256U),
                              1, 1);
                if (timestamp_query_pool != VK_NULL_HANDLE) {
                    vkCmdWriteTimestamp(slot.command_buffer,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        timestamp_query_pool,
                                        slot.di_timestamp_query_base + 1U);
                }
                std::array<VkBufferMemoryBarrier, 4> yuv_barriers{};
                for (std::size_t plane = 0; plane < 3U; ++plane) {
                    auto& barrier = yuv_barriers[plane];
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer = slot.intermediate[0][plane].buffer;
                    barrier.offset = 0;
                    barrier.size = VK_WHOLE_SIZE;
                }
                auto& status_ready = yuv_barriers[3];
                status_ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                status_ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                status_ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                status_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                status_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                status_ready.buffer = slot.control_status.buffer;
                status_ready.offset = 0;
                status_ready.size = sizeof(std::uint32_t);
                vkCmdPipelineBarrier(slot.command_buffer,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                         VK_PIPELINE_STAGE_HOST_BIT,
                                     0, 0, nullptr,
                                     static_cast<std::uint32_t>(yuv_barriers.size()),
                                     yuv_barriers.data(), 0, nullptr);
            }
            vkCmdPipelineBarrier(slot.command_buffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                 0, nullptr, static_cast<std::uint32_t>(image_count),
                                 barriers.data());
            if (timestamp_query_pool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(slot.command_buffer,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    timestamp_query_pool,
                                    slot.yuv_timestamp_query_base);
            }
            vkCmdBindPipeline(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
            const FramePushConstants push{
                config.width, config.height,
                config.chroma_filter == ChromaFilter::quality ? 1U : 0U,
                config.deterministic_dither ? 1U : 0U,
                static_cast<std::uint64_t>(frame_index)};
            vkCmdPushConstants(slot.command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            const auto pairs = config.width / 2U;
            vkCmdDispatch(slot.command_buffer, (pairs + 63U) / 64U, config.height, 1);
            if (timestamp_query_pool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(slot.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    timestamp_query_pool,
                                    slot.yuv_timestamp_query_base + 1U);
            }
            require_frame_vulkan(vkEndCommandBuffer(slot.command_buffer),
                                 "end direct-frame command buffer");
            VkTimelineSemaphoreSubmitInfo timeline{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount = static_cast<std::uint32_t>(image_count);
            timeline.pWaitSemaphoreValues = wait_values.data();
            timeline.signalSemaphoreValueCount = static_cast<std::uint32_t>(image_count);
            timeline.pSignalSemaphoreValues = signal_values.data();
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.pNext = &timeline;
            submit.waitSemaphoreCount = static_cast<std::uint32_t>(image_count);
            submit.pWaitSemaphores = frame->sem;
            submit.pWaitDstStageMask = wait_stages.data();
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &slot.command_buffer;
            submit.signalSemaphoreCount = static_cast<std::uint32_t>(image_count);
            submit.pSignalSemaphores = frame->sem;
            slot.submitted_at = std::chrono::steady_clock::now();
            // VkQueue is externally synchronized. Use the FFmpeg device's
            // queue mutex so its encoder submissions cannot race this one.
            const auto queue_lock_start = std::chrono::steady_clock::now();
            lock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            const auto queue_locked_at = std::chrono::steady_clock::now();
            const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
            unlock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            const auto queue_submitted_at = std::chrono::steady_clock::now();
            const auto queue_lock_wait_ms = std::chrono::duration<double, std::milli>(
                queue_locked_at - queue_lock_start).count();
            ++counters.queue_lock_wait_samples;
            counters.queue_lock_wait_total_ms += queue_lock_wait_ms;
            counters.queue_lock_wait_mean_ms = counters.queue_lock_wait_total_ms /
                static_cast<double>(counters.queue_lock_wait_samples);
            counters.queue_lock_wait_max_ms = std::max(
                counters.queue_lock_wait_max_ms, queue_lock_wait_ms);
            const auto queue_submit_ms = std::chrono::duration<double, std::milli>(
                queue_submitted_at - queue_locked_at).count();
            ++counters.queue_submit_samples;
            counters.queue_submit_total_ms += queue_submit_ms;
            counters.queue_submit_mean_ms = counters.queue_submit_total_ms /
                static_cast<double>(counters.queue_submit_samples);
            counters.queue_submit_max_ms = std::max(
                counters.queue_submit_max_ms, queue_submit_ms);
            require_frame_vulkan(submit_result, "submit direct-frame compute");
            submitted = true;
            slot.camera_chain = camera_chain;
            slot.in_flight = true;
            ++counters.in_flight;
            counters.max_in_flight = std::max(counters.max_in_flight,
                                              counters.in_flight);
            for (std::size_t image = 0; image < image_count; ++image) {
                frame->layout[image] = VK_IMAGE_LAYOUT_GENERAL;
                frame->access[image] = VK_ACCESS_SHADER_WRITE_BIT;
                frame->sem_value[image] = signal_values[image];
                if (frame->queue_family[image] != VK_QUEUE_FAMILY_IGNORED) {
                    frame->queue_family[image] = queue_family;
                }
            }
            vk_frames->unlock_frame(hw_frames, frame);
            frame_locked = false;
            ++counters.dispatches;
            ++counters.output_frames;
        } catch (...) {
            if (frame_locked) vk_frames->unlock_frame(hw_frames, frame);
            if (submitted) {
                vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                slot.in_flight = false;
                if (counters.in_flight > 0U) --counters.in_flight;
            }
            for (auto& view : slot.views) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, allocator);
                view = VK_NULL_HANDLE;
            }
            throw;
        }
        return output;
    }

    VulkanVideoFrame pack(const TargetLogRgbF32& input,
                          std::size_t frame_index,
                          FrameMetadata metadata) {
        return pack_impl(input, frame_index, metadata, nullptr,
                         0.0, 1.0, 0.0, 0.0,
                         NegativePolicy::preserve_by_curve);
    }

    VulkanVideoFrame pack_camera_rgb(
        const CameraRgbF32& input,
        const Matrix3d& camera_to_target,
        double exposure_offset_stops,
        double input_scale,
        double sharpening_amount,
        double sharpening_threshold,
        NegativePolicy negative_policy,
        std::size_t frame_index,
        FrameMetadata metadata) {
        return pack_impl(input, frame_index, metadata, &camera_to_target,
                         exposure_offset_stops, input_scale,
                         sharpening_amount, sharpening_threshold,
                         negative_policy);
    }

    void wait() {
        for (auto& slot : slots) recycle_slot(slot, false);
    }

    void cleanup() noexcept {
        if (device == VK_NULL_HANDLE) return;
        try {
            wait();
        } catch (...) {
            // Destructors cannot surface device-lost. Resource teardown below
            // remains best-effort; the owning runtime reports operational
            // errors through pack()/wait() during normal shutdown.
        }
        for (auto& slot : slots) {
            for (auto& view : slot.views) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, allocator);
            }
            if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(device, slot.fence, allocator);
        }
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, allocator);
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, timestamp_query_pool, allocator);
        }
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, allocator);
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, allocator);
        }
        if (color_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, color_pipeline, allocator);
        }
        if (color_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, color_pipeline_layout, allocator);
        }
        if (sharpen_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, sharpen_pipeline, allocator);
        }
        if (sharpen_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, sharpen_pipeline_layout, allocator);
        }
        if (di_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, di_pipeline, allocator);
        }
        if (di_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, di_pipeline_layout, allocator);
        }
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, allocator);
        }
        if (descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_layout, allocator);
        }
        if (camera_descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, camera_descriptor_layout, allocator);
        }
        if (di_descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, di_descriptor_layout, allocator);
        }
        for (auto& slot : slots) {
            for (auto& buffer : slot.input_buffers) destroy_buffer(buffer);
            for (auto& set : slot.intermediate) {
                for (auto& buffer : set) destroy_buffer(buffer);
            }
            destroy_buffer(slot.control_status);
        }
        destroy_buffer(di_lut);
    }

    VulkanRuntime& runtime;
    FfmpegVulkanFrameContext& frames;
    VulkanRgbToYuvConfig config;
    AVHWDeviceContext* hw_device{};
    AVVulkanDeviceContext* vulkan_device{};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    const VkAllocationCallbacks* allocator{};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    std::vector<Slot> slots;
    std::size_t next_slot{};
    VkDescriptorSetLayout descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout camera_descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout di_descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout color_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline color_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout sharpen_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline sharpen_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout di_pipeline_layout{VK_NULL_HANDLE};
    VkPipeline di_pipeline{VK_NULL_HANDLE};
    DaVinciIntermediateLut di_curve;
    Buffer di_lut;
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkQueryPool timestamp_query_pool{VK_NULL_HANDLE};
    std::uint32_t timestamp_valid_bits{};
    float timestamp_period_ns{};
    std::vector<double> gpu_timestamp_ms;
    std::vector<double> color_timestamp_ms;
    std::vector<double> sharpen_timestamp_ms;
    std::vector<double> di_timestamp_ms;
    VulkanRgbToYuvFrameTelemetry counters;

    [[nodiscard]] VulkanRgbToYuvFrameTelemetry telemetry() const {
        auto result = counters;
        if (!gpu_timestamp_ms.empty()) {
            auto sorted = gpu_timestamp_ms;
            std::sort(sorted.begin(), sorted.end());
            result.gpu_p50_ms = timestamp_percentile(sorted, 0.50);
            result.gpu_p95_ms = timestamp_percentile(sorted, 0.95);
            result.gpu_p99_ms = timestamp_percentile(sorted, 0.99);
        }
        const auto populate_percentiles = [](const std::vector<double>& samples,
                                             double& p50, double& p95,
                                             double& p99) {
            if (samples.empty()) return;
            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end());
            p50 = timestamp_percentile(sorted, 0.50);
            p95 = timestamp_percentile(sorted, 0.95);
            p99 = timestamp_percentile(sorted, 0.99);
        };
        populate_percentiles(color_timestamp_ms,
                             result.camera_to_dwg_gpu_p50_ms,
                             result.camera_to_dwg_gpu_p95_ms,
                             result.camera_to_dwg_gpu_p99_ms);
        populate_percentiles(sharpen_timestamp_ms,
                             result.capture_sharpening_gpu_p50_ms,
                             result.capture_sharpening_gpu_p95_ms,
                             result.capture_sharpening_gpu_p99_ms);
        populate_percentiles(di_timestamp_ms,
                             result.davinci_intermediate_gpu_p50_ms,
                             result.davinci_intermediate_gpu_p95_ms,
                             result.davinci_intermediate_gpu_p99_ms);
        return result;
    }
};

VulkanRgbToYuvFrameWriter::VulkanRgbToYuvFrameWriter(
    VulkanRuntime& runtime,
    FfmpegVulkanFrameContext& frames,
    VulkanRgbToYuvConfig config)
    : impl_(std::make_unique<Impl>(runtime, frames, config)) {}
VulkanRgbToYuvFrameWriter::~VulkanRgbToYuvFrameWriter() = default;
VulkanRgbToYuvFrameWriter::VulkanRgbToYuvFrameWriter(
    VulkanRgbToYuvFrameWriter&&) noexcept = default;
VulkanRgbToYuvFrameWriter& VulkanRgbToYuvFrameWriter::operator=(
    VulkanRgbToYuvFrameWriter&&) noexcept = default;
VulkanVideoFrame VulkanRgbToYuvFrameWriter::pack(const TargetLogRgbF32& input,
                                                 std::size_t frame_index,
                                                 FrameMetadata metadata) {
    return impl_->pack(input, frame_index, metadata);
}
VulkanVideoFrame VulkanRgbToYuvFrameWriter::pack_camera_rgb(
    const CameraRgbF32& input,
    const Matrix3d& camera_to_target,
    double exposure_offset_stops,
    double input_scale,
    double sharpening_amount,
    double sharpening_threshold,
    NegativePolicy negative_policy,
    std::size_t frame_index,
    FrameMetadata metadata) {
    return impl_->pack_camera_rgb(
        input, camera_to_target, exposure_offset_stops, input_scale,
        sharpening_amount, sharpening_threshold, negative_policy,
        frame_index, metadata);
}
void VulkanRgbToYuvFrameWriter::wait() { impl_->wait(); }
VulkanRgbToYuvFrameTelemetry VulkanRgbToYuvFrameWriter::telemetry() const {
    return impl_->telemetry();
}

} // namespace mcraw
