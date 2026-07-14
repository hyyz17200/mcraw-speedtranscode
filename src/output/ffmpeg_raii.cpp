#include <mcraw/output/ffmpeg_raii.hpp>

#include <array>

extern "C" {
#include <libavutil/error.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {

void AvFrameDeleter::operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
void AvPacketDeleter::operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
void AvBufferRefDeleter::operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
void AvCodecContextDeleter::operator()(AVCodecContext* value) const noexcept {
    avcodec_free_context(&value);
}

AvFramePtr make_av_frame() {
    AvFramePtr result(av_frame_alloc());
    if (!result) throw Error(ErrorCode::encode_failed, "cannot allocate FFmpeg frame");
    return result;
}

AvPacketPtr make_av_packet() {
    AvPacketPtr result(av_packet_alloc());
    if (!result) throw Error(ErrorCode::encode_failed, "cannot allocate FFmpeg packet");
    return result;
}

std::string ffmpeg_error_string(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error " + std::to_string(code);
    }
    return buffer.data();
}

void require_ffmpeg(int code, std::string_view operation) {
    if (code < 0) {
        throw Error(ErrorCode::encode_failed,
                    std::string(operation) + ": " + ffmpeg_error_string(code));
    }
}

} // namespace mcraw
