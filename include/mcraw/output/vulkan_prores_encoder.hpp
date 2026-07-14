#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/output/video_encoder.hpp>

namespace mcraw {

struct VulkanProResEncoderConfig {
    int width{};
    int height{};
    AVRational time_base{1, 90'000};
    AVRational frame_rate{30, 1};
    std::string profile{"hq"};
    std::size_t async_depth{1};
};

struct VulkanProResTelemetry {
    bool gpu_resident{};
    std::uint64_t upload_frames{};
    std::uint64_t readback_frames{};
    std::uint64_t packets{};
};

class VulkanProResEncoder final : public IVideoEncoder {
public:
    VulkanProResEncoder(FfmpegVulkanFrameContext& frames,
                        VulkanProResEncoderConfig config);
    ~VulkanProResEncoder() override;
    VulkanProResEncoder(VulkanProResEncoder&&) noexcept;
    VulkanProResEncoder& operator=(VulkanProResEncoder&&) noexcept;
    VulkanProResEncoder(const VulkanProResEncoder&) = delete;
    VulkanProResEncoder& operator=(const VulkanProResEncoder&) = delete;

    [[nodiscard]] VideoEncoderCapabilities capabilities() const override;
    void send(VideoFrame frame) override;
    [[nodiscard]] std::vector<EncodedPacket> drain() override;
    [[nodiscard]] std::vector<EncodedPacket> flush() override;
    [[nodiscard]] VulkanProResTelemetry telemetry() const noexcept;
    void copy_parameters_to(AVCodecParameters* parameters) const;
    [[nodiscard]] AVRational time_base() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
