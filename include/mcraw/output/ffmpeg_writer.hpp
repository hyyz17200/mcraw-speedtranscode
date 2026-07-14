#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <mcraw/core/pixel_types.hpp>
#include <mcraw/io/mcraw_reader.hpp>

namespace mcraw {

// ProRes is intra-only, so independent frames can encode concurrently on
// separate identically configured encoder contexts and mux in source order.
// contexts scales the number of frames in flight; threads_per_context is the
// FFmpeg slice-thread count inside each encode (0 lets FFmpeg decide).
struct VideoEncodeConcurrency {
    std::size_t contexts{1};
    int threads_per_context{0};
};

class FfmpegWriter {
public:
    FfmpegWriter(const std::filesystem::path& output,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::int64_t timeline_origin_ns,
                 int audio_sample_rate,
                 int audio_channels,
                 VideoEncodeConcurrency video_concurrency = {});
    ~FfmpegWriter();
    FfmpegWriter(FfmpegWriter&&) noexcept;
    FfmpegWriter& operator=(FfmpegWriter&&) noexcept;
    FfmpegWriter(const FfmpegWriter&) = delete;
    FfmpegWriter& operator=(const FfmpegWriter&) = delete;

    void write_video(Yuv422P10 frame, std::int64_t timestamp_ns);
    void write_audio(const AudioChunk& chunk);
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
