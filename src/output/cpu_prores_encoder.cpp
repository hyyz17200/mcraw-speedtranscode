#include <mcraw/output/cpu_prores_encoder.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

extern "C" {
#include <libavutil/opt.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {

class CpuProResEncoder::Impl {
public:
    explicit Impl(const CpuProResEncoderConfig& config) {
        if (config.width <= 0 || config.height <= 0 || (config.width & 1) != 0) {
            throw Error(ErrorCode::invalid_argument, "invalid CPU ProRes frame dimensions");
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
        if (codec == nullptr) {
            throw Error(ErrorCode::encode_failed, "FFmpeg build has no prores_ks encoder");
        }
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw Error(ErrorCode::encode_failed, "cannot allocate ProRes codec context");
        context->codec_type = AVMEDIA_TYPE_VIDEO;
        context->codec_id = codec->id;
        context->width = config.width;
        context->height = config.height;
        context->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        context->time_base = config.time_base;
        context->framerate = config.frame_rate;
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT2020_NCL;
        context->color_primaries = AVCOL_PRI_UNSPECIFIED;
        context->color_trc = AVCOL_TRC_UNSPECIFIED;
        context->chroma_sample_location = AVCHROMA_LOC_LEFT;
        context->thread_count = std::max(config.threads, 0);
        context->thread_type = FF_THREAD_SLICE;
        require_ffmpeg(av_opt_set(context->priv_data, "profile", config.profile.c_str(), 0),
                       "select CPU ProRes profile");
        require_ffmpeg(avcodec_open2(context.get(), codec, nullptr), "open prores_ks encoder");
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
                                "prores_ks requested more input after flush");
                }
                break;
            }
            require_ffmpeg(result, "receive CPU ProRes packet");
            output.push_back({std::move(packet), context->time_base});
        }
        return output;
    }

    AvCodecContextPtr context;
    std::vector<EncodedPacket> buffered;
    bool flush_sent{};
    bool reached_eof{};
};

CpuProResEncoder::CpuProResEncoder(CpuProResEncoderConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
CpuProResEncoder::~CpuProResEncoder() = default;
CpuProResEncoder::CpuProResEncoder(CpuProResEncoder&&) noexcept = default;
CpuProResEncoder& CpuProResEncoder::operator=(CpuProResEncoder&&) noexcept = default;

VideoEncoderCapabilities CpuProResEncoder::capabilities() const {
    return {"prores_ks", FrameStorage::cpu, true,
            (impl_->context->codec->capabilities & AV_CODEC_CAP_DELAY) != 0, {}};
}

void CpuProResEncoder::send(VideoFrame input) {
    if (impl_->flush_sent) {
        throw Error(ErrorCode::encode_failed, "cannot send CPU ProRes frame after flush");
    }
    auto* frame = std::get_if<CpuVideoFrame>(&input);
    if (frame == nullptr || !frame->frame) {
        throw Error(ErrorCode::invalid_argument, "CPU ProRes encoder requires an owned CPU frame");
    }
    if (frame->format != AV_PIX_FMT_YUV422P10LE ||
        frame->frame->format != AV_PIX_FMT_YUV422P10LE ||
        frame->metadata.width != impl_->context->width ||
        frame->metadata.height != impl_->context->height ||
        frame->frame->width != impl_->context->width ||
        frame->frame->height != impl_->context->height ||
        av_cmp_q(frame->metadata.time_base, impl_->context->time_base) != 0) {
        throw Error(ErrorCode::invalid_argument,
                    "CPU ProRes input frame format, dimensions, or time base mismatch");
    }
    frame->frame->pts = frame->metadata.pts;
    frame->frame->duration = frame->metadata.duration;
    frame->frame->color_range = frame->metadata.range;
    frame->frame->colorspace = frame->metadata.matrix;
    frame->frame->color_primaries = frame->metadata.primaries;
    frame->frame->color_trc = frame->metadata.transfer;
    frame->frame->chroma_location = frame->metadata.chroma_location;
    int result = avcodec_send_frame(impl_->context.get(), frame->frame.get());
    if (result == AVERROR(EAGAIN)) {
        auto packets = impl_->receive_available(false);
        std::move(packets.begin(), packets.end(), std::back_inserter(impl_->buffered));
        result = avcodec_send_frame(impl_->context.get(), frame->frame.get());
    }
    require_ffmpeg(result, "send CPU ProRes frame");
}

std::vector<EncodedPacket> CpuProResEncoder::drain() {
    auto output = std::move(impl_->buffered);
    impl_->buffered.clear();
    auto packets = impl_->receive_available(false);
    std::move(packets.begin(), packets.end(), std::back_inserter(output));
    return output;
}

std::vector<EncodedPacket> CpuProResEncoder::flush() {
    if (!impl_->flush_sent) {
        int result = avcodec_send_frame(impl_->context.get(), nullptr);
        if (result == AVERROR(EAGAIN)) {
            auto packets = impl_->receive_available(false);
            std::move(packets.begin(), packets.end(), std::back_inserter(impl_->buffered));
            result = avcodec_send_frame(impl_->context.get(), nullptr);
        }
        require_ffmpeg(result, "flush CPU ProRes encoder");
        impl_->flush_sent = true;
    }
    auto output = std::move(impl_->buffered);
    impl_->buffered.clear();
    if (!impl_->reached_eof) {
        auto packets = impl_->receive_available(true);
        std::move(packets.begin(), packets.end(), std::back_inserter(output));
    }
    return output;
}

} // namespace mcraw
