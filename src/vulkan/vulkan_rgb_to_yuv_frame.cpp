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
#include <mcraw/vulkan/rgb_to_yuv_422_image_spv.hpp>

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
        VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        std::array<VkImageView, 3> views{};
        std::uint32_t timestamp_query_base{};
        bool in_flight{};
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
            create_timestamp_queries();
            create_commands();
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
                    "GPU has no coherent host-visible RGB input memory");
    }

    Buffer create_buffer(VkDeviceSize size) {
        Buffer result;
        result.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_frame_vulkan(vkCreateBuffer(device, &info, allocator, &result.buffer),
                             "create direct-frame RGB buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
        }
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
        const auto descriptor_count = static_cast<std::uint32_t>(config.slots * 3U);
        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptor_count}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = static_cast<std::uint32_t>(config.slots);
        pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        require_frame_vulkan(vkCreateDescriptorPool(device, &pool, allocator,
                                                    &descriptor_pool),
                             "create direct-frame descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(config.slots, descriptor_layout);
        std::vector<VkDescriptorSet> sets(config.slots);
        VkDescriptorSetAllocateInfo allocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptor_pool;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(config.slots);
        allocation.pSetLayouts = layouts.data();
        require_frame_vulkan(vkAllocateDescriptorSets(device, &allocation, sets.data()),
                             "allocate direct-frame descriptor sets");
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].descriptor_set = sets[index];
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
            slots[index].timestamp_query_base = static_cast<std::uint32_t>(index * 2U);
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
        info.queryCount = static_cast<std::uint32_t>(slots.size() * 2U);
        require_frame_vulkan(vkCreateQueryPool(device, &info, allocator, &timestamp_query_pool),
                             "create direct-frame timestamp query pool");
        counters.gpu_timestamps_supported = true;
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

    VkDescriptorSet update_descriptors(Slot& slot) {
        std::array<VkDescriptorBufferInfo, 3> buffer_info{};
        std::array<VkDescriptorImageInfo, 3> image_info{};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (std::uint32_t index = 0; index < 3U; ++index) {
            buffer_info[index] = {slot.input_buffers[index].buffer, 0,
                                  slot.input_buffers[index].size};
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = slot.descriptor_set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &buffer_info[index];
            image_info[index] = {VK_NULL_HANDLE, slot.views[index], VK_IMAGE_LAYOUT_GENERAL};
            writes[index + 3U].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index + 3U].dstSet = slot.descriptor_set;
            writes[index + 3U].dstBinding = index + 3U;
            writes[index + 3U].descriptorCount = 1;
            writes[index + 3U].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[index + 3U].pImageInfo = &image_info[index];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return slot.descriptor_set;
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
            std::array<std::uint64_t, 2> values{};
            require_frame_vulkan(vkGetQueryPoolResults(
                device, timestamp_query_pool, slot.timestamp_query_base, 2,
                sizeof(values), values.data(), sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "read direct-frame GPU timestamps");
            const auto ticks = timestamp_delta(values[0], values[1], timestamp_valid_bits);
            const double milliseconds = static_cast<double>(ticks) *
                                        static_cast<double>(timestamp_period_ns) / 1.0e6;
            gpu_timestamp_ms.push_back(milliseconds);
            counters.last_gpu_dispatch_ms = milliseconds;
            counters.gpu_total_ms += milliseconds;
            counters.gpu_timestamp_samples = gpu_timestamp_ms.size();
            counters.gpu_mean_ms = counters.gpu_total_ms /
                                   static_cast<double>(counters.gpu_timestamp_samples);
            if (counters.gpu_timestamp_samples == 1U) {
                counters.gpu_min_ms = milliseconds;
                counters.gpu_max_ms = milliseconds;
            } else {
                counters.gpu_min_ms = std::min(counters.gpu_min_ms, milliseconds);
                counters.gpu_max_ms = std::max(counters.gpu_max_ms, milliseconds);
            }
        }
        for (auto& view : slot.views) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, allocator);
            view = VK_NULL_HANDLE;
        }
        slot.in_flight = false;
        if (counters.in_flight > 0U) --counters.in_flight;
    }

    VulkanVideoFrame pack(const TargetLogRgbF32& input,
                          std::size_t frame_index,
                          FrameMetadata metadata) {
        input.validate();
        if (input.width != config.width || input.height != config.height ||
            metadata.width != static_cast<int>(config.width) ||
            metadata.height != static_cast<int>(config.height)) {
            throw Error(ErrorCode::invalid_argument,
                        "direct Vulkan frame dimensions do not match RGB input");
        }
        auto& slot = slots[next_slot];
        next_slot = (next_slot + 1U) % slots.size();
        recycle_slot(slot, true);
        for (std::size_t plane = 0; plane < 3; ++plane) {
            upload(slot, plane, input.planes[plane]);
        }
        auto output = frames.allocate_frame(metadata);
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
        const auto descriptor_set = update_descriptors(slot);
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
                                    slot.timestamp_query_base, 2);
                vkCmdWriteTimestamp(slot.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    timestamp_query_pool, slot.timestamp_query_base);
            }
            vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                 0, nullptr, static_cast<std::uint32_t>(image_count),
                                 barriers.data());
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
                                    slot.timestamp_query_base + 1U);
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
            lock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            const auto submit_result = vkQueueSubmit(queue, 1, &submit, slot.fence);
            unlock_ffmpeg_queue(*vulkan_device, hw_device, queue_family);
            require_frame_vulkan(submit_result, "submit direct-frame compute");
            submitted = true;
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
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, allocator);
        }
        if (descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_layout, allocator);
        }
        for (auto& slot : slots) {
            for (auto& buffer : slot.input_buffers) {
                if (buffer.buffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device, buffer.buffer, allocator);
                }
                if (buffer.memory != VK_NULL_HANDLE) {
                    vkFreeMemory(device, buffer.memory, allocator);
                }
                buffer = {};
            }
        }
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
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkQueryPool timestamp_query_pool{VK_NULL_HANDLE};
    std::uint32_t timestamp_valid_bits{};
    float timestamp_period_ns{};
    std::vector<double> gpu_timestamp_ms;
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
void VulkanRgbToYuvFrameWriter::wait() { impl_->wait(); }
VulkanRgbToYuvFrameTelemetry VulkanRgbToYuvFrameWriter::telemetry() const {
    return impl_->telemetry();
}

} // namespace mcraw
