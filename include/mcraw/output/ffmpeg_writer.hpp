#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <mcraw/core/pixel_types.hpp>
#include <mcraw/io/mcraw_reader.hpp>

namespace mcraw {

class FfmpegWriter {
public:
    FfmpegWriter(const std::filesystem::path& output,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::int64_t timeline_origin_ns,
                 int audio_sample_rate,
                 int audio_channels);
    ~FfmpegWriter();
    FfmpegWriter(FfmpegWriter&&) noexcept;
    FfmpegWriter& operator=(FfmpegWriter&&) noexcept;
    FfmpegWriter(const FfmpegWriter&) = delete;
    FfmpegWriter& operator=(const FfmpegWriter&) = delete;

    void write_video(const Yuv422P10& frame, std::int64_t timestamp_ns);
    void write_audio(const AudioChunk& chunk);
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw

