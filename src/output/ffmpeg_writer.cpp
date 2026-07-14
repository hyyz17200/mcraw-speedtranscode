#include <mcraw/output/ffmpeg_writer.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

void free_packets(std::vector<AVPacket*>& packets) noexcept {
    for (auto& packet : packets) av_packet_free(&packet);
    packets.clear();
}

} // namespace

class FfmpegWriter::Impl {
public:
    Impl(const std::filesystem::path& output,
         std::uint32_t width,
         std::uint32_t height,
         std::int64_t timeline_origin_ns,
         int audio_sample_rate,
         int audio_channels,
         VideoEncodeConcurrency video_concurrency)
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
            create_video(width, height, video_concurrency);
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
            start_workers();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void create_video(std::uint32_t width,
                      std::uint32_t height,
                      const VideoEncodeConcurrency& concurrency) {
        const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
        if (codec == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg build has no prores_ks encoder");
        video_stream = avformat_new_stream(format, nullptr);
        if (video_stream == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate video stream");
        // ProRes is intra-only, so identically configured contexts can encode
        // independent frames concurrently; packets mux in submission order.
        const auto contexts = std::clamp<std::size_t>(concurrency.contexts, 1U, 16U);
        max_jobs_in_flight = contexts + 2U;
        video_codecs.resize(contexts, nullptr);
        for (auto& context : video_codecs) {
            context = avcodec_alloc_context3(codec);
            if (context == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate ProRes codec context");
            context->codec_type = AVMEDIA_TYPE_VIDEO;
            context->codec_id = codec->id;
            context->width = static_cast<int>(width);
            context->height = static_cast<int>(height);
            context->pix_fmt = AV_PIX_FMT_YUV422P10LE;
            // 90 kHz preserves sub-frame source timing while remaining a broadly
            // compatible MOV track timescale (including QuickTime readers).
            context->time_base = AVRational{1, 90'000};
            context->framerate = AVRational{30, 1};
            context->profile = AV_PROFILE_PRORES_HQ;
            context->color_range = AVCOL_RANGE_MPEG;
            context->colorspace = AVCOL_SPC_BT2020_NCL;
            context->color_primaries = AVCOL_PRI_UNSPECIFIED;
            context->color_trc = AVCOL_TRC_UNSPECIFIED;
            context->thread_count = std::max(concurrency.threads_per_context, 0);
            // Slice threading only: frame threading would delay packets by
            // thread_count-1 frames, breaking the one-packet-per-send contract
            // the cross-context mux ordering relies on. Frame-level overlap
            // already comes from the writer's own encoder contexts.
            context->thread_type = FF_THREAD_SLICE;
            if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            require_ffmpeg(av_opt_set(context->priv_data, "profile", "hq", 0), "select ProRes HQ profile");
            require_ffmpeg(avcodec_open2(context, codec, nullptr), "open prores_ks encoder");
        }
        video_stream->time_base = video_codecs.front()->time_base;
        require_ffmpeg(avcodec_parameters_from_context(video_stream->codecpar, video_codecs.front()),
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

    // Synchronous encode+mux used for audio frames and end-of-stream flushes.
    void write_packet(AVCodecContext* codec, AVStream* stream, AVFrame* frame) {
        const auto input_duration = frame != nullptr ? frame->duration : 0;
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
            if (packet->duration <= 0 && input_duration > 0) {
                packet->duration = av_rescale_q(input_duration, codec->time_base, stream->time_base);
            }
            packet->stream_index = stream->index;
            int write_result;
            {
                std::scoped_lock lock(mux_mutex);
                write_result = av_interleaved_write_frame(format, packet);
            }
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
        rethrow_pipeline_error();
        input.validate();
        auto* front = video_codecs.front();
        if (input.width != static_cast<std::uint32_t>(front->width) ||
            input.height != static_cast<std::uint32_t>(front->height)) {
            throw Error(ErrorCode::invalid_argument, "video frame dimensions changed during encode");
        }
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate video frame");
        frame->format = front->pix_fmt;
        frame->width = front->width;
        frame->height = front->height;
        frame->color_range = front->color_range;
        frame->colorspace = front->colorspace;
        frame->color_primaries = front->color_primaries;
        frame->color_trc = front->color_trc;
        try {
            attach_vector_plane(frame, 0, std::move(input.y), input.width);
            attach_vector_plane(frame, 1, std::move(input.cb), input.width / 2U);
            attach_vector_plane(frame, 2, std::move(input.cr), input.width / 2U);
            const auto pts = pts_from_ns(timestamp_ns, origin_ns, front->time_base);
            if (pts <= last_video_pts) {
                throw Error(ErrorCode::encode_failed, "video timestamps are not strictly increasing after rescale");
            }
            frame->pts = pts;
            frame->duration = last_video_pts == AV_NOPTS_VALUE
                ? av_rescale_q(1, av_inv_q(front->framerate), front->time_base)
                : pts - last_video_pts;
            last_video_pts = pts;

            std::unique_lock lock(pipe_mutex);
            space_cv.wait(lock, [this] {
                return pipeline_failed || jobs_in_flight < max_jobs_in_flight;
            });
            if (pipeline_failed) {
                lock.unlock();
                rethrow_pipeline_error();
            }
            job_queue.push_back({next_submit_sequence++, frame, frame->duration});
            ++jobs_in_flight;
            job_cv.notify_one();
        } catch (...) {
            av_frame_free(&frame);
            throw;
        }
    }

    void write_audio(const AudioChunk& chunk) {
        if (audio_codec == nullptr) return;
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write audio after finish");
        rethrow_pipeline_error();
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
        {
            std::unique_lock lock(pipe_mutex);
            space_cv.wait(lock, [this] { return pipeline_failed || jobs_in_flight == 0U; });
        }
        rethrow_pipeline_error();
        stop_worker_threads(false);
        rethrow_pipeline_error();
        for (auto* context : video_codecs) write_packet(context, video_stream, nullptr);
        if (audio_codec != nullptr) write_packet(audio_codec, audio_stream, nullptr);
        require_ffmpeg(av_write_trailer(format), "write MOV trailer");
        finished = true;
    }

    void cleanup() noexcept {
        stop_worker_threads(true);
        for (auto& job : job_queue) av_frame_free(&job.frame);
        job_queue.clear();
        for (auto& [sequence, packets] : completed) free_packets(packets);
        completed.clear();
        if (format != nullptr && header_written && !finished) {
            av_write_trailer(format);
        }
        for (auto& context : video_codecs) avcodec_free_context(&context);
        video_codecs.clear();
        avcodec_free_context(&audio_codec);
        if (format != nullptr) {
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) avio_closep(&format->pb);
            avformat_free_context(format);
            format = nullptr;
        }
    }

private:
    struct EncodeJob {
        std::uint64_t sequence{};
        AVFrame* frame{};
        std::int64_t duration{};
    };

    void start_workers() {
        workers.reserve(video_codecs.size());
        for (auto* context : video_codecs) {
            workers.emplace_back([this, context] { worker_main(context); });
        }
    }

    void stop_worker_threads(bool abort) noexcept {
        {
            std::scoped_lock lock(pipe_mutex);
            if (abort) abort_workers = true;
            stop_workers = true;
        }
        job_cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        workers.clear();
    }

    void worker_main(AVCodecContext* codec) noexcept {
        for (;;) {
            EncodeJob job{};
            {
                std::unique_lock lock(pipe_mutex);
                job_cv.wait(lock, [this] {
                    return abort_workers || stop_workers || !job_queue.empty();
                });
                if (abort_workers) return;
                if (job_queue.empty()) {
                    if (stop_workers) return;
                    continue;
                }
                job = job_queue.front();
                job_queue.pop_front();
            }
            try {
                encode_job(codec, job);
            } catch (...) {
                std::scoped_lock lock(pipe_mutex);
                if (worker_error == nullptr) worker_error = std::current_exception();
                pipeline_failed = true;
                job_cv.notify_all();
                space_cv.notify_all();
                return;
            }
        }
    }

    void encode_job(AVCodecContext* codec, EncodeJob job) {
        const int send_result = avcodec_send_frame(codec, job.frame);
        av_frame_free(&job.frame);
        require_ffmpeg(send_result, "send video frame");
        std::vector<AVPacket*> packets;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate encoded packet");
        try {
            while (true) {
                const int result = avcodec_receive_packet(codec, packet);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                require_ffmpeg(result, "receive video packet");
                av_packet_rescale_ts(packet, codec->time_base, video_stream->time_base);
                if (packet->duration <= 0 && job.duration > 0) {
                    packet->duration = av_rescale_q(job.duration, codec->time_base,
                                                    video_stream->time_base);
                }
                packet->stream_index = video_stream->index;
                packets.push_back(packet);
                packet = av_packet_alloc();
                if (packet == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate encoded packet");
            }
            av_packet_free(&packet);
            packet = nullptr;
            // An intra-only encoder must emit each frame's packet immediately;
            // buffering would make cross-context mux order undecidable.
            if (packets.empty()) {
                throw Error(ErrorCode::encode_failed, "ProRes encoder buffered a frame unexpectedly");
            }
            complete_job(job.sequence, packets);
        } catch (...) {
            av_packet_free(&packet);
            free_packets(packets);
            throw;
        }
    }

    void complete_job(std::uint64_t sequence, std::vector<AVPacket*>& packets) {
        std::unique_lock lock(pipe_mutex);
        completed.emplace(sequence, std::move(packets));
        packets.clear();
        // Whichever worker completes the lowest outstanding sequence drains
        // the in-order prefix; next_mux_sequence only advances under the lock.
        while (!pipeline_failed && !completed.empty() &&
               completed.begin()->first == next_mux_sequence) {
            auto ready = std::move(completed.begin()->second);
            completed.erase(completed.begin());
            lock.unlock();
            try {
                for (auto& item : ready) {
                    int write_result;
                    {
                        std::scoped_lock mux(mux_mutex);
                        write_result = av_interleaved_write_frame(format, item);
                    }
                    av_packet_free(&item);
                    require_ffmpeg(write_result, "interleave video packet");
                }
            } catch (...) {
                free_packets(ready);
                throw;
            }
            lock.lock();
            ++next_mux_sequence;
            --jobs_in_flight;
            space_cv.notify_all();
        }
    }

    void rethrow_pipeline_error() {
        std::exception_ptr error;
        {
            std::scoped_lock lock(pipe_mutex);
            error = worker_error;
        }
        if (error != nullptr) std::rethrow_exception(error);
    }

public:
    std::int64_t origin_ns{};
    std::filesystem::path output_path;
    AVFormatContext* format{};
    std::vector<AVCodecContext*> video_codecs;
    AVCodecContext* audio_codec{};
    AVStream* video_stream{};
    AVStream* audio_stream{};
    std::int64_t last_video_pts{AV_NOPTS_VALUE};
    std::int64_t next_audio_pts{};
    bool header_written{};
    bool finished{};

private:
    // Encode pipeline state; guarded by pipe_mutex unless noted otherwise.
    std::mutex pipe_mutex;
    std::condition_variable job_cv;
    std::condition_variable space_cv;
    std::deque<EncodeJob> job_queue;
    std::map<std::uint64_t, std::vector<AVPacket*>> completed;
    std::uint64_t next_submit_sequence{};
    std::uint64_t next_mux_sequence{};
    std::size_t jobs_in_flight{};
    std::size_t max_jobs_in_flight{1};
    bool stop_workers{};
    bool abort_workers{};
    bool pipeline_failed{};
    std::exception_ptr worker_error;
    std::mutex mux_mutex; // serializes every muxer write across threads
    std::vector<std::thread> workers;
};

FfmpegWriter::FfmpegWriter(const std::filesystem::path& output,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::int64_t timeline_origin_ns,
                           int audio_sample_rate,
                           int audio_channels,
                           VideoEncodeConcurrency video_concurrency)
    : impl_(std::make_unique<Impl>(output, width, height, timeline_origin_ns,
                                   audio_sample_rate, audio_channels, video_concurrency)) {}

FfmpegWriter::~FfmpegWriter() = default;
FfmpegWriter::FfmpegWriter(FfmpegWriter&&) noexcept = default;
FfmpegWriter& FfmpegWriter::operator=(FfmpegWriter&&) noexcept = default;
void FfmpegWriter::write_video(Yuv422P10 frame, std::int64_t timestamp_ns) {
    impl_->write_video(std::move(frame), timestamp_ns);
}
void FfmpegWriter::write_audio(const AudioChunk& chunk) { impl_->write_audio(chunk); }
void FfmpegWriter::finish() { impl_->finish(); }

} // namespace mcraw
