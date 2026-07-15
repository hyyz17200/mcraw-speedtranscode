#pragma once

#include <cstddef>
#include <string_view>

#include <mcraw/core/config.hpp>
#include <mcraw/core/metadata.hpp>
#include <mcraw/core/timing.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/processing/log_curve.hpp>
#include <mcraw/processing/yuv.hpp>

namespace mcraw {

enum class CpuPipelineOutput {
    packed_yuv,
    target_log_rgb,
    camera_rgb,
};

[[nodiscard]] std::string_view to_string(CpuPipelineOutput value) noexcept;

struct ProcessedFrame {
    std::int64_t timestamp_ns{};
    NormalizedCameraMetadata metadata;
    CameraColorSolution color_solution;
    PackedYuvResult packed;
    TargetLogRgbF32 target_log;
    CameraRgbF32 camera_rgb;
};

class CpuPipeline {
public:
    explicit CpuPipeline(EffectiveConfig config,
                         std::size_t worker_threads = 1,
                         CpuPipelineOutput output = CpuPipelineOutput::packed_yuv);
    [[nodiscard]] ProcessedFrame process(
        const McrawReader& reader,
        std::size_t frame_index,
        StageTimings& timings) const;

private:
    EffectiveConfig config_;
    std::size_t worker_threads_{1};
    CpuPipelineOutput output_{CpuPipelineOutput::packed_yuv};
    DaVinciIntermediateLut di_curve_;
};

} // namespace mcraw
