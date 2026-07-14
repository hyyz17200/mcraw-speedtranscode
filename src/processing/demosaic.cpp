#include <mcraw/processing/demosaic.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <mcraw/core/error.hpp>

#if MCRAW_HAS_RTPROCESS
#include <librtprocess.h>
#endif

namespace mcraw {
namespace {

template <typename T>
std::vector<T*> row_pointers(std::vector<T>& values, std::uint32_t width, std::uint32_t height) {
    std::vector<T*> rows(height);
    for (std::uint32_t y = 0; y < height; ++y) rows[y] = values.data() + static_cast<std::size_t>(y) * width;
    return rows;
}

std::vector<const float*> const_row_pointers(const std::vector<float>& values,
                                             std::uint32_t width,
                                             std::uint32_t height) {
    std::vector<const float*> rows(height);
    for (std::uint32_t y = 0; y < height; ++y) rows[y] = values.data() + static_cast<std::size_t>(y) * width;
    return rows;
}

} // namespace

CameraRgbF32 demosaic(const RawNormalizedF32& input,
                      DemosaicAlgorithm algorithm,
                      std::size_t worker_threads) {
    input.validate();
#if !MCRAW_HAS_RTPROCESS
    static_cast<void>(algorithm);
    throw Error(ErrorCode::unsupported_format, "this build has no librtprocess support");
#else
    if (input.width < 32U || input.height < 32U) {
        throw Error(ErrorCode::invalid_argument, "high-quality demosaic requires at least a 32x32 frame");
    }
    CameraRgbF32 output{input.width, input.height, {}};
    const auto count = static_cast<std::size_t>(input.width) * input.height;
    for (auto& plane : output.planes) plane.resize(count);

    // librtprocess documents a 0..65535 float working scale. Values outside the
    // nominal range are deliberately not clipped, so negative and super-white
    // samples remain available to algorithms that can propagate them.
    std::vector<float> scaled_input(input.pixels.size());
    const int thread_count = static_cast<int>(std::max<std::size_t>(1U, worker_threads));
#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(input.pixels.size()); ++i) {
        scaled_input[static_cast<std::size_t>(i)] = input.pixels[static_cast<std::size_t>(i)] * 65535.0F;
    }
    auto raw_rows = const_row_pointers(scaled_input, input.width, input.height);
    auto red_rows = row_pointers(output.planes[0], input.width, input.height);
    auto green_rows = row_pointers(output.planes[1], input.width, input.height);
    auto blue_rows = row_pointers(output.planes[2], input.width, input.height);
    unsigned cfa[2][2] = {
        {cfa_color(input.cfa, 0, 0), cfa_color(input.cfa, 1, 0)},
        {cfa_color(input.cfa, 0, 1), cfa_color(input.cfa, 1, 1)}
    };
    const std::function<bool(double)> progress = [](double) { return false; };
    rpError status = RP_NO_ERROR;
#ifdef _OPENMP
    omp_set_num_threads(thread_count);
#endif
    switch (algorithm) {
    case DemosaicAlgorithm::rcd:
        status = rcd_demosaic(static_cast<int>(input.width), static_cast<int>(input.height),
                              raw_rows.data(), red_rows.data(), green_rows.data(), blue_rows.data(),
                              cfa, progress, 2U, false, true);
        break;
    case DemosaicAlgorithm::amaze:
        status = amaze_demosaic(static_cast<int>(input.width), static_cast<int>(input.height),
                                0, 0, static_cast<int>(input.width), static_cast<int>(input.height),
                                raw_rows.data(), red_rows.data(), green_rows.data(), blue_rows.data(),
                                cfa, progress, 1.0, 16, 65535.0F, 65535.0F, 2U, false);
        break;
    case DemosaicAlgorithm::igv:
        status = igv_demosaic(static_cast<int>(input.width), static_cast<int>(input.height),
                              raw_rows.data(), red_rows.data(), green_rows.data(), blue_rows.data(),
                              cfa, progress);
        break;
    case DemosaicAlgorithm::dcb:
        status = dcb_demosaic(static_cast<int>(input.width), static_cast<int>(input.height),
                              raw_rows.data(), red_rows.data(), green_rows.data(), blue_rows.data(),
                              cfa, progress, 2, false);
        break;
    case DemosaicAlgorithm::lmmse:
        status = lmmse_demosaic(static_cast<int>(input.width), static_cast<int>(input.height),
                                raw_rows.data(), red_rows.data(), green_rows.data(), blue_rows.data(),
                                cfa, progress, 2);
        break;
    }
    if (status != RP_NO_ERROR) {
        throw Error(ErrorCode::processing_failed, "librtprocess demosaic failed with code " +
                                                std::to_string(static_cast<int>(status)));
    }
    for (auto& plane : output.planes) {
#pragma omp parallel for schedule(static) num_threads(thread_count)
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(plane.size()); ++i) {
            plane[static_cast<std::size_t>(i)] /= 65535.0F;
        }
    }
    output.validate();
    return output;
#endif
}

} // namespace mcraw
