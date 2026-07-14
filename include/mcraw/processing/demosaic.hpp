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

} // namespace mcraw
