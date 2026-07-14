#pragma once

#include <memory>
#include <string>

#include <mcraw/output/video_encoder.hpp>

namespace mcraw {

struct CpuProResEncoderConfig {
    int width{};
    int height{};
    AVRational time_base{1, 90'000};
    AVRational frame_rate{30, 1};
    std::string profile{"hq"};
    int threads{};
};

class CpuProResEncoder final : public IVideoEncoder {
public:
    explicit CpuProResEncoder(CpuProResEncoderConfig config);
    ~CpuProResEncoder() override;
    CpuProResEncoder(CpuProResEncoder&&) noexcept;
    CpuProResEncoder& operator=(CpuProResEncoder&&) noexcept;
    CpuProResEncoder(const CpuProResEncoder&) = delete;
    CpuProResEncoder& operator=(const CpuProResEncoder&) = delete;

    [[nodiscard]] VideoEncoderCapabilities capabilities() const override;
    void send(VideoFrame frame) override;
    [[nodiscard]] std::vector<EncodedPacket> drain() override;
    [[nodiscard]] std::vector<EncodedPacket> flush() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
