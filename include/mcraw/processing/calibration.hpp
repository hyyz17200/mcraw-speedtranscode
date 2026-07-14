#pragma once

#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

[[nodiscard]] RawNormalizedF32 calibrate_raw(
    const RawMosaicU16& input,
    const NormalizedCameraMetadata& metadata,
    std::size_t worker_threads = 1);

[[nodiscard]] RawDemosaicF32 calibrate_raw_for_demosaic(
    const RawMosaicU16& input,
    const NormalizedCameraMetadata& metadata,
    std::size_t worker_threads = 1);

} // namespace mcraw
