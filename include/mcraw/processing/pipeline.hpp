#pragma once

#include <cstddef>

#include <mcraw/core/config.hpp>
#include <mcraw/core/metadata.hpp>
#include <mcraw/core/timing.hpp>
#include <mcraw/io/mcraw_reader.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/processing/log_curve.hpp>
#include <mcraw/processing/yuv.hpp>

namespace mcraw {

struct ProcessedFrame {
    std::int64_t timestamp_ns{};
    NormalizedCameraMetadata metadata;
    CameraColorSolution color_solution;
    PackedYuvResult packed;
    TargetLogRgbF32 target_log;
};

class CpuPipeline {
public:
    explicit CpuPipeline(EffectiveConfig config,
                         std::size_t worker_threads = 1,
                         bool output_target_log = false);
    [[nodiscard]] ProcessedFrame process(
        const McrawReader& reader,
        std::size_t frame_index,
        StageTimings& timings) const;

private:
    EffectiveConfig config_;
    std::size_t worker_threads_{1};
    bool output_target_log_{};
    DaVinciIntermediateLut di_curve_;
};

} // namespace mcraw
