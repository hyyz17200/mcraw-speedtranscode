#include <mcraw/output/ffmpeg_vulkan_context.hpp>

#include <algorithm>
#include <limits>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

std::string pixel_format_name(AVPixelFormat format) {
    const auto* name = av_get_pix_fmt_name(format);
    return name != nullptr ? name : "unknown";
}

bool contains_format(const AVPixelFormat* formats, AVPixelFormat required) {
    if (formats == nullptr) return false;
    for (auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
        if (*current == required) return true;
    }
    return false;
}

std::vector<AVPixelFormat> copy_formats(const AVPixelFormat* formats) {
    std::vector<AVPixelFormat> output;
    if (formats == nullptr) return output;
    for (auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
        output.push_back(*current);
    }
    return output;
}

} // namespace

class FfmpegVulkanFrameContext::Impl {
public:
    Impl(VulkanRuntime& runtime, const FfmpegVulkanFrameContextConfig& input)
        : config(input) {
        if (config.width <= 0 || config.height <= 0 || (config.width & 1) != 0) {
            throw Error(ErrorCode::invalid_argument, "invalid Vulkan frame dimensions");
        }
        if (config.pool_size == 0U || config.pool_size > 64U) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan frame pool size must be between 1 and 64");
        }
        if (config.pool_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw Error(ErrorCode::invalid_argument, "Vulkan frame pool size is too large");
        }

        auto device_reference = runtime.reference_device_context();
        AVHWFramesConstraints* raw_constraints =
            av_hwdevice_get_hwframe_constraints(device_reference.get(), nullptr);
        if (raw_constraints == nullptr) {
            throw Error(ErrorCode::unsupported_format,
                        "FFmpeg returned no Vulkan hardware frame constraints");
        }
        valid_software_formats = copy_formats(raw_constraints->valid_sw_formats);
        const bool format_supported = contains_format(
            raw_constraints->valid_sw_formats, config.software_format);
        const bool dimensions_supported =
            config.width >= raw_constraints->min_width &&
            config.height >= raw_constraints->min_height &&
            config.width <= raw_constraints->max_width &&
            config.height <= raw_constraints->max_height;
        av_hwframe_constraints_free(&raw_constraints);
        if (!format_supported) {
            throw Error(ErrorCode::unsupported_format,
                        "Vulkan hardware frames do not support software format " +
                        pixel_format_name(config.software_format));
        }
        if (!dimensions_supported) {
            throw Error(ErrorCode::unsupported_format,
                        "Vulkan hardware frame dimensions are outside FFmpeg constraints");
        }

        AVBufferRef* raw_frames = av_hwframe_ctx_alloc(device_reference.get());
        if (raw_frames == nullptr) {
            throw Error(ErrorCode::processing_failed,
                        "cannot allocate FFmpeg Vulkan hardware frame context");
        }
        frames_context.reset(raw_frames);
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_context->data);
        frames->format = AV_PIX_FMT_VULKAN;
        frames->sw_format = config.software_format;
        frames->width = config.width;
        frames->height = config.height;
        frames->initial_pool_size = static_cast<int>(config.pool_size);
        auto* vulkan_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        image_usage = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vulkan_frames->usage = static_cast<VkImageUsageFlagBits>(image_usage);
        require_ffmpeg(av_hwframe_ctx_init(frames_context.get()),
                       "initialize FFmpeg Vulkan hardware frame context");

        for (const auto format : vulkan_frames->format) {
            if (format == VK_FORMAT_UNDEFINED) break;
            image_formats.push_back(format);
        }
        if (image_formats.empty()) {
            throw Error(ErrorCode::unsupported_format,
                        "FFmpeg selected no Vulkan image format for hardware frames");
        }

        AVPixelFormat* transfer_formats = nullptr;
        require_ffmpeg(av_hwframe_transfer_get_formats(
                           frames_context.get(), AV_HWFRAME_TRANSFER_DIRECTION_TO,
                           &transfer_formats, 0),
                       "query Vulkan upload formats");
        const bool upload_supported = contains_format(transfer_formats, config.software_format);
        av_freep(&transfer_formats);
        require_ffmpeg(av_hwframe_transfer_get_formats(
                           frames_context.get(), AV_HWFRAME_TRANSFER_DIRECTION_FROM,
                           &transfer_formats, 0),
                       "query Vulkan readback formats");
        const bool readback_supported = contains_format(transfer_formats, config.software_format);
        av_freep(&transfer_formats);
        if (!upload_supported || !readback_supported) {
            throw Error(ErrorCode::unsupported_format,
                        "Vulkan frame context lacks required yuv422p10 upload/readback transfer");
        }
    }

    FfmpegVulkanFrameContextConfig config;
    AvBufferRefPtr frames_context;
    VkImageUsageFlags image_usage{};
    std::vector<AVPixelFormat> valid_software_formats;
    std::vector<VkFormat> image_formats;
};

FfmpegVulkanFrameContext::FfmpegVulkanFrameContext(
    VulkanRuntime& runtime, FfmpegVulkanFrameContextConfig config)
    : impl_(std::make_unique<Impl>(runtime, config)) {}
FfmpegVulkanFrameContext::~FfmpegVulkanFrameContext() = default;
FfmpegVulkanFrameContext::FfmpegVulkanFrameContext(FfmpegVulkanFrameContext&&) noexcept = default;
FfmpegVulkanFrameContext& FfmpegVulkanFrameContext::operator=(
    FfmpegVulkanFrameContext&&) noexcept = default;

VulkanVideoFrame FfmpegVulkanFrameContext::allocate_frame(FrameMetadata metadata) const {
    if (metadata.width != impl_->config.width || metadata.height != impl_->config.height) {
        throw Error(ErrorCode::invalid_argument,
                    "Vulkan frame metadata dimensions do not match the frame context");
    }
    auto frame = make_av_frame();
    require_ffmpeg(av_hwframe_get_buffer(impl_->frames_context.get(), frame.get(), 0),
                   "allocate Vulkan hardware frame");
    frame->pts = metadata.pts;
    frame->duration = metadata.duration;
    frame->color_primaries = metadata.primaries;
    frame->color_trc = metadata.transfer;
    frame->colorspace = metadata.matrix;
    frame->color_range = metadata.range;
    frame->chroma_location = metadata.chroma_location;
    return {metadata, AV_PIX_FMT_VULKAN, std::move(frame), impl_->config.software_format};
}

VulkanFrameAllocationInfo FfmpegVulkanFrameContext::inspect_frame(const AVFrame& frame) const {
    if (frame.format != AV_PIX_FMT_VULKAN || frame.data[0] == nullptr) {
        throw Error(ErrorCode::invalid_argument, "frame is not an allocated Vulkan frame");
    }
    const auto* vulkan_frame = reinterpret_cast<const AVVkFrame*>(frame.data[0]);
    VulkanFrameAllocationInfo result;
    for (std::size_t image = 0; image < AV_NUM_DATA_POINTERS; ++image) {
        if (vulkan_frame->img[image] == VK_NULL_HANDLE) break;
        ++result.image_count;
        result.layouts.push_back(vulkan_frame->layout[image]);
        result.access.push_back(vulkan_frame->access[image]);
        result.queue_families.push_back(vulkan_frame->queue_family[image]);
        result.semaphore_values.push_back(vulkan_frame->sem_value[image]);
        result.formats.push_back(image < impl_->image_formats.size()
            ? impl_->image_formats[image] : VK_FORMAT_UNDEFINED);
        if (vulkan_frame->sem[image] == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "allocated Vulkan frame has no timeline semaphore");
        }
    }
    if (result.image_count == 0U) {
        throw Error(ErrorCode::processing_failed, "allocated Vulkan frame has no VkImage");
    }
    return result;
}

AvBufferRefPtr FfmpegVulkanFrameContext::reference_frames_context() const {
    auto* reference = av_buffer_ref(impl_->frames_context.get());
    if (reference == nullptr) {
        throw Error(ErrorCode::processing_failed, "cannot reference Vulkan frame context");
    }
    return AvBufferRefPtr(reference);
}

AVPixelFormat FfmpegVulkanFrameContext::software_format() const noexcept {
    return impl_->config.software_format;
}
int FfmpegVulkanFrameContext::width() const noexcept { return impl_->config.width; }
int FfmpegVulkanFrameContext::height() const noexcept { return impl_->config.height; }
std::size_t FfmpegVulkanFrameContext::pool_size() const noexcept { return impl_->config.pool_size; }
VkImageUsageFlags FfmpegVulkanFrameContext::image_usage() const noexcept {
    return impl_->image_usage;
}
const std::vector<AVPixelFormat>& FfmpegVulkanFrameContext::valid_software_formats() const noexcept {
    return impl_->valid_software_formats;
}
const std::vector<VkFormat>& FfmpegVulkanFrameContext::image_formats() const noexcept {
    return impl_->image_formats;
}

} // namespace mcraw
