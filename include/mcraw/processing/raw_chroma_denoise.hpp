#pragma once

#include <cstddef>

#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

// Noise-profile-driven spatial filtering of red/blue color differences in the
// calibrated Bayer mosaic. Green samples, which carry most luminance detail,
// are copied unchanged. A strength of zero is an exact no-op copy.
[[nodiscard]] RawNormalizedF32 denoise_raw_chroma(
    const RawNormalizedF32& input,
    const NormalizedCameraMetadata& metadata,
    double strength,
    std::size_t worker_threads = 1);

} // namespace mcraw
