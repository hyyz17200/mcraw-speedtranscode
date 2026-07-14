#pragma once

#include <memory>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

namespace mcraw {

struct AvFrameDeleter {
    void operator()(AVFrame* value) const noexcept;
};

struct AvPacketDeleter {
    void operator()(AVPacket* value) const noexcept;
};

struct AvBufferRefDeleter {
    void operator()(AVBufferRef* value) const noexcept;
};

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* value) const noexcept;
};

using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using AvBufferRefPtr = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;
using AvCodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;

[[nodiscard]] AvFramePtr make_av_frame();
[[nodiscard]] AvPacketPtr make_av_packet();
[[nodiscard]] std::string ffmpeg_error_string(int code);
void require_ffmpeg(int code, std::string_view operation);

} // namespace mcraw
