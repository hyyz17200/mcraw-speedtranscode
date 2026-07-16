#include <mcraw/output/ffmpeg_writer.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
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
#include <type_traits>
#include <utility>
#include <variant>
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
#include <mcraw/output/ffmpeg_raii.hpp>
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/output/vulkan_prores_encoder.hpp>
#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>
#endif

namespace mcraw {
namespace {

std::uint32_t prores_codec_tag(std::string_view profile) {
    if (profile == "proxy") return MKTAG('a', 'p', 'c', 'o');
    if (profile == "lt") return MKTAG('a', 'p', 'c', 's');
    if (profile == "standard") return MKTAG('a', 'p', 'c', 'n');
    if (profile == "hq") return MKTAG('a', 'p', 'c', 'h');
    if (profile == "4444") return MKTAG('a', 'p', '4', 'h');
    if (profile == "4444xq") return MKTAG('a', 'p', '4', 'x');
    throw Error(ErrorCode::invalid_argument, "unknown ProRes profile: " + std::string(profile));
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
         VideoEncodeConcurrency video_concurrency,
         FfmpegVideoBackendConfig backend_config)
        : origin_ns(timeline_origin_ns), output_path(output) {
        performance_mode = backend_config.performance_mode;
        deterministic_dither = backend_config.deterministic_dither;
        if (width == 0 || height == 0 || (width & 1U) != 0U) {
            throw Error(ErrorCode::invalid_argument, "invalid ProRes frame dimensions");
        }
        const auto path_utf8 = output.u8string();
        const std::string path_string(path_utf8.begin(), path_utf8.end());
        require_ffmpeg(avformat_alloc_output_context2(&format, nullptr, "mov", path_string.c_str()),
                       "allocate MOV output context");
        if (format == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg returned no MOV output context");

        try {
            create_video(width, height, video_concurrency, backend_config);
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
                      const VideoEncodeConcurrency& concurrency,
                      const FfmpegVideoBackendConfig& backend_config) {
        video_stream = avformat_new_stream(format, nullptr);
        if (video_stream == nullptr) throw Error(ErrorCode::encode_failed, "cannot allocate video stream");
        video_backend = backend_config.backend;
        if (video_backend == VideoBackend::vulkan) {
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
            const auto encoder_depth = std::clamp<std::size_t>(
                backend_config.async_depth, 1U, 32U);
            // Preparation now overlaps encoder submission. Two resident slots
            // cover that producer/consumer overlap without replicating the two
            // full-resolution FP32 ping sets for every encoder-delay frame.
            const auto slot_count = std::min<std::size_t>(encoder_depth, 2U);
            const auto pool_size = std::clamp<std::size_t>(
                encoder_depth * 2U + 4U, 8U, 64U);
            vulkan_job_capacity = std::clamp<std::size_t>(
                encoder_depth + 2U, 4U, 64U);
            vulkan_frame_capacity = slot_count;
            vulkan_packet_capacity = std::clamp<std::size_t>(
                encoder_depth * 2U, 8U, 128U);
            vulkan_runtime = std::make_unique<VulkanRuntime>(VulkanRuntimeConfig{
                backend_config.gpu_selector, backend_config.enable_validation});
            vulkan_frames = std::make_unique<FfmpegVulkanFrameContext>(
                *vulkan_runtime,
                FfmpegVulkanFrameContextConfig{static_cast<int>(width),
                                               static_cast<int>(height), pool_size});
            vulkan_frame_writer = std::make_unique<VulkanRgbToYuvFrameWriter>(
                *vulkan_runtime, *vulkan_frames,
                VulkanRgbToYuvConfig{width, height, backend_config.chroma_filter,
                                     backend_config.deterministic_dither,
                                     backend_config.precision, slot_count,
                                     backend_config.performance_mode});
            vulkan_encoder = std::make_unique<VulkanProResEncoder>(
                *vulkan_frames,
                VulkanProResEncoderConfig{static_cast<int>(width), static_cast<int>(height),
                                          {1, 90'000}, {30, 1}, backend_config.prores_profile,
                                          backend_config.async_depth});
            video_stream->time_base = vulkan_encoder->time_base();
            vulkan_encoder->copy_parameters_to(video_stream->codecpar);
            return;
#else
            throw Error(ErrorCode::unsupported_format,
                        "Vulkan backend selected in a build without Vulkan support");
#endif
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
        if (codec == nullptr) throw Error(ErrorCode::encode_failed, "FFmpeg build has no prores_ks encoder");
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
            require_ffmpeg(av_opt_set(context->priv_data, "profile",
                                      backend_config.prores_profile.c_str(), 0),
                           "select ProRes profile");
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
    void validate_video_packet(const AVPacket* packet) {
        if (packet == nullptr || packet->pts == AV_NOPTS_VALUE ||
            (previous_video_pts != AV_NOPTS_VALUE && packet->pts <= previous_video_pts) ||
            packet->duration <= 0) {
            throw Error(ErrorCode::encode_failed,
                        "encoded video packet has invalid PTS or duration");
        }
        previous_video_pts = packet->pts;
    }

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
                if (stream == video_stream) validate_video_packet(packet);
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
        record_pipeline_entry(video_backend == VideoBackend::vulkan
                                  ? "yuv422p10_cpu_upload"
                                  : "yuv422p10_cpu");
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            enqueue_vulkan_video(std::move(input), timestamp_ns, 0U);
            return;
        }
#endif
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

    void write_video(TargetLogRgbF32 input,
                     [[maybe_unused]] std::int64_t timestamp_ns,
                     [[maybe_unused]] std::size_t frame_index) {
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write video after finish");
        rethrow_pipeline_error();
        input.validate();
        record_pipeline_entry("target_log_f32");
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            enqueue_vulkan_video(std::move(input), timestamp_ns, frame_index);
            return;
        }
#endif
        throw Error(ErrorCode::invalid_argument,
                    "TargetLog RGB video input requires the Vulkan backend");
    }

    void write_video(VulkanCameraRgbInput input,
                     [[maybe_unused]] std::int64_t timestamp_ns,
                     [[maybe_unused]] std::size_t frame_index) {
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write video after finish");
        rethrow_pipeline_error();
        input.image.validate();
        if (!std::isfinite(input.exposure_offset_stops) ||
            !std::isfinite(input.input_scale) || input.input_scale <= 0.0 ||
            !std::isfinite(input.capture_sharpening) ||
            input.capture_sharpening < 0.0 ||
            !std::isfinite(input.capture_sharpening_threshold) ||
            input.capture_sharpening_threshold < 0.0 ||
            !std::all_of(input.camera_to_target.v.begin(),
                         input.camera_to_target.v.end(),
                         [](double value) { return std::isfinite(value); })) {
            throw Error(ErrorCode::invalid_argument,
                        "Camera RGB Vulkan parameters must be finite and valid");
        }
        record_pipeline_entry("camera_rgb_f32");
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            enqueue_vulkan_video(std::move(input), timestamp_ns, frame_index);
            return;
        }
#endif
        throw Error(ErrorCode::invalid_argument,
                    "Camera RGB video input requires the Vulkan backend");
    }

    void write_video(VulkanRawMosaicInput input,
                     [[maybe_unused]] std::int64_t timestamp_ns,
                     [[maybe_unused]] std::size_t frame_index) {
        if (finished) throw Error(ErrorCode::encode_failed, "cannot write video after finish");
        rethrow_pipeline_error();
        input.image.validate();
        input.metadata.validate_for_raw();
        if (input.image.width != input.metadata.width ||
            input.image.height != input.metadata.height ||
            input.image.cfa != input.metadata.cfa ||
            !std::isfinite(input.exposure_offset_stops) ||
            !std::isfinite(input.capture_sharpening) || input.capture_sharpening < 0.0 ||
            !std::isfinite(input.capture_sharpening_threshold) ||
            input.capture_sharpening_threshold < 0.0 ||
            !std::all_of(input.camera_to_target.v.begin(),
                         input.camera_to_target.v.end(),
                         [](double value) { return std::isfinite(value); })) {
            throw Error(ErrorCode::invalid_argument,
                        "U16 RAW Vulkan parameters must be finite and valid");
        }
        record_pipeline_entry("raw_mosaic_u16");
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            enqueue_vulkan_video(std::move(input), timestamp_ns, frame_index);
            return;
        }
#endif
        throw Error(ErrorCode::invalid_argument,
                    "U16 RAW video input requires the Vulkan backend");
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
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            stop_vulkan_threads(false);
            rethrow_vulkan_error();
        } else
#endif
        {
            std::unique_lock lock(pipe_mutex);
            space_cv.wait(lock, [this] { return pipeline_failed || jobs_in_flight == 0U; });
            lock.unlock();
            rethrow_pipeline_error();
            stop_worker_threads(false);
            rethrow_pipeline_error();
            for (auto* context : video_codecs) write_packet(context, video_stream, nullptr);
        }
        if (audio_codec != nullptr) write_packet(audio_codec, audio_stream, nullptr);
        require_ffmpeg(av_write_trailer(format), "write MOV trailer");
        finished = true;
    }

    void cleanup() noexcept {
        stop_worker_threads(true);
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        stop_vulkan_threads(true);
#endif
        for (auto& job : job_queue) av_frame_free(&job.frame);
        job_queue.clear();
        for (auto& [sequence, packets] : completed) free_packets(packets);
        completed.clear();
        if (format != nullptr && header_written && !finished) {
            av_write_trailer(format);
        }
        for (auto& context : video_codecs) avcodec_free_context(&context);
        video_codecs.clear();
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        vulkan_frame_writer.reset();
        vulkan_encoder.reset();
        vulkan_frames.reset();
        vulkan_runtime.reset();
#endif
        avcodec_free_context(&audio_codec);
        if (format != nullptr) {
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) avio_closep(&format->pb);
            avformat_free_context(format);
            format = nullptr;
        }
    }

    [[nodiscard]] FfmpegWriterTelemetry telemetry() const {
        FfmpegWriterTelemetry result;
        result.video_packets = video_packet_count.load();
        if (video_backend == VideoBackend::vulkan) {
            result.effective_async_depth = vulkan_encoder != nullptr
                ? vulkan_encoder->async_depth() : 0U;
            result.compute_pool_size = result.effective_async_depth;
            result.compute_queue_family = vulkan_runtime != nullptr
                ? vulkan_runtime->compute_queue_family() : 0U;
            result.compute_queue_index = 0U;
        }
        result.pipeline_entry = pipeline_entry;
        result.performance_mode = std::string(to_string(performance_mode));
        result.intermediate_storage = "fp32";
        result.di_implementation = performance_mode == GpuPerformanceMode::fast
            ? "fp32_analytic" : "fp32_lut";
        result.dither_mode = deterministic_dither ? "deterministic" : "disabled";
        if (pipeline_entry == "target_log_f32" ||
            pipeline_entry == "camera_rgb_f32") {
            result.pipeline_precision = "fp32/precise";
            result.demosaic_location = "cpu";
            result.demosaic_implementation = "cpu_configured";
            result.color_solution_location = "cpu_fp64";
        } else if (pipeline_entry == "raw_mosaic_u16") {
            const bool mixed_storage =
                performance_mode != GpuPerformanceMode::precise;
            result.pipeline_precision = mixed_storage
                ? "fp16-storage/fp32-compute" : "fp32/precise";
            result.intermediate_storage = mixed_storage ? "fp16" : "fp32";
            result.demosaic_location = "gpu_rcd_precise";
            result.demosaic_implementation = "gpu_rcd_precise";
            result.color_solution_location = "cpu_fp64";
        } else if (pipeline_entry == "yuv422p10_cpu" ||
                   pipeline_entry == "yuv422p10_cpu_upload") {
            result.pipeline_precision = "cpu_reference";
            result.demosaic_location = "cpu";
            result.demosaic_implementation = "cpu_configured";
            result.color_solution_location = "cpu_fp64";
        }
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan && vulkan_encoder && vulkan_runtime) {
            const auto counters = vulkan_encoder->telemetry();
            const auto& device = vulkan_runtime->device();
            result.backend = "prores_ks_vulkan";
            result.gpu_resident = counters.gpu_resident;
            result.upload_frames = counters.upload_frames;
            result.readback_frames = counters.readback_frames;
            result.direct_frames = counters.direct_frames;
            if (vulkan_frame_writer) {
                const auto frame_counters = vulkan_frame_writer->telemetry();
                result.rgb_upload_bytes = frame_counters.rgb_upload_bytes;
                result.u16_raw_upload_bytes = frame_counters.u16_raw_upload_bytes;
                result.fp32_rgb_upload_bytes = frame_counters.rgb_upload_bytes;
                if (pipeline_entry == "target_log_f32") {
                    result.target_log_fp32_upload_bytes =
                        frame_counters.rgb_upload_bytes;
                } else if (pipeline_entry == "camera_rgb_f32") {
                    result.camera_rgb_fp32_upload_bytes =
                        frame_counters.rgb_upload_bytes;
                }
                result.job_queue_backpressure_waits =
                    vulkan_job_backpressure_waits;
                result.job_queue_backpressure_wait_ms =
                    vulkan_job_backpressure_wait_ms;
                result.packet_queue_backpressure_waits =
                    vulkan_packet_backpressure_waits;
                result.packet_queue_backpressure_wait_ms =
                    vulkan_packet_backpressure_wait_ms;
                result.slot_backpressure_waits = frame_counters.backpressure_waits;
                result.slot_backpressure_wait_ms = frame_counters.backpressure_wait_ms;
                result.backpressure_waits = result.job_queue_backpressure_waits +
                    result.packet_queue_backpressure_waits +
                    result.slot_backpressure_waits;
                result.backpressure_wait_ms = result.job_queue_backpressure_wait_ms +
                    result.packet_queue_backpressure_wait_ms +
                    result.slot_backpressure_wait_ms;
                result.frame_allocation_samples =
                    frame_counters.frame_allocation_samples;
                result.frame_allocation_total_ms =
                    frame_counters.frame_allocation_total_ms;
                result.frame_allocation_mean_ms =
                    frame_counters.frame_allocation_mean_ms;
                result.frame_allocation_max_ms =
                    frame_counters.frame_allocation_max_ms;
                result.queue_lock_wait_samples =
                    frame_counters.queue_lock_wait_samples;
                result.queue_lock_wait_total_ms =
                    frame_counters.queue_lock_wait_total_ms;
                result.queue_lock_wait_mean_ms =
                    frame_counters.queue_lock_wait_mean_ms;
                result.queue_lock_wait_max_ms =
                    frame_counters.queue_lock_wait_max_ms;
                result.queue_submit_samples = frame_counters.queue_submit_samples;
                result.queue_submit_total_ms = frame_counters.queue_submit_total_ms;
                result.queue_submit_mean_ms = frame_counters.queue_submit_mean_ms;
                result.queue_submit_max_ms = frame_counters.queue_submit_max_ms;
                result.resident_slot_count = frame_counters.slot_count;
                result.gpu_timestamps_supported =
                    frame_counters.gpu_timestamps_supported;
                result.rgb_to_yuv_gpu_timestamp_samples =
                    frame_counters.gpu_timestamp_samples;
                result.rgb_to_yuv_gpu_total_ms = frame_counters.gpu_total_ms;
                result.rgb_to_yuv_gpu_mean_ms = frame_counters.gpu_mean_ms;
                result.rgb_to_yuv_gpu_p50_ms = frame_counters.gpu_p50_ms;
                result.rgb_to_yuv_gpu_p95_ms = frame_counters.gpu_p95_ms;
                result.rgb_to_yuv_gpu_p99_ms = frame_counters.gpu_p99_ms;
                result.rgb_to_yuv_gpu_min_ms = frame_counters.gpu_min_ms;
                result.rgb_to_yuv_gpu_max_ms = frame_counters.gpu_max_ms;
                result.camera_to_dwg_gpu_timestamp_samples =
                    frame_counters.camera_to_dwg_timestamp_samples;
                result.camera_to_dwg_gpu_total_ms =
                    frame_counters.camera_to_dwg_gpu_total_ms;
                result.camera_to_dwg_gpu_mean_ms =
                    frame_counters.camera_to_dwg_gpu_mean_ms;
                result.camera_to_dwg_gpu_p50_ms =
                    frame_counters.camera_to_dwg_gpu_p50_ms;
                result.camera_to_dwg_gpu_p95_ms =
                    frame_counters.camera_to_dwg_gpu_p95_ms;
                result.camera_to_dwg_gpu_p99_ms =
                    frame_counters.camera_to_dwg_gpu_p99_ms;
                result.camera_to_dwg_gpu_min_ms =
                    frame_counters.camera_to_dwg_gpu_min_ms;
                result.camera_to_dwg_gpu_max_ms =
                    frame_counters.camera_to_dwg_gpu_max_ms;
                result.capture_sharpening_gpu_timestamp_samples =
                    frame_counters.capture_sharpening_timestamp_samples;
                result.capture_sharpening_gpu_total_ms =
                    frame_counters.capture_sharpening_gpu_total_ms;
                result.capture_sharpening_gpu_mean_ms =
                    frame_counters.capture_sharpening_gpu_mean_ms;
                result.capture_sharpening_gpu_p50_ms =
                    frame_counters.capture_sharpening_gpu_p50_ms;
                result.capture_sharpening_gpu_p95_ms =
                    frame_counters.capture_sharpening_gpu_p95_ms;
                result.capture_sharpening_gpu_p99_ms =
                    frame_counters.capture_sharpening_gpu_p99_ms;
                result.capture_sharpening_gpu_min_ms =
                    frame_counters.capture_sharpening_gpu_min_ms;
                result.capture_sharpening_gpu_max_ms =
                    frame_counters.capture_sharpening_gpu_max_ms;
                result.davinci_intermediate_gpu_timestamp_samples =
                    frame_counters.davinci_intermediate_timestamp_samples;
                result.davinci_intermediate_gpu_total_ms =
                    frame_counters.davinci_intermediate_gpu_total_ms;
                result.davinci_intermediate_gpu_mean_ms =
                    frame_counters.davinci_intermediate_gpu_mean_ms;
                result.davinci_intermediate_gpu_p50_ms =
                    frame_counters.davinci_intermediate_gpu_p50_ms;
                result.davinci_intermediate_gpu_p95_ms =
                    frame_counters.davinci_intermediate_gpu_p95_ms;
                result.davinci_intermediate_gpu_p99_ms =
                    frame_counters.davinci_intermediate_gpu_p99_ms;
                result.davinci_intermediate_gpu_min_ms =
                    frame_counters.davinci_intermediate_gpu_min_ms;
                result.davinci_intermediate_gpu_max_ms =
                    frame_counters.davinci_intermediate_gpu_max_ms;
                result.raw_calibration_gpu_timestamp_samples =
                    frame_counters.raw_calibration_timestamp_samples;
                result.raw_calibration_gpu_total_ms =
                    frame_counters.raw_calibration_gpu_total_ms;
                result.raw_calibration_gpu_mean_ms =
                    frame_counters.raw_calibration_gpu_mean_ms;
                result.raw_calibration_gpu_p50_ms =
                    frame_counters.raw_calibration_gpu_p50_ms;
                result.raw_calibration_gpu_p95_ms =
                    frame_counters.raw_calibration_gpu_p95_ms;
                result.raw_calibration_gpu_p99_ms =
                    frame_counters.raw_calibration_gpu_p99_ms;
                result.raw_calibration_gpu_min_ms =
                    frame_counters.raw_calibration_gpu_min_ms;
                result.raw_calibration_gpu_max_ms =
                    frame_counters.raw_calibration_gpu_max_ms;
                result.rcd_demosaic_gpu_timestamp_samples =
                    frame_counters.rcd_demosaic_timestamp_samples;
                result.rcd_demosaic_gpu_total_ms =
                    frame_counters.rcd_demosaic_gpu_total_ms;
                result.rcd_demosaic_gpu_mean_ms =
                    frame_counters.rcd_demosaic_gpu_mean_ms;
                result.rcd_demosaic_gpu_p50_ms =
                    frame_counters.rcd_demosaic_gpu_p50_ms;
                result.rcd_demosaic_gpu_p95_ms =
                    frame_counters.rcd_demosaic_gpu_p95_ms;
                result.rcd_demosaic_gpu_p99_ms =
                    frame_counters.rcd_demosaic_gpu_p99_ms;
                result.rcd_demosaic_gpu_min_ms =
                    frame_counters.rcd_demosaic_gpu_min_ms;
                result.rcd_demosaic_gpu_max_ms =
                    frame_counters.rcd_demosaic_gpu_max_ms;
                result.control_status_read_bytes =
                    frame_counters.control_status_read_bytes;
                result.control_status_failures =
                    frame_counters.control_status_failures;
            }
            result.gpu_queue_capacity = vulkan_job_capacity;
            result.gpu_queue_max_depth = vulkan_job_max_depth;
            result.packet_queue_capacity = vulkan_packet_capacity;
            result.packet_queue_max_depth = vulkan_packet_max_depth;
            result.prepared_frame_queue_capacity = vulkan_frame_capacity;
            result.prepared_frame_queue_max_depth = vulkan_frame_max_depth;
            result.job_queue_latency_samples = vulkan_job_queue_latency_samples;
            result.job_queue_latency_total_ms = vulkan_job_queue_latency_total_ms;
            result.job_queue_latency_mean_ms = vulkan_job_queue_latency_mean_ms;
            result.job_queue_latency_max_ms = vulkan_job_queue_latency_max_ms;
            result.frame_pack_samples = vulkan_frame_pack_samples;
            result.frame_pack_total_ms = vulkan_frame_pack_total_ms;
            result.frame_pack_mean_ms = vulkan_frame_pack_mean_ms;
            result.frame_pack_max_ms = vulkan_frame_pack_max_ms;
            result.encoder_send_samples = vulkan_encoder_send_samples;
            result.encoder_send_total_ms = vulkan_encoder_send_total_ms;
            result.encoder_send_mean_ms = vulkan_encoder_send_mean_ms;
            result.encoder_send_max_ms = vulkan_encoder_send_max_ms;
            result.encoder_receive_samples = vulkan_encoder_receive_samples;
            result.encoder_receive_total_ms = vulkan_encoder_receive_total_ms;
            result.encoder_receive_mean_ms = vulkan_encoder_receive_mean_ms;
            result.encoder_receive_max_ms = vulkan_encoder_receive_max_ms;
            result.mux_bytes = vulkan_mux_bytes;
            result.compressed_packet_download_bytes = vulkan_mux_bytes;
            if (vulkan_mux_wall_ms > 0.0) {
                result.mux_megabytes_per_second =
                    (static_cast<double>(vulkan_mux_bytes) / (1024.0 * 1024.0)) /
                    (vulkan_mux_wall_ms / 1000.0);
            }
            result.gpu_name = device.name;
            result.gpu_uuid = device.uuid;
            result.gpu_driver = device.driver_name + " " + device.driver_info;
        }
#endif
        return result;
    }

private:
    void record_pipeline_entry(std::string_view entry) {
        if (pipeline_entry == "uninitialized") {
            pipeline_entry = entry;
            return;
        }
        if (pipeline_entry != entry) {
            throw Error(ErrorCode::invalid_argument,
                        "video pipeline input entry changed within one MOV");
        }
    }

    static void record_wall_sample(double milliseconds,
                                   std::uint64_t& samples,
                                   double& total,
                                   double& mean,
                                   double& maximum) {
        ++samples;
        total += milliseconds;
        mean = total / static_cast<double>(samples);
        maximum = std::max(maximum, milliseconds);
    }

    struct EncodeJob {
        std::uint64_t sequence{};
        AVFrame* frame{};
        std::int64_t duration{};
    };

#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
    using VulkanInput = std::variant<Yuv422P10, TargetLogRgbF32,
                                     VulkanCameraRgbInput, VulkanRawMosaicInput>;

    struct VulkanJob {
        VulkanInput input;
        std::int64_t timestamp_ns{};
        std::size_t frame_index{};
        std::chrono::steady_clock::time_point queued_at{};
    };

    struct VulkanPreparedJob {
        VideoFrame frame;
        std::int64_t pts{};
        std::int64_t duration{};
    };

    void enqueue_vulkan_video(Yuv422P10 input,
                              std::int64_t timestamp_ns,
                              std::size_t frame_index) {
        enqueue_vulkan_job({std::move(input), timestamp_ns, frame_index});
    }

    void enqueue_vulkan_video(TargetLogRgbF32 input,
                              std::int64_t timestamp_ns,
                              std::size_t frame_index) {
        enqueue_vulkan_job({std::move(input), timestamp_ns, frame_index});
    }

    void enqueue_vulkan_video(VulkanCameraRgbInput input,
                              std::int64_t timestamp_ns,
                              std::size_t frame_index) {
        enqueue_vulkan_job({std::move(input), timestamp_ns, frame_index});
    }

    void enqueue_vulkan_video(VulkanRawMosaicInput input,
                              std::int64_t timestamp_ns,
                              std::size_t frame_index) {
        enqueue_vulkan_job({std::move(input), timestamp_ns, frame_index});
    }

    void enqueue_vulkan_job(VulkanJob job) {
        if (!vulkan_frames || !vulkan_encoder || !vulkan_frame_writer) {
            throw Error(ErrorCode::encode_failed, "Vulkan video pipeline is not initialized");
        }
        const auto dimensions_match = std::visit([this](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, VulkanCameraRgbInput> ||
                          std::is_same_v<Input, VulkanRawMosaicInput>) {
                return input.image.width ==
                           static_cast<std::uint32_t>(vulkan_frames->width()) &&
                       input.image.height ==
                           static_cast<std::uint32_t>(vulkan_frames->height());
            } else {
                return input.width ==
                           static_cast<std::uint32_t>(vulkan_frames->width()) &&
                       input.height ==
                           static_cast<std::uint32_t>(vulkan_frames->height());
            }
        }, job.input);
        if (!dimensions_match) {
            throw Error(ErrorCode::invalid_argument,
                        "video frame dimensions changed during encode");
        }
        const auto wait_start = std::chrono::steady_clock::now();
        std::unique_lock lock(vulkan_mutex);
        const bool full = vulkan_jobs.size() >= vulkan_job_capacity;
        vulkan_job_space_cv.wait(lock, [this] {
            return vulkan_failed || vulkan_abort ||
                   vulkan_jobs.size() < vulkan_job_capacity;
        });
        if (full) {
            ++vulkan_job_backpressure_waits;
            vulkan_job_backpressure_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start).count();
        }
        if (vulkan_failed || vulkan_abort) {
            lock.unlock();
            rethrow_vulkan_error();
            throw Error(ErrorCode::encode_failed, "Vulkan pipeline was cancelled");
        }
        job.queued_at = std::chrono::steady_clock::now();
        vulkan_jobs.push_back(std::move(job));
        vulkan_job_max_depth = std::max(vulkan_job_max_depth, vulkan_jobs.size());
        lock.unlock();
        vulkan_job_cv.notify_one();
    }

    FrameMetadata make_vulkan_metadata(std::int64_t timestamp_ns) {
        const auto time_base = vulkan_encoder->time_base();
        const auto pts = pts_from_ns(timestamp_ns, origin_ns, time_base);
        if (pts <= last_video_pts) {
            throw Error(ErrorCode::encode_failed,
                        "video timestamps are not strictly increasing after rescale");
        }
        const auto duration = last_video_pts == AV_NOPTS_VALUE
            ? av_rescale_q(1, AVRational{1, 30}, time_base)
            : pts - last_video_pts;
        last_video_pts = pts;
        return {vulkan_frames->width(), vulkan_frames->height(), pts, duration, time_base,
                AVCOL_PRI_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG,
                AVCHROMA_LOC_UNSPECIFIED};
    }

    VulkanPreparedJob prepare_vulkan_job(VulkanJob job) {
        auto metadata = make_vulkan_metadata(job.timestamp_ns);
        if (auto* yuv = std::get_if<Yuv422P10>(&job.input)) {
            auto frame = make_av_frame();
            frame->format = AV_PIX_FMT_YUV422P10LE;
            frame->width = vulkan_frames->width();
            frame->height = vulkan_frames->height();
            frame->pts = metadata.pts;
            frame->duration = metadata.duration;
            frame->color_range = metadata.range;
            frame->colorspace = metadata.matrix;
            frame->color_primaries = metadata.primaries;
            frame->color_trc = metadata.transfer;
            attach_vector_plane(frame.get(), 0, std::move(yuv->y), yuv->width);
            attach_vector_plane(frame.get(), 1, std::move(yuv->cb), yuv->width / 2U);
            attach_vector_plane(frame.get(), 2, std::move(yuv->cr), yuv->width / 2U);
            return {CpuVideoFrame{metadata, AV_PIX_FMT_YUV422P10LE,
                                  std::move(frame)}, metadata.pts, metadata.duration};
        } else if (auto* rgb = std::get_if<TargetLogRgbF32>(&job.input)) {
            const auto pack_start = std::chrono::steady_clock::now();
            auto frame = vulkan_frame_writer->pack(*rgb, job.frame_index, metadata);
            record_wall_sample(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - pack_start).count(),
                               vulkan_frame_pack_samples, vulkan_frame_pack_total_ms,
                               vulkan_frame_pack_mean_ms, vulkan_frame_pack_max_ms);
            return {std::move(frame), metadata.pts, metadata.duration};
        } else if (auto* camera = std::get_if<VulkanCameraRgbInput>(&job.input)) {
            const auto pack_start = std::chrono::steady_clock::now();
            auto frame = vulkan_frame_writer->pack_camera_rgb(
                camera->image, camera->camera_to_target,
                camera->exposure_offset_stops, camera->input_scale,
                camera->capture_sharpening,
                camera->capture_sharpening_threshold,
                camera->negative_policy, job.frame_index, metadata);
            record_wall_sample(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - pack_start).count(),
                               vulkan_frame_pack_samples, vulkan_frame_pack_total_ms,
                               vulkan_frame_pack_mean_ms, vulkan_frame_pack_max_ms);
            return {std::move(frame), metadata.pts, metadata.duration};
        } else if (auto* raw = std::get_if<VulkanRawMosaicInput>(&job.input)) {
            const auto pack_start = std::chrono::steady_clock::now();
            auto frame = vulkan_frame_writer->pack_raw_mosaic(
                raw->image, raw->metadata, raw->camera_to_target,
                raw->exposure_offset_stops, raw->capture_sharpening,
                raw->capture_sharpening_threshold, raw->negative_policy,
                job.frame_index, metadata);
            record_wall_sample(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - pack_start).count(),
                               vulkan_frame_pack_samples, vulkan_frame_pack_total_ms,
                               vulkan_frame_pack_mean_ms, vulkan_frame_pack_max_ms);
            return {std::move(frame), metadata.pts, metadata.duration};
        } else {
            throw Error(ErrorCode::unsupported_format,
                        "unsupported Vulkan writer input variant");
        }
    }

    void process_vulkan_frame(VulkanPreparedJob prepared) {
        vulkan_packet_durations[prepared.pts] = prepared.duration;
        const auto send_start = std::chrono::steady_clock::now();
        vulkan_encoder->send(std::move(prepared.frame));
        record_wall_sample(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - send_start).count(),
                           vulkan_encoder_send_samples, vulkan_encoder_send_total_ms,
                           vulkan_encoder_send_mean_ms, vulkan_encoder_send_max_ms);
        const auto receive_start = std::chrono::steady_clock::now();
        auto packets = vulkan_encoder->drain();
        record_wall_sample(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - receive_start).count(),
                           vulkan_encoder_receive_samples,
                           vulkan_encoder_receive_total_ms,
                           vulkan_encoder_receive_mean_ms,
                           vulkan_encoder_receive_max_ms);
        enqueue_vulkan_packets(std::move(packets));
    }

    void prepare_vulkan_packets(std::vector<EncodedPacket>& packets) {
        for (auto& encoded : packets) {
            auto* packet = encoded.packet.get();
            if (packet == nullptr) continue;
            const auto found = vulkan_packet_durations.find(packet->pts);
            if (packet->duration <= 0 && found != vulkan_packet_durations.end()) {
                packet->duration = found->second;
            }
            if (found != vulkan_packet_durations.end()) {
                vulkan_packet_durations.erase(found);
            }
        }
    }

    void enqueue_vulkan_packets(std::vector<EncodedPacket> packets) {
        if (packets.empty()) return;
        prepare_vulkan_packets(packets);
        const auto wait_start = std::chrono::steady_clock::now();
        std::unique_lock lock(vulkan_mutex);
        const bool full = vulkan_packets.size() >= vulkan_packet_capacity;
        vulkan_packet_space_cv.wait(lock, [this] {
            return vulkan_failed || vulkan_abort ||
                   vulkan_packets.size() < vulkan_packet_capacity;
        });
        if (full) {
            ++vulkan_packet_backpressure_waits;
            vulkan_packet_backpressure_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start).count();
        }
        if (vulkan_failed || vulkan_abort) return;
        vulkan_packets.push_back(std::move(packets));
        vulkan_packet_max_depth = std::max(vulkan_packet_max_depth,
                                           vulkan_packets.size());
        lock.unlock();
        vulkan_packet_cv.notify_one();
    }

    void write_vulkan_packets(std::vector<EncodedPacket> packets) {
        for (auto& encoded : packets) {
            auto* packet = encoded.packet.get();
            if (packet == nullptr) continue;
            const auto packet_bytes = static_cast<std::uint64_t>(packet->size);
            av_packet_rescale_ts(packet, encoded.time_base, video_stream->time_base);
            packet->stream_index = video_stream->index;
            validate_video_packet(packet);
            int result;
            {
                std::scoped_lock lock(mux_mutex);
                result = av_interleaved_write_frame(format, packet);
            }
            require_ffmpeg(result, "interleave Vulkan ProRes packet");
            vulkan_mux_bytes += packet_bytes;
            ++video_packet_count;
        }
    }

    void start_vulkan_threads() {
        vulkan_prepare_thread = std::thread([this] { vulkan_prepare_main(); });
        vulkan_gpu_thread = std::thread([this] { vulkan_gpu_main(); });
        vulkan_packet_thread = std::thread([this] { vulkan_packet_main(); });
    }

    void enqueue_vulkan_frame(VulkanPreparedJob prepared) {
        std::unique_lock lock(vulkan_mutex);
        vulkan_frame_space_cv.wait(lock, [this] {
            return vulkan_abort || vulkan_frames_ready.size() < vulkan_frame_capacity;
        });
        if (vulkan_abort) {
            throw Error(ErrorCode::encode_failed, "Vulkan pipeline was cancelled");
        }
        vulkan_frames_ready.push_back(std::move(prepared));
        vulkan_frame_max_depth = std::max(vulkan_frame_max_depth,
                                          vulkan_frames_ready.size());
        lock.unlock();
        vulkan_frame_cv.notify_one();
    }

    void fail_vulkan_pipeline(std::exception_ptr error) noexcept {
        {
            std::scoped_lock lock(vulkan_mutex);
            if (vulkan_worker_error == nullptr) vulkan_worker_error = error;
            vulkan_failed = true;
            vulkan_abort = true;
            vulkan_jobs.clear();
            vulkan_frames_ready.clear();
            vulkan_packets.clear();
        }
        vulkan_job_cv.notify_all();
        vulkan_job_space_cv.notify_all();
        vulkan_frame_cv.notify_all();
        vulkan_frame_space_cv.notify_all();
        vulkan_packet_cv.notify_all();
        vulkan_packet_space_cv.notify_all();
    }

    void vulkan_prepare_main() noexcept {
        try {
            for (;;) {
                VulkanJob job;
                {
                    std::unique_lock lock(vulkan_mutex);
                    vulkan_job_cv.wait(lock, [this] {
                        return vulkan_abort || vulkan_stop || !vulkan_jobs.empty();
                    });
                    if (vulkan_abort) return;
                    if (vulkan_jobs.empty()) {
                        if (vulkan_stop) break;
                        continue;
                    }
                    job = std::move(vulkan_jobs.front());
                    vulkan_jobs.pop_front();
                }
                vulkan_job_space_cv.notify_all();
                record_wall_sample(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - job.queued_at).count(),
                                   vulkan_job_queue_latency_samples,
                                   vulkan_job_queue_latency_total_ms,
                                   vulkan_job_queue_latency_mean_ms,
                                   vulkan_job_queue_latency_max_ms);
                enqueue_vulkan_frame(prepare_vulkan_job(std::move(job)));
            }
            vulkan_frame_writer->wait();
            {
                std::scoped_lock lock(vulkan_mutex);
                vulkan_prepare_done = true;
            }
            vulkan_frame_cv.notify_all();
        } catch (...) {
            fail_vulkan_pipeline(std::current_exception());
        }
    }

    void vulkan_gpu_main() noexcept {
        try {
            for (;;) {
                VulkanPreparedJob prepared;
                {
                    std::unique_lock lock(vulkan_mutex);
                    vulkan_frame_cv.wait(lock, [this] {
                        return vulkan_abort || vulkan_prepare_done ||
                               !vulkan_frames_ready.empty();
                    });
                    if (vulkan_abort) return;
                    if (vulkan_frames_ready.empty()) {
                        if (vulkan_prepare_done) break;
                        continue;
                    }
                    prepared = std::move(vulkan_frames_ready.front());
                    vulkan_frames_ready.pop_front();
                }
                vulkan_frame_space_cv.notify_all();
                process_vulkan_frame(std::move(prepared));
            }
            enqueue_vulkan_packets(vulkan_encoder->flush());
            {
                std::scoped_lock lock(vulkan_mutex);
                vulkan_gpu_done = true;
            }
            vulkan_packet_cv.notify_all();
        } catch (...) {
            fail_vulkan_pipeline(std::current_exception());
        }
    }

    void vulkan_packet_main() noexcept {
        try {
            for (;;) {
                std::vector<EncodedPacket> packets;
                {
                    std::unique_lock lock(vulkan_mutex);
                    vulkan_packet_cv.wait(lock, [this] {
                        return vulkan_abort || vulkan_gpu_done || !vulkan_packets.empty();
                    });
                    if (vulkan_abort) return;
                    if (vulkan_packets.empty()) {
                        if (vulkan_gpu_done) return;
                        continue;
                    }
                    packets = std::move(vulkan_packets.front());
                    vulkan_packets.pop_front();
                }
                vulkan_packet_space_cv.notify_all();
                const auto start = std::chrono::steady_clock::now();
                write_vulkan_packets(std::move(packets));
                vulkan_mux_wall_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
            }
        } catch (...) {
            fail_vulkan_pipeline(std::current_exception());
        }
    }

    void stop_vulkan_threads(bool abort) noexcept {
        if (!vulkan_prepare_thread.joinable() && !vulkan_gpu_thread.joinable() &&
            !vulkan_packet_thread.joinable()) return;
        {
            std::scoped_lock lock(vulkan_mutex);
            if (abort) {
                vulkan_abort = true;
                vulkan_jobs.clear();
                vulkan_frames_ready.clear();
                vulkan_packets.clear();
            }
            vulkan_stop = true;
        }
        vulkan_job_cv.notify_all();
        vulkan_job_space_cv.notify_all();
        vulkan_frame_cv.notify_all();
        vulkan_frame_space_cv.notify_all();
        vulkan_packet_cv.notify_all();
        vulkan_packet_space_cv.notify_all();
        if (vulkan_prepare_thread.joinable()) vulkan_prepare_thread.join();
        vulkan_frame_cv.notify_all();
        if (vulkan_gpu_thread.joinable()) vulkan_gpu_thread.join();
        vulkan_packet_cv.notify_all();
        if (vulkan_packet_thread.joinable()) vulkan_packet_thread.join();
    }

    void rethrow_vulkan_error() {
        std::exception_ptr error;
        {
            std::scoped_lock lock(vulkan_mutex);
            error = vulkan_worker_error;
        }
        if (error != nullptr) std::rethrow_exception(error);
    }
#endif

    void start_workers() {
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            start_vulkan_threads();
            return;
        }
#endif
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
                        validate_video_packet(item);
                        write_result = av_interleaved_write_frame(format, item);
                    }
                    av_packet_free(&item);
                    require_ffmpeg(write_result, "interleave video packet");
                    ++video_packet_count;
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
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
        if (video_backend == VideoBackend::vulkan) {
            rethrow_vulkan_error();
            return;
        }
#endif
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
    GpuPerformanceMode performance_mode{GpuPerformanceMode::precise};
    bool deterministic_dither{true};
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
    VideoBackend video_backend{VideoBackend::cpu};
    std::atomic<std::uint64_t> video_packet_count{};
    std::int64_t previous_video_pts{AV_NOPTS_VALUE};
    std::string pipeline_entry{"uninitialized"};
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
    std::unique_ptr<VulkanRuntime> vulkan_runtime;
    std::unique_ptr<FfmpegVulkanFrameContext> vulkan_frames;
    std::unique_ptr<VulkanRgbToYuvFrameWriter> vulkan_frame_writer;
    std::unique_ptr<VulkanProResEncoder> vulkan_encoder;
    std::map<std::int64_t, std::int64_t> vulkan_packet_durations;
    std::mutex vulkan_mutex;
    std::condition_variable vulkan_job_cv;
    std::condition_variable vulkan_job_space_cv;
    std::condition_variable vulkan_frame_cv;
    std::condition_variable vulkan_frame_space_cv;
    std::condition_variable vulkan_packet_cv;
    std::condition_variable vulkan_packet_space_cv;
    std::deque<VulkanJob> vulkan_jobs;
    std::deque<VulkanPreparedJob> vulkan_frames_ready;
    std::deque<std::vector<EncodedPacket>> vulkan_packets;
    std::thread vulkan_prepare_thread;
    std::thread vulkan_gpu_thread;
    std::thread vulkan_packet_thread;
    std::exception_ptr vulkan_worker_error;
    std::size_t vulkan_job_capacity{4};
    std::size_t vulkan_job_max_depth{};
    std::size_t vulkan_frame_capacity{1};
    std::size_t vulkan_frame_max_depth{};
    std::size_t vulkan_packet_capacity{8};
    std::size_t vulkan_packet_max_depth{};
    std::uint64_t vulkan_job_backpressure_waits{};
    double vulkan_job_backpressure_wait_ms{};
    std::uint64_t vulkan_packet_backpressure_waits{};
    double vulkan_packet_backpressure_wait_ms{};
    std::uint64_t vulkan_job_queue_latency_samples{};
    double vulkan_job_queue_latency_total_ms{};
    double vulkan_job_queue_latency_mean_ms{};
    double vulkan_job_queue_latency_max_ms{};
    std::uint64_t vulkan_frame_pack_samples{};
    double vulkan_frame_pack_total_ms{};
    double vulkan_frame_pack_mean_ms{};
    double vulkan_frame_pack_max_ms{};
    std::uint64_t vulkan_encoder_send_samples{};
    double vulkan_encoder_send_total_ms{};
    double vulkan_encoder_send_mean_ms{};
    double vulkan_encoder_send_max_ms{};
    std::uint64_t vulkan_encoder_receive_samples{};
    double vulkan_encoder_receive_total_ms{};
    double vulkan_encoder_receive_mean_ms{};
    double vulkan_encoder_receive_max_ms{};
    std::uint64_t vulkan_mux_bytes{};
    double vulkan_mux_wall_ms{};
    bool vulkan_stop{};
    bool vulkan_abort{};
    bool vulkan_failed{};
    bool vulkan_prepare_done{};
    bool vulkan_gpu_done{};
#endif
};

FfmpegWriter::FfmpegWriter(const std::filesystem::path& output,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::int64_t timeline_origin_ns,
                           int audio_sample_rate,
                           int audio_channels,
                           VideoEncodeConcurrency video_concurrency,
                           FfmpegVideoBackendConfig backend)
    : impl_(std::make_unique<Impl>(output, width, height, timeline_origin_ns,
                                   audio_sample_rate, audio_channels, video_concurrency,
                                   std::move(backend))) {}

FfmpegWriter::~FfmpegWriter() = default;
FfmpegWriter::FfmpegWriter(FfmpegWriter&&) noexcept = default;
FfmpegWriter& FfmpegWriter::operator=(FfmpegWriter&&) noexcept = default;
void FfmpegWriter::write_video(Yuv422P10 frame, std::int64_t timestamp_ns) {
    impl_->write_video(std::move(frame), timestamp_ns);
}
void FfmpegWriter::write_video(TargetLogRgbF32 frame,
                               std::int64_t timestamp_ns,
                               std::size_t frame_index) {
    impl_->write_video(std::move(frame), timestamp_ns, frame_index);
}
void FfmpegWriter::write_video(VulkanCameraRgbInput frame,
                               std::int64_t timestamp_ns,
                               std::size_t frame_index) {
    impl_->write_video(std::move(frame), timestamp_ns, frame_index);
}
void FfmpegWriter::write_video(VulkanRawMosaicInput frame,
                               std::int64_t timestamp_ns,
                               std::size_t frame_index) {
    impl_->write_video(std::move(frame), timestamp_ns, frame_index);
}
void FfmpegWriter::write_audio(const AudioChunk& chunk) { impl_->write_audio(chunk); }
void FfmpegWriter::finish() { impl_->finish(); }
FfmpegWriterTelemetry FfmpegWriter::telemetry() const { return impl_->telemetry(); }

void validate_prores_mov_metadata(const std::filesystem::path& path,
                                  const std::string& prores_profile) {
    AVFormatContext* input = nullptr;
    const auto path_utf8 = path.u8string();
    const std::string path_string(path_utf8.begin(), path_utf8.end());
    require_ffmpeg(avformat_open_input(&input, path_string.c_str(), nullptr, nullptr),
                   "reopen completed MOV");
    struct InputCloser {
        AVFormatContext*& value;
        ~InputCloser() { avformat_close_input(&value); }
    } closer{input};
    require_ffmpeg(avformat_find_stream_info(input, nullptr), "read completed MOV stream info");
    const int stream_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1,
                                                 nullptr, 0);
    require_ffmpeg(stream_index, "find completed MOV video stream");
    const auto* parameters = input->streams[stream_index]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_PRORES ||
        parameters->codec_tag != prores_codec_tag(prores_profile) ||
        parameters->color_range != AVCOL_RANGE_MPEG ||
        parameters->color_space != AVCOL_SPC_BT2020_NCL ||
        parameters->color_primaries != AVCOL_PRI_UNSPECIFIED ||
        parameters->color_trc != AVCOL_TRC_UNSPECIFIED) {
        throw Error(ErrorCode::encode_failed,
                    "completed MOV does not match the requested ProRes/color metadata contract");
    }
}

void validate_prores_mov(const std::filesystem::path& path,
                         std::uint64_t expected_video_packets,
                         const std::string& prores_profile) {
    AVFormatContext* input = nullptr;
    const auto path_utf8 = path.u8string();
    const std::string path_string(path_utf8.begin(), path_utf8.end());
    require_ffmpeg(avformat_open_input(&input, path_string.c_str(), nullptr, nullptr),
                   "reopen completed MOV");
    struct InputCloser {
        AVFormatContext*& value;
        ~InputCloser() { avformat_close_input(&value); }
    } closer{input};
    require_ffmpeg(avformat_find_stream_info(input, nullptr), "read completed MOV stream info");
    const int stream_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1,
                                                 nullptr, 0);
    require_ffmpeg(stream_index, "find completed MOV video stream");
    const auto* parameters = input->streams[stream_index]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_PRORES ||
        parameters->codec_tag != prores_codec_tag(prores_profile) ||
        parameters->color_range != AVCOL_RANGE_MPEG ||
        parameters->color_space != AVCOL_SPC_BT2020_NCL ||
        parameters->color_primaries != AVCOL_PRI_UNSPECIFIED ||
        parameters->color_trc != AVCOL_TRC_UNSPECIFIED) {
        throw Error(ErrorCode::encode_failed,
                    "completed MOV does not match the requested ProRes/color metadata contract");
    }
    auto packet = make_av_packet();
    std::uint64_t video_packets = 0;
    std::int64_t previous_pts = AV_NOPTS_VALUE;
    while (av_read_frame(input, packet.get()) >= 0) {
        if (packet->stream_index == stream_index) {
            if (packet->pts == AV_NOPTS_VALUE ||
                (previous_pts != AV_NOPTS_VALUE && packet->pts <= previous_pts) ||
                packet->duration <= 0) {
                throw Error(ErrorCode::encode_failed,
                            "completed MOV has invalid video PTS or duration");
            }
            previous_pts = packet->pts;
            ++video_packets;
        }
        av_packet_unref(packet.get());
    }
    if (video_packets != expected_video_packets) {
        throw Error(ErrorCode::encode_failed,
                    "completed MOV video packet count does not match submitted frames");
    }
}

} // namespace mcraw
