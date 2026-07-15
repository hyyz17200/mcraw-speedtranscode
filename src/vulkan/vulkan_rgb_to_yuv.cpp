#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

#include <mcraw/core/error.hpp>
#include <mcraw/vulkan/rgb_to_yuv_422_spv.hpp>

namespace mcraw {
namespace {

void require_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw Error(result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost
                                                   : ErrorCode::processing_failed,
                    std::string(operation) + " failed with VkResult " +
                    std::to_string(static_cast<int>(result)));
    }
}

std::size_t checked_bytes(std::size_t values, std::size_t value_size) {
    if (values > std::numeric_limits<std::size_t>::max() / value_size) {
        throw Error(ErrorCode::invalid_argument, "Vulkan RGB-to-YUV buffer size overflow");
    }
    return values * value_size;
}

struct PushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t quality_filter{};
    std::uint32_t enable_dither{};
    std::uint64_t frame_index{};
};
static_assert(sizeof(PushConstants) == 24U);

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

} // namespace

class VulkanRgbToYuv422::Impl {
public:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{};
    };

    Impl(VulkanRuntime& owner, VulkanRgbToYuvConfig requested)
        : runtime(owner), config(requested) {
        if (config.width == 0U || config.height == 0U || (config.width & 1U) != 0U) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan RGB-to-YUV requires non-zero dimensions and even width");
        }
        if (config.precision != GpuPrecision::fp32) {
            throw Error(ErrorCode::unsupported_format,
                        "FP16 RGB-to-YUV is disabled until its error budget is validated");
        }
        const auto* context = runtime.ffmpeg_vulkan_context();
        if (context == nullptr || context->act_dev == VK_NULL_HANDLE ||
            context->phys_dev == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg-owned Vulkan device is unavailable to RGB-to-YUV");
        }
        device = context->act_dev;
        physical_device = context->phys_dev;
        queue_family = runtime.compute_queue_family();
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (queue == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed, "cannot obtain Vulkan compute queue");
        }
        try {
            create_buffers();
            create_descriptors();
            create_pipeline();
            create_commands();
            create_timestamp_queries();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void create_buffers() {
        const auto pixels = static_cast<std::size_t>(config.width) * config.height;
        const auto chroma = pixels / 2U;
        const std::array<VkDeviceSize, 6> sizes{
            checked_bytes(pixels, sizeof(float)),
            checked_bytes(pixels, sizeof(float)),
            checked_bytes(pixels, sizeof(float)),
            checked_bytes(pixels, sizeof(std::uint32_t)),
            checked_bytes(chroma, sizeof(std::uint32_t)),
            checked_bytes(chroma, sizeof(std::uint32_t))};
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            buffers[index] = create_buffer(sizes[index]);
        }
    }

    Buffer create_buffer(VkDeviceSize size) {
        Buffer result;
        result.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_vulkan(vkCreateBuffer(device, &info, nullptr, &result.buffer),
                       "create RGB-to-YUV storage buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {
            require_vulkan(vkAllocateMemory(device, &allocation, nullptr, &result.memory),
                           "allocate RGB-to-YUV storage memory");
            require_vulkan(vkBindBufferMemory(device, result.buffer, result.memory, 0),
                           "bind RGB-to-YUV storage memory");
        } catch (...) {
            if (result.memory != VK_NULL_HANDLE) vkFreeMemory(device, result.memory, nullptr);
            vkDestroyBuffer(device, result.buffer, nullptr);
            throw;
        }
        return result;
    }

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
                    "GPU has no coherent host-visible storage memory for RGB-to-YUV validation");
    }

    void create_descriptors() {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
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
        require_vulkan(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
                                                   &descriptor_layout),
                       "create RGB-to-YUV descriptor layout");

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       static_cast<std::uint32_t>(bindings.size())};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        require_vulkan(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool),
                       "create RGB-to-YUV descriptor pool");
        VkDescriptorSetAllocateInfo allocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptor_pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &descriptor_layout;
        require_vulkan(vkAllocateDescriptorSets(device, &allocation, &descriptor_set),
                       "allocate RGB-to-YUV descriptor set");

        std::array<VkDescriptorBufferInfo, 6> buffer_info{};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (std::uint32_t index = 0; index < buffers.size(); ++index) {
            buffer_info[index] = {buffers[index].buffer, 0, buffers[index].size};
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptor_set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &buffer_info[index];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    void create_pipeline() {
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
        require_vulkan(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout),
                       "create RGB-to-YUV pipeline layout");

        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = generated::rgb_to_yuv_422_spv.size() * sizeof(std::uint32_t);
        shader_info.pCode = generated::rgb_to_yuv_422_spv.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        require_vulkan(vkCreateShaderModule(device, &shader_info, nullptr, &shader),
                       "create RGB-to-YUV shader module");
        VkComputePipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.layout = pipeline_layout;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader;
        pipeline_info.stage.pName = "main";
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                     &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(device, shader, nullptr);
        require_vulkan(result, "create RGB-to-YUV compute pipeline");
    }

    void create_commands() {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        require_vulkan(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool),
                       "create RGB-to-YUV command pool");
        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        require_vulkan(vkAllocateCommandBuffers(device, &allocation, &command_buffer),
                       "allocate RGB-to-YUV command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        require_vulkan(vkCreateFence(device, &fence_info, nullptr, &fence),
                       "create RGB-to-YUV fence");
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
        info.queryCount = 2;
        require_vulkan(vkCreateQueryPool(device, &info, nullptr, &timestamp_query_pool),
                       "create RGB-to-YUV timestamp query pool");
        counters.gpu_timestamps_supported = true;
    }

    void upload(std::size_t buffer_index, const std::vector<float>& values) {
        const auto bytes = checked_bytes(values.size(), sizeof(float));
        if (bytes != buffers[buffer_index].size) {
            throw Error(ErrorCode::invalid_argument, "RGB plane size changed during Vulkan pack");
        }
        void* mapped = nullptr;
        require_vulkan(vkMapMemory(device, buffers[buffer_index].memory, 0,
                                   buffers[buffer_index].size, 0, &mapped),
                       "map RGB upload buffer");
        std::memcpy(mapped, values.data(), bytes);
        vkUnmapMemory(device, buffers[buffer_index].memory);
        counters.upload_bytes += bytes;
    }

    std::vector<std::uint16_t> download(std::size_t buffer_index,
                                        std::size_t values) {
        const auto bytes = checked_bytes(values, sizeof(std::uint32_t));
        if (bytes != buffers[buffer_index].size) {
            throw Error(ErrorCode::processing_failed, "YUV download buffer size mismatch");
        }
        void* mapped = nullptr;
        require_vulkan(vkMapMemory(device, buffers[buffer_index].memory, 0,
                                   buffers[buffer_index].size, 0, &mapped),
                       "map YUV download buffer");
        const auto* source = static_cast<const std::uint32_t*>(mapped);
        std::vector<std::uint16_t> result(values);
        for (std::size_t index = 0; index < values; ++index) {
            if (source[index] > 1023U) {
                vkUnmapMemory(device, buffers[buffer_index].memory);
                throw Error(ErrorCode::processing_failed,
                            "RGB-to-YUV shader emitted a code outside 10-bit range");
            }
            result[index] = static_cast<std::uint16_t>(source[index]);
        }
        vkUnmapMemory(device, buffers[buffer_index].memory);
        counters.download_bytes += bytes;
        return result;
    }

    Yuv422P10 pack(const TargetLogRgbF32& input, std::size_t frame_index) {
        input.validate();
        if (input.width != config.width || input.height != config.height) {
            throw Error(ErrorCode::invalid_argument,
                        "RGB frame dimensions changed during Vulkan pack");
        }
        for (std::size_t plane = 0; plane < 3; ++plane) upload(plane, input.planes[plane]);
        require_vulkan(vkResetFences(device, 1, &fence), "reset RGB-to-YUV fence");
        require_vulkan(vkResetCommandBuffer(command_buffer, 0),
                       "reset RGB-to-YUV command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_vulkan(vkBeginCommandBuffer(command_buffer, &begin),
                       "begin RGB-to-YUV command buffer");
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(command_buffer, timestamp_query_pool, 0, 2);
            vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                timestamp_query_pool, 0);
        }
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        const PushConstants push{config.width, config.height,
                                 config.chroma_filter == ChromaFilter::quality ? 1U : 0U,
                                 config.deterministic_dither ? 1U : 0U,
                                 static_cast<std::uint64_t>(frame_index)};
        vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        const auto pairs = config.width / 2U;
        vkCmdDispatch(command_buffer, (pairs + 63U) / 64U, config.height, 1);
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                timestamp_query_pool, 1);
        }
        require_vulkan(vkEndCommandBuffer(command_buffer),
                       "end RGB-to-YUV command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        const auto start = std::chrono::steady_clock::now();
        require_vulkan(vkQueueSubmit(queue, 1, &submit, fence), "submit RGB-to-YUV compute");
        require_vulkan(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
                       "wait for RGB-to-YUV compute");
        counters.last_dispatch_wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            std::array<std::uint64_t, 2> values{};
            require_vulkan(vkGetQueryPoolResults(
                device, timestamp_query_pool, 0, 2, sizeof(values), values.data(),
                sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "read RGB-to-YUV GPU timestamps");
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
        ++counters.dispatches;

        const auto pixels = static_cast<std::size_t>(config.width) * config.height;
        Yuv422P10 result;
        result.width = config.width;
        result.height = config.height;
        result.y = download(3, pixels);
        result.cb = download(4, pixels / 2U);
        result.cr = download(5, pixels / 2U);
        result.validate();
        return result;
    }

    void cleanup() noexcept {
        if (device == VK_NULL_HANDLE) return;
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
        if (timestamp_query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, timestamp_query_pool, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (descriptor_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        }
        for (auto& buffer : buffers) {
            if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer.buffer, nullptr);
            if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer.memory, nullptr);
            buffer = {};
        }
    }

    VulkanRuntime& runtime;
    VulkanRgbToYuvConfig config;
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    std::array<Buffer, 6> buffers;
    VkDescriptorSetLayout descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkQueryPool timestamp_query_pool{VK_NULL_HANDLE};
    std::uint32_t timestamp_valid_bits{};
    float timestamp_period_ns{};
    std::vector<double> gpu_timestamp_ms;
    VulkanRgbToYuvTelemetry counters;

    [[nodiscard]] VulkanRgbToYuvTelemetry telemetry() const {
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

VulkanRgbToYuv422::VulkanRgbToYuv422(VulkanRuntime& runtime,
                                     VulkanRgbToYuvConfig config)
    : impl_(std::make_unique<Impl>(runtime, config)) {}
VulkanRgbToYuv422::~VulkanRgbToYuv422() = default;
VulkanRgbToYuv422::VulkanRgbToYuv422(VulkanRgbToYuv422&&) noexcept = default;
VulkanRgbToYuv422& VulkanRgbToYuv422::operator=(VulkanRgbToYuv422&&) noexcept = default;
Yuv422P10 VulkanRgbToYuv422::pack(const TargetLogRgbF32& input,
                                  std::size_t frame_index) {
    return impl_->pack(input, frame_index);
}
VulkanRgbToYuvTelemetry VulkanRgbToYuv422::telemetry() const {
    return impl_->telemetry();
}

} // namespace mcraw
