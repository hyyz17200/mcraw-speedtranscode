#pragma once

#include <cstddef>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>
#include <mcraw/processing/color.hpp>
#include <mcraw/processing/log_curve.hpp>

namespace mcraw {

struct PackingStats {
    std::size_t luma_clipped_low{};
    std::size_t luma_clipped_high{};
    std::size_t chroma_clipped{};
};

struct PackedYuvResult {
    Yuv422P10 image;
    PackingStats stats;
};

[[nodiscard]] PackedYuvResult pack_dwg_log_to_yuv422p10(
    const TargetLogRgbF32& input,
    ChromaFilter filter,
    bool dither,
    std::size_t frame_index);

// Fused production path. The sensor matrix remains per-frame while the 1D
// transfer-curve table is cached by the owning conversion pipeline.
// input_scale maps the camera RGB planes to the normalized [0,1] domain
// (1/65535 for demosaic_unnormalized output); it folds into the per-frame
// matrix, so the recorded color solution stays unscaled.
[[nodiscard]] PackedYuvResult pack_camera_to_dwg_di_yuv422p10(
    const CameraRgbF32& input,
    const CameraColorSolution& solution,
    double exposure_offset_stops,
    NegativePolicy negative_policy,
    const DaVinciIntermediateLut& curve,
    ChromaFilter filter,
    bool dither,
    std::size_t frame_index,
    std::size_t worker_threads,
    double capture_sharpening = 0.0,
    double capture_sharpening_threshold = 0.002,
    double input_scale = 1.0);

} // namespace mcraw
