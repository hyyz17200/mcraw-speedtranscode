#include <mcraw/output/ffmpeg_writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void require_ffmpeg(int code, std::string_view operation) {
    if (code < 0) {
        throw Error(ErrorCode::encode_failed,
                    std::string(operation) + ": " + ffmpeg_error(code));
    }
}

std::int64_t pts_from_ns(std::int64_t timestamp_ns,
                         std::int64_t origin_ns,
                         AVRational time_base) {
    if (timestamp_ns < origin_ns) {
        throw Error(ErrorCode::encode_failed, "timestamp precedes the output timeline origin");
    }
    return av_rescale_q(timestamp_ns - origin_ns, AVRational{1, 1'000'000'000}, time_base);
}

void free_vector_plane(void* opaque, std::uint8_t*) {
    delete static_cast<std::vector<std::uint16_t>*>(opaque);
}

void attach_vector_plane(AVFrame* frame,
                         int plane,
                         std::vector<std::uint16_t>&& samples,
                         std::uint32_t width) {
    auto* owner = new std::vector<std::uint16_t>(std::move(samples));
    auto* buffer = av_buffer_create(
        reinterpret_cast<std::uint8_t*>(owner->data()),
        owner->size() * sizeof(std::uint16_t), free_vector_plane, owner, 0);
    if (buffer == nullptr) {
        delete owner;
        throw Error(ErrorCode::encode_failed, "cannot wrap YUV plane for FFmpeg");
    }
    frame->buf[plane] = buffer;
    frame->data[plane] = buffer->data;
    frame->linesize[plane] = static_cast<int>(width * sizeof(std::uint16_t));
}

} // namespace

class FfmpegWriter::Impl {
public:
    Impl(const std::filesystem::path& output,
         std::uint32_t width,
         std::uint32_t height,
         std::int64_t timeline_origin_ns,
         int audio_sample_rate,
         int audio_channels)
        : origin_ns(timeline_origin_ns), output_path(output) {
        if (width == 0 || height == 0 || (width & 1U) != 0U) {
            throw Error(ErrorCode::invalid_argument, "invalid ProRes frame dimensions");
        }
        const auto path_utf8 = output.u8string();
        const std::string path_string(path_utf8.begin(), path_utf8.end());
        require_ffmpeg(avformat_alloc_output_context2(&format, nullptr, "mov", path_string.c_str()),
                       "allocate MOV output context");
        if (format == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg returned no MOV output context");

        try {
            create_video(width, height);
            if (audio_sample_rate > 0 && audio_channels > 0) {
                create_audio(audio_sample_rate, audio_channels);
            }
            if ((format->oformat->flags & AVFMT_NOFILE) == 0) {
                require_ffmpeg(avio_open(&format->pb, path_string.c_str(), AVIO_FLAG_WRITE), "open output MOV");
            }
            AVDictionary* options = nullptr;
            av_dict_set(&options, "movflags", "write_colr", 0);
            const int header_result = avformat_write_header(format, &options);
            av_dict_free(&options);
            require_ffmpeg(header_result, "write MOV header");
            header_written = true;
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void create_video(std::uint32_t width, std::uint32_t height) {
        const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
        if (codec == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg build has no prores_ks encoder");
        video_stream = avformat_new_stream(format, nullptr);
        if (video_stream == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate video stream");
        video_codec = avcodec_alloc_context3(codec);
        if (video_codec == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate ProRes codec context");
        video_codec->codec_type = AVMEDIA_TYPE_VIDEO;
        video_codec->codec_id = codec->id;
        video_codec->width = static_cast<int>(width);
        video_codec->height = static_cast<int>(height);
        video_codec->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        // 90 kHz preserves sub-frame source timing while remaining a broadly
        // compatible MOV track timescale (including QuickTime readers).
        video_codec->time_base = AVRational{1, 90'000};
        video_codec->framerate = AVRational{30, 1};
        video_codec->profile = AV_PROFILE_PRORES_HQ;
        video_codec->color_range = AVCOL_RANGE_MPEG;
        video_codec->colorspace = AVCOL_SPC_BT2020_NCL;
        video_codec->color_primaries = AVCOL_PRI_UNSPECIFIED;
        video_codec->color_trc = AVCOL_TRC_UNSPECIFIED;
        video_codec->thread_count = 0;
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) video_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        require_ffmpeg(av_opt_set(video_codec->priv_data, "profile", "hq", 0), "select ProRes HQ profile");
        require_ffmpeg(avcodec_open2(video_codec, codec, nullptr), "open prores_ks encoder");
        video_stream->time_base = video_codec->time_base;
        require_ffmpeg(avcodec_parameters_from_context(video_stream->codecpar, video_codec),
                       "copy video codec parameters");
    }

    void create_audio(int sample_rate, int channels) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (codec == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg build has no PCM S16LE encoder");
        audio_stream = avformat_new_stream(format, nullptr);
        if (audio_stream == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate audio stream");
        audio_codec = avcodec_alloc_context3(codec);
        if (audio_codec == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate PCM codec context");
        audio_codec->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_codec->codec_id = codec->id;
        audio_codec->sample_fmt = AV_SAMPLE_FMT_S16;
        audio_codec->sample_rate = sample_rate;
        audio_codec->time_base = AVRational{1, sample_rate};
        av_channel_layout_default(&audio_codec->ch_layout, channels);
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) audio_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        require_ffmpeg(avcodec_open2(audio_codec, codec, nullptr), "open PCM encoder");
        audio_stream->time_base = audio_codec->time_base;
        require_ffmpeg(avcodec_parameters_from_context(audio_stream->codecpar, audio_codec),
                       "copy audio codec parameters");
    }

    void write_packet(AVCodecContext* codec, AVStream* stream, AVFrame* frame) {
        const auto input_duration = frame != nullptr ? frame->duration : 0;
        if (codec == video_codec && frame != nullptr) {
            pending_video_durations.push_back(frame->duration);
        }
        require_ffmpeg(avcodec_send_frame(codec, frame), frame == nullptr ? "flush encoder" : "send frame");
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate encoded packet");
        while (true) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                av_packet_free(&packet);
                require_ffmpeg(result, "receive encoded packet");
            }
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            if (codec == video_codec && !pending_video_durations.empty()) {
                const auto duration = pending_video_durations.front();
                pending_video_durations.pop_front();
                if (packet->duration <= 0 && duration > 0) {
                    packet->duration = av_rescale_q(duration, codec->time_base, stream->time_base);
                }
            } else if (packet->duration <= 0 && input_duration > 0) {
                packet->duration = av_rescale_q(input_duration, codec->time_base, stream->time_base);
            }
            packet->stream_index = stream->index;
            const int write_result = av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
            if (write_result < 0) {
                av_packet_free(&packet);
                require_ffmpeg(write_result, "interleave encoded packet");
            }
        }
        av_packet_free(&packet);
    }

    void write_video(Yuv422P10 input, std::int64_t timestamp_ns) {
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write video after finish");
        input.validate();
        if (input.width != static_cast<std::uint32_t>(video_codec->width) ||
            input.height != static_cast<std::uint32_t>(video_codec->height)) {
            throw Error(ErrorCode::invalid_argument, "video frame dimensions changed during encode");
        }
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate video frame");
        frame->format = video_codec->pix_fmt;
        frame->width = video_codec->width;
        frame->height = video_codec->height;
        frame->color_range = video_codec->color_range;
        frame->colorspace = video_codec->colorspace;
        frame->color_primaries = video_codec->color_primaries;
        frame->color_trc = video_codec->color_trc;
        try {
            attach_vector_plane(frame, 0, std::move(input.y), input.width);
            attach_vector_plane(frame, 1, std::move(input.cb), input.width / 2U);
            attach_vector_plane(frame, 2, std::move(input.cr), input.width / 2U);
        } catch (...) {
            av_frame_free(&frame);
            throw;
        }
        const auto pts = pts_from_ns(timestamp_ns, origin_ns, video_codec->time_base);
        if (pts <= last_video_pts) {
            av_frame_free(&frame);
            throw Error(ErrorCode::encode_failed, "video timestamps are not strictly increasing after rescale");
        }
        frame->pts = pts;
        frame->duration = last_video_pts == AV_NOPTS_VALUE
            ? av_rescale_q(1, av_inv_q(video_codec->framerate), video_codec->time_base)
            : pts - last_video_pts;
        last_video_pts = pts;
        try {
            write_packet(video_codec, video_stream, frame);
        } catch (...) {
            av_frame_free(&frame);
            throw;
        }
        av_frame_free(&frame);
    }

    void write_audio(const AudioChunk& chunk) {
        if (audio_codec == nullptr) return;
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write audio after finish");
        const auto channels = static_cast<std::size_t>(audio_codec->ch_layout.nb_channels);
        if (channels == 0U || chunk.interleaved_samples.size() % channels != 0U) {
            throw Error(ErrorCode::invalid_argument, "audio chunk is not channel-aligned");
        }
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate audio frame");
        frame->format = audio_codec->sample_fmt;
        frame->sample_rate = audio_codec->sample_rate;
        frame->nb_samples = static_cast<int>(chunk.interleaved_samples.size() / channels);
        require_ffmpeg(av_channel_layout_copy(&frame->ch_layout, &audio_codec->ch_layout),
                       "copy audio channel layout");
        try {
            require_ffmpeg(av_frame_get_buffer(frame, 0), "allocate audio frame samples");
            require_ffmpeg(av_frame_make_writable(frame), "make audio frame writable");
            std::memcpy(frame->data[0], chunk.interleaved_samples.data(),
                        chunk.interleaved_samples.size() * sizeof(std::int16_t));
            if (chunk.timestamp_ns >= 0) {
                frame->pts = pts_from_ns(chunk.timestamp_ns, origin_ns, audio_codec->time_base);
            } else {
                frame->pts = next_audio_pts;
            }
            if (frame->pts < next_audio_pts) {
                throw Error(ErrorCode::encode_failed, "audio timestamps overlap or move backward");
            }
            next_audio_pts = frame->pts + frame->nb_samples;
            write_packet(audio_codec, audio_stream, frame);
        } catch (...) {
            av_frame_free(&frame);
            throw;
        }
        av_frame_free(&frame);
    }

    void finish() {
        if (finished) return;
        write_packet(video_codec, video_stream, nullptr);
        if (audio_codec != nullptr) write_packet(audio_codec, audio_stream, nullptr);
        require_ffmpeg(av_write_trailer(format), "write MOV trailer");
        finished = true;
    }

    void cleanup() noexcept {
        if (format != nullptr && header_written && !finished) {
            av_write_trailer(format);
        }
        avcodec_free_context(&video_codec);
        avcodec_free_context(&audio_codec);
        if (format != nullptr) {
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) avio_closep(&format->pb);
            avformat_free_context(format);
            format = nullptr;
        }
    }

    std::int64_t origin_ns{};
    std::filesystem::path output_path;
    AVFormatContext* format{};
    AVCodecContext* video_codec{};
    AVCodecContext* audio_codec{};
    AVStream* video_stream{};
    AVStream* audio_stream{};
    std::deque<std::int64_t> pending_video_durations;
    std::int64_t last_video_pts{AV_NOPTS_VALUE};
    std::int64_t next_audio_pts{};
    bool header_written{};
    bool finished{};
};

FfmpegWriter::FfmpegWriter(const std::filesystem::path& output,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::int64_t timeline_origin_ns,
                           int audio_sample_rate,
                           int audio_channels)
    : impl_(std::make_unique<Impl>(output, width, height, timeline_origin_ns,
                                   audio_sample_rate, audio_channels)) {}

FfmpegWriter::~FfmpegWriter() = default;
FfmpegWriter::FfmpegWriter(FfmpegWriter&&) noexcept = default;
FfmpegWriter& FfmpegWriter::operator=(FfmpegWriter&&) noexcept = default;
void FfmpegWriter::write_video(Yuv422P10 frame, std::int64_t timestamp_ns) {
    impl_->write_video(std::move(frame), timestamp_ns);
}
void FfmpegWriter::write_audio(const AudioChunk& chunk) { impl_->write_audio(chunk); }
void FfmpegWriter::finish() { impl_->finish(); }

} // namespace mcraw
