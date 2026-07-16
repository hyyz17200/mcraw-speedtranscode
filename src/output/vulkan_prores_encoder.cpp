#include <mcraw/output/vulkan_prores_encoder.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/opt.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

void require_private_option(void* private_data, const char* name) {
    if (av_opt_find(private_data, name, nullptr, 0, 0) == nullptr) {
        throw Error(ErrorCode::unsupported_format,
                    std::string("prores_ks_vulkan has no required option: ") + name);
    }
}

void append_packets(std::vector<EncodedPacket>& destination,
                    std::vector<EncodedPacket>&& source) {
    std::move(source.begin(), source.end(), std::back_inserter(destination));
}

} // namespace

class VulkanProResEncoder::Impl {
public:
    Impl(FfmpegVulkanFrameContext& input_frames,
         const VulkanProResEncoderConfig& config)
        : frames(input_frames), configured_async_depth(config.async_depth) {
        if (config.width <= 0 || config.height <= 0 || (config.width & 1) != 0 ||
            config.width != frames.width() || config.height != frames.height()) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan ProRes encoder dimensions do not match its frame context");
        }
        if (config.async_depth == 0U ||
            config.async_depth > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw Error(ErrorCode::invalid_argument, "invalid Vulkan ProRes async depth");
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks_vulkan");
        if (codec == nullptr) {
            throw Error(ErrorCode::unsupported_format,
                        "linked FFmpeg build has no prores_ks_vulkan encoder");
        }
        context.reset(avcodec_alloc_context3(codec));
        if (!context) {
            throw Error(ErrorCode::encode_failed,
                        "cannot allocate prores_ks_vulkan codec context");
        }
        context->codec_type = AVMEDIA_TYPE_VIDEO;
        context->codec_id = codec->id;
        context->width = config.width;
        context->height = config.height;
        context->pix_fmt = AV_PIX_FMT_VULKAN;
        context->time_base = config.time_base;
        context->framerate = config.frame_rate;
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT2020_NCL;
        context->color_primaries = AVCOL_PRI_UNSPECIFIED;
        context->color_trc = AVCOL_TRC_UNSPECIFIED;
        context->chroma_sample_location = AVCHROMA_LOC_LEFT;
        auto frames_reference = frames.reference_frames_context();
        context->hw_frames_ctx = frames_reference.release();

        require_private_option(context->priv_data, "profile");
        require_private_option(context->priv_data, "alpha_bits");
        require_private_option(context->priv_data, "async_depth");
        require_ffmpeg(av_opt_set(context->priv_data, "profile", config.profile.c_str(), 0),
                       "select Vulkan ProRes profile");
        require_ffmpeg(av_opt_set_int(context->priv_data, "alpha_bits", 0, 0),
                       "disable Vulkan ProRes alpha");
        require_ffmpeg(av_opt_set_int(context->priv_data, "async_depth",
                                      static_cast<std::int64_t>(config.async_depth), 0),
                       "set Vulkan ProRes async depth");
        require_ffmpeg(avcodec_open2(context.get(), codec, nullptr),
                       "open prores_ks_vulkan encoder");
    }

    std::vector<EncodedPacket> receive_available(bool flushing) {
        std::vector<EncodedPacket> output;
        while (true) {
            auto packet = make_av_packet();
            const int result = avcodec_receive_packet(context.get(), packet.get());
            if (result == AVERROR_EOF) {
                reached_eof = true;
                break;
            }
            if (result == AVERROR(EAGAIN)) {
                if (flushing) {
                    throw Error(ErrorCode::encode_failed,
                                "prores_ks_vulkan requested more input after flush");
                }
                break;
            }
            require_ffmpeg(result, "receive Vulkan ProRes packet");
            ++counters.packets;
            output.push_back({std::move(packet), context->time_base});
        }
        return output;
    }

    FfmpegVulkanFrameContext& frames;
    std::size_t configured_async_depth{};
    AvCodecContextPtr context;
    std::vector<EncodedPacket> buffered;
    VulkanProResTelemetry counters;
    bool flush_sent{};
    bool reached_eof{};
};

VulkanProResEncoder::VulkanProResEncoder(FfmpegVulkanFrameContext& frames,
                                         VulkanProResEncoderConfig config)
    : impl_(std::make_unique<Impl>(frames, config)) {}
VulkanProResEncoder::~VulkanProResEncoder() = default;
VulkanProResEncoder::VulkanProResEncoder(VulkanProResEncoder&&) noexcept = default;
VulkanProResEncoder& VulkanProResEncoder::operator=(VulkanProResEncoder&&) noexcept = default;

VideoEncoderCapabilities VulkanProResEncoder::capabilities() const {
    return {"prores_ks_vulkan", FrameStorage::vulkan, true, true, {}};
}

void VulkanProResEncoder::send(VideoFrame input) {
    if (impl_->flush_sent) {
        throw Error(ErrorCode::encode_failed, "cannot send Vulkan ProRes frame after flush");
    }
    VulkanVideoFrame uploaded;
    AVFrame* encoder_frame = nullptr;
    if (auto* software = std::get_if<CpuVideoFrame>(&input)) {
        if (!software->frame || software->format != AV_PIX_FMT_YUV422P10LE ||
            software->frame->format != AV_PIX_FMT_YUV422P10LE ||
            software->metadata.width != impl_->context->width ||
            software->metadata.height != impl_->context->height ||
            software->frame->width != impl_->context->width ||
            software->frame->height != impl_->context->height ||
            av_cmp_q(software->metadata.time_base, impl_->context->time_base) != 0) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan upload bridge requires matching owned yuv422p10 CPU frames");
        }
        uploaded = impl_->frames.allocate_frame(software->metadata);
        require_ffmpeg(av_hwframe_transfer_data(uploaded.frame.get(), software->frame.get(), 0),
                       "upload CPU frame to Vulkan ProRes input");
        ++impl_->counters.upload_frames;
        encoder_frame = uploaded.frame.get();
    } else if (auto* hardware = std::get_if<VulkanVideoFrame>(&input)) {
        if (!hardware->frame || hardware->format != AV_PIX_FMT_VULKAN ||
            hardware->software_format != AV_PIX_FMT_YUV422P10LE ||
            hardware->metadata.width != impl_->context->width ||
            hardware->metadata.height != impl_->context->height ||
            av_cmp_q(hardware->metadata.time_base, impl_->context->time_base) != 0 ||
            !impl_->frames.owns(*hardware->frame)) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan ProRes requires a frame from its exact FFmpeg frame pool");
        }
        ++impl_->counters.direct_frames;
        encoder_frame = hardware->frame.get();
    }
    if (encoder_frame == nullptr) {
        throw Error(ErrorCode::invalid_argument, "Vulkan ProRes received no usable frame");
    }
    impl_->counters.gpu_resident = impl_->counters.upload_frames == 0U &&
                                   impl_->counters.direct_frames > 0U;

    int result = avcodec_send_frame(impl_->context.get(), encoder_frame);
    if (result == AVERROR(EAGAIN)) {
        append_packets(impl_->buffered, impl_->receive_available(false));
        result = avcodec_send_frame(impl_->context.get(), encoder_frame);
    }
    require_ffmpeg(result, "send Vulkan ProRes frame");
}

std::vector<EncodedPacket> VulkanProResEncoder::drain() {
    auto output = std::move(impl_->buffered);
    impl_->buffered.clear();
    append_packets(output, impl_->receive_available(false));
    return output;
}

std::vector<EncodedPacket> VulkanProResEncoder::flush() {
    if (!impl_->flush_sent) {
        int result = avcodec_send_frame(impl_->context.get(), nullptr);
        if (result == AVERROR(EAGAIN)) {
            append_packets(impl_->buffered, impl_->receive_available(false));
            result = avcodec_send_frame(impl_->context.get(), nullptr);
        }
        require_ffmpeg(result, "flush Vulkan ProRes encoder");
        impl_->flush_sent = true;
    }
    auto output = std::move(impl_->buffered);
    impl_->buffered.clear();
    if (!impl_->reached_eof) append_packets(output, impl_->receive_available(true));
    return output;
}

VulkanProResTelemetry VulkanProResEncoder::telemetry() const noexcept {
    return impl_->counters;
}

void VulkanProResEncoder::copy_parameters_to(AVCodecParameters* parameters) const {
    if (parameters == nullptr) {
        throw Error(ErrorCode::invalid_argument, "Vulkan ProRes codec parameters are null");
    }
    require_ffmpeg(avcodec_parameters_from_context(parameters, impl_->context.get()),
                   "copy Vulkan ProRes codec parameters");
}

AVRational VulkanProResEncoder::time_base() const noexcept {
    return impl_->context->time_base;
}

std::size_t VulkanProResEncoder::async_depth() const noexcept {
    return impl_->configured_async_depth;
}

} // namespace mcraw
