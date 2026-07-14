#pragma once

#include <string>

#include <mcraw/output/video_encoder.hpp>

namespace mcraw {

class VulkanProResEncoderStub final : public IVideoEncoder {
public:
    explicit VulkanProResEncoderStub(std::string unavailable_reason);

    [[nodiscard]] VideoEncoderCapabilities capabilities() const override;
    void send(VideoFrame frame) override;
    [[nodiscard]] std::vector<EncodedPacket> drain() override;
    [[nodiscard]] std::vector<EncodedPacket> flush() override;

private:
    [[noreturn]] void fail() const;
    std::string unavailable_reason_;
};

} // namespace mcraw
