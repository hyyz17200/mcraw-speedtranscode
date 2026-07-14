#pragma once

#include <cstdint>
#include <variant>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <mcraw/output/ffmpeg_raii.hpp>

namespace mcraw {

enum class FrameStorage { cpu, vulkan };

struct FrameMetadata {
    int width{};
    int height{};
    std::int64_t pts{AV_NOPTS_VALUE};
    std::int64_t duration{};
    AVRational time_base{0, 1};
    AVColorPrimaries primaries{AVCOL_PRI_UNSPECIFIED};
    AVColorTransferCharacteristic transfer{AVCOL_TRC_UNSPECIFIED};
    AVColorSpace matrix{AVCOL_SPC_UNSPECIFIED};
    AVColorRange range{AVCOL_RANGE_UNSPECIFIED};
    AVChromaLocation chroma_location{AVCHROMA_LOC_UNSPECIFIED};
};

struct CpuVideoFrame {
    FrameMetadata metadata;
    AVPixelFormat format{AV_PIX_FMT_NONE};
    AvFramePtr frame;
};

struct VulkanVideoFrame {
    FrameMetadata metadata;
    AVPixelFormat format{AV_PIX_FMT_VULKAN};
    AvFramePtr frame;
    AVPixelFormat software_format{AV_PIX_FMT_NONE};
};

using VideoFrame = std::variant<CpuVideoFrame, VulkanVideoFrame>;

} // namespace mcraw
