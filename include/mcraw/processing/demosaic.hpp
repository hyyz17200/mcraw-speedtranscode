#pragma once

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

[[nodiscard]] CameraRgbF32 demosaic(
    const RawNormalizedF32& input,
    DemosaicAlgorithm algorithm,
    std::size_t worker_threads = 1);

[[nodiscard]] CameraRgbF32 demosaic(
    const RawDemosaicF32& input,
    DemosaicAlgorithm algorithm,
    std::size_t worker_threads = 1);

// Production-pipeline variant: planes stay in the librtprocess 0..65535
// domain, skipping a full-frame normalization pass. Consumers must fold
// the 1/65535 scale into their per-frame color math.
[[nodiscard]] CameraRgbF32 demosaic_unnormalized(
    const RawDemosaicF32& input,
    DemosaicAlgorithm algorithm,
    std::size_t worker_threads = 1);

} // namespace mcraw
