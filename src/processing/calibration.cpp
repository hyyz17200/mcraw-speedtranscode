#include <mcraw/processing/calibration.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

template <typename Output>
Output calibrate_raw_impl(const RawMosaicU16& input,
                          const NormalizedCameraMetadata& metadata,
                          std::size_t worker_threads,
                          float output_scale) {
    input.validate();
    metadata.validate_for_raw();
    if (input.width != metadata.width || input.height != metadata.height || input.cfa != metadata.cfa) {
        throw Error(ErrorCode::invalid_argument, "RAW image and normalized metadata do not agree");
    }

    Output output{input.width, input.height, input.cfa, {}};
    output.pixels.resize(input.pixels.size());
    const int thread_count = static_cast<int>(std::max<std::size_t>(1U, worker_threads));
#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t row = 0; row < static_cast<std::int64_t>(input.height); ++row) {
        const auto y = static_cast<std::uint32_t>(row);
        for (std::uint32_t x = 0; x < input.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * input.width + x;
            const auto cfa_position = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const double black = metadata.black_level[cfa_position];
            const double scale = metadata.white_level[cfa_position] - black;
            const float normalized = static_cast<float>(
                (static_cast<double>(input.pixels[pixel]) - black) / scale);
            output.pixels[pixel] = normalized * output_scale;
        }
    }
    return output;
}

} // namespace

RawNormalizedF32 calibrate_raw(const RawMosaicU16& input,
                               const NormalizedCameraMetadata& metadata,
                               std::size_t worker_threads) {
    return calibrate_raw_impl<RawNormalizedF32>(input, metadata, worker_threads, 1.0F);
}

RawDemosaicF32 calibrate_raw_for_demosaic(const RawMosaicU16& input,
                                           const NormalizedCameraMetadata& metadata,
                                           std::size_t worker_threads) {
    return calibrate_raw_impl<RawDemosaicF32>(input, metadata, worker_threads, 65535.0F);
}

} // namespace mcraw
