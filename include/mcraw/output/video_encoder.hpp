#pragma once

#include <string>
#include <vector>

#include <mcraw/output/video_frame.hpp>

namespace mcraw {

struct EncodedPacket {
    AvPacketPtr packet;
    AVRational time_base{0, 1};
};

struct VideoEncoderCapabilities {
    std::string name;
    FrameStorage input_storage{FrameStorage::cpu};
    bool available{};
    bool delayed_output{};
    std::string unavailable_reason;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    [[nodiscard]] virtual VideoEncoderCapabilities capabilities() const = 0;

    // send() transfers ownership of one frame. It may produce no packet.
    // drain() returns every packet currently available without signalling EOS.
    // flush() signals EOS and returns all delayed packets through AVERROR_EOF.
    virtual void send(VideoFrame frame) = 0;
    [[nodiscard]] virtual std::vector<EncodedPacket> drain() = 0;
    [[nodiscard]] virtual std::vector<EncodedPacket> flush() = 0;
};

} // namespace mcraw
