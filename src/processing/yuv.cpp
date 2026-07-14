#include <mcraw/processing/yuv.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

constexpr double kr = 0.2627;
constexpr double kb = 0.0593;
constexpr double kg = 1.0 - kr - kb;

double deterministic_noise(std::size_t frame, std::size_t plane, std::size_t sample) {
    std::uint64_t state = static_cast<std::uint64_t>(sample) * 0x9E3779B185EBCA87ULL;
    state ^= static_cast<std::uint64_t>(frame) * 0xC2B2AE3D27D4EB4FULL;
    state ^= static_cast<std::uint64_t>(plane) * 0x165667B19E3779F9ULL;
    state ^= state >> 30U;
    state *= 0xBF58476D1CE4E5B9ULL;
    state ^= state >> 27U;
    state *= 0x94D049BB133111EBULL;
    state ^= state >> 31U;
    return static_cast<double>(state >> 11U) * (1.0 / 9007199254740992.0) - 0.5;
}

std::uint16_t quantize(double value,
                       double minimum,
                       double maximum,
                       double noise,
                       std::size_t& low,
                       std::size_t& high) {
    if (value < minimum) ++low;
    if (value > maximum) ++high;
    const double clipped = std::clamp(value, minimum, maximum);
    // All legal-range code values are positive and noise is in [-0.5, 0.5).
    // Adding 0.5 before the integer conversion is therefore exactly equivalent
    // to llround, without an external CRT call in the per-sample hot path.
    return static_cast<std::uint16_t>(clipped + noise + 0.5);
}

std::size_t clamped_x(int x, std::uint32_t width) {
    return static_cast<std::size_t>(std::clamp(x, 0, static_cast<int>(width) - 1));
}

double filtered_chroma(const std::vector<double>& row,
                       std::uint32_t x,
                       std::uint32_t width,
                       ChromaFilter filter) {
    if (filter == ChromaFilter::fast) return row[x];
    // Five-tap symmetric low-pass sampled at even/left chroma positions.
    static constexpr std::array<double, 5> taps{-1.0 / 16.0, 4.0 / 16.0, 10.0 / 16.0,
                                                 4.0 / 16.0, -1.0 / 16.0};
    double result = 0.0;
    for (int offset = -2; offset <= 2; ++offset) {
        result += taps[static_cast<std::size_t>(offset + 2)] * row[clamped_x(static_cast<int>(x) + offset, width)];
    }
    return result;
}

template <ChromaFilter Filter>
double filtered_chroma_fixed(const std::vector<double>& row,
                             std::uint32_t x,
                             std::uint32_t width) {
    if constexpr (Filter == ChromaFilter::fast) return row[x];
    static constexpr std::array<double, 5> taps{-1.0 / 16.0, 4.0 / 16.0, 10.0 / 16.0,
                                                 4.0 / 16.0, -1.0 / 16.0};
    double result = 0.0;
    if (x >= 2U && x + 2U < width) {
        const auto base = static_cast<std::size_t>(x - 2U);
        for (std::size_t i = 0; i < taps.size(); ++i) result += taps[i] * row[base + i];
        return result;
    }
    for (int offset = -2; offset <= 2; ++offset) {
        result += taps[static_cast<std::size_t>(offset + 2)] *
                  row[clamped_x(static_cast<int>(x) + offset, width)];
    }
    return result;
}

int current_thread_index() noexcept {
#ifdef _OPENMP
    return omp_get_thread_num();
#else
    return 0;
#endif
}

template <NegativePolicy Policy, ChromaFilter Filter, bool Dither, bool Sharpen>
PackedYuvResult pack_fused(const CameraRgbF32& input,
                           const CameraColorSolution& solution,
                           double exposure_offset_stops,
                           const DaVinciIntermediateLut& curve,
                           std::size_t frame_index,
                           std::size_t worker_threads,
                           double capture_sharpening,
                           double capture_sharpening_threshold) {
    input.validate();
    if ((input.width & 1U) != 0U) {
        throw Error(ErrorCode::invalid_argument, "ProRes 422 requires an even frame width");
    }
    PackedYuvResult result;
    result.image.width = input.width;
    result.image.height = input.height;
    const auto pixels = static_cast<std::size_t>(input.width) * input.height;
    result.image.y.resize(pixels);
    result.image.cb.resize(pixels / 2U);
    result.image.cr.resize(pixels / 2U);

    const auto bounded_threads = std::clamp<std::size_t>(worker_threads, 1U, 256U);
    const int thread_count = static_cast<int>(bounded_threads);
    std::vector<std::vector<double>> cb_scratch(
        bounded_threads, std::vector<double>(input.width));
    std::vector<std::vector<double>> cr_scratch(
        bounded_threads, std::vector<double>(input.width));
    std::vector<PackingStats> thread_stats(bounded_threads);
    std::atomic_bool non_finite{false};
    std::atomic_bool rejected_negative{false};
    const double exposure = std::exp2(exposure_offset_stops);
    const bool unit_exposure = exposure_offset_stops == 0.0;
    const auto& matrix = solution.camera_to_target.v;
    const std::array<double, 3> camera_to_luma{
        kr * matrix[0] + kg * matrix[3] + kb * matrix[6],
        kr * matrix[1] + kg * matrix[4] + kb * matrix[7],
        kr * matrix[2] + kg * matrix[5] + kb * matrix[8]
    };

    const auto pack_row = [&](std::uint32_t y,
                              std::size_t thread,
                              [[maybe_unused]] const double* previous_luma,
                              [[maybe_unused]] const double* current_luma,
                              [[maybe_unused]] const double* next_luma) {
        auto& cb_row = cb_scratch[thread];
        auto& cr_row = cr_scratch[thread];
        auto& stats = thread_stats[thread];
        for (std::uint32_t x = 0; x < input.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * input.width + x;
            const double camera_r = input.planes[0][pixel];
            const double camera_g = input.planes[1][pixel];
            const double camera_b = input.planes[2][pixel];
            std::array<double, 3> linear{
                matrix[0] * camera_r + matrix[1] * camera_g + matrix[2] * camera_b,
                matrix[3] * camera_r + matrix[4] * camera_g + matrix[5] * camera_b,
                matrix[6] * camera_r + matrix[7] * camera_g + matrix[8] * camera_b
            };
            if (!unit_exposure) {
                for (double& channel : linear) channel *= exposure;
            }
            if constexpr (Sharpen) {
                const auto left = x == 0U ? 0U : x - 1U;
                const auto right = std::min(x + 1U, input.width - 1U);
                const double center_luma = kr * linear[0] + kg * linear[1] + kb * linear[2];
                const double neighbor_luma = 0.25 * (
                    current_luma[left] + current_luma[right] +
                    previous_luma[x] + next_luma[x]);
                const double detail = center_luma - neighbor_luma;
                const double magnitude = std::abs(detail);
                if (magnitude > capture_sharpening_threshold) {
                    const double delta = capture_sharpening * std::copysign(
                        magnitude - capture_sharpening_threshold, detail);
                    for (double& channel : linear) channel += delta;
                }
            }
            std::array<float, 3> encoded{};
            for (std::size_t channel = 0; channel < encoded.size(); ++channel) {
                if (!std::isfinite(linear[channel])) {
                    non_finite.store(true, std::memory_order_relaxed);
                    linear[channel] = 0.0;
                }
                if constexpr (Policy == NegativePolicy::clamp_zero) {
                    linear[channel] = std::max(0.0, linear[channel]);
                } else if constexpr (Policy == NegativePolicy::error) {
                    if (linear[channel] < 0.0) {
                        rejected_negative.store(true, std::memory_order_relaxed);
                        linear[channel] = 0.0;
                    }
                }
                encoded[channel] = curve.encode(static_cast<float>(linear[channel]));
            }
            const double r = encoded[0];
            const double g = encoded[1];
            const double b = encoded[2];
            const double luma = kr * r + kg * g + kb * b;
            cb_row[x] = (b - luma) / (2.0 * (1.0 - kb));
            cr_row[x] = (r - luma) / (2.0 * (1.0 - kr));
            const double noise = Dither ? deterministic_noise(frame_index, 0, pixel) : 0.0;
            result.image.y[pixel] = quantize(64.0 + 876.0 * luma, 64.0, 940.0, noise,
                                             stats.luma_clipped_low, stats.luma_clipped_high);
        }
        for (std::uint32_t x = 0; x < input.width; x += 2U) {
            const auto chroma = static_cast<std::size_t>(y) * (input.width / 2U) + x / 2U;
            std::size_t cb_low = 0;
            std::size_t cb_high = 0;
            std::size_t cr_low = 0;
            std::size_t cr_high = 0;
            const double cb_noise = Dither ? deterministic_noise(frame_index, 1, chroma) : 0.0;
            const double cr_noise = Dither ? deterministic_noise(frame_index, 2, chroma) : 0.0;
            result.image.cb[chroma] = quantize(
                512.0 + 896.0 * filtered_chroma_fixed<Filter>(cb_row, x, input.width),
                64.0, 960.0, cb_noise, cb_low, cb_high);
            result.image.cr[chroma] = quantize(
                512.0 + 896.0 * filtered_chroma_fixed<Filter>(cr_row, x, input.width),
                64.0, 960.0, cr_noise, cr_low, cr_high);
            stats.chroma_clipped += cb_low + cb_high + cr_low + cr_high;
        }
    };

    if constexpr (Sharpen) {
        const auto fill_luma_row = [&](std::vector<double>& row_values, std::uint32_t y) {
            const auto row_offset = static_cast<std::size_t>(y) * input.width;
            for (std::uint32_t x = 0; x < input.width; ++x) {
                const auto sample = row_offset + x;
                const double value =
                    camera_to_luma[0] * input.planes[0][sample] +
                    camera_to_luma[1] * input.planes[1][sample] +
                    camera_to_luma[2] * input.planes[2][sample];
                row_values[x] = unit_exposure ? value : exposure * value;
            }
        };
#pragma omp parallel num_threads(thread_count)
        {
            const auto thread = static_cast<std::size_t>(current_thread_index());
#ifdef _OPENMP
            const auto active_threads = static_cast<std::size_t>(omp_get_num_threads());
#else
            constexpr std::size_t active_threads = 1U;
#endif
            const auto begin = static_cast<std::uint32_t>(
                static_cast<std::size_t>(input.height) * thread / active_threads);
            const auto end = static_cast<std::uint32_t>(
                static_cast<std::size_t>(input.height) * (thread + 1U) / active_threads);
            if (begin < end) {
                std::vector<double> previous(input.width);
                std::vector<double> current(input.width);
                std::vector<double> next(input.width);
                fill_luma_row(current, begin);
                if (begin == 0U) previous = current;
                else fill_luma_row(previous, begin - 1U);
                if (begin + 1U < input.height) fill_luma_row(next, begin + 1U);
                else next = current;
                for (auto y = begin; y < end; ++y) {
                    pack_row(y, thread, previous.data(), current.data(), next.data());
                    if (y + 1U < end) {
                        previous.swap(current);
                        current.swap(next);
                        if (y + 2U < input.height) fill_luma_row(next, y + 2U);
                        else next = current;
                    }
                }
            }
        }
    } else {
#pragma omp parallel for schedule(static) num_threads(thread_count)
        for (std::int64_t row = 0; row < static_cast<std::int64_t>(input.height); ++row) {
            pack_row(static_cast<std::uint32_t>(row),
                     static_cast<std::size_t>(current_thread_index()), nullptr, nullptr, nullptr);
        }
    }
    if (non_finite.load(std::memory_order_relaxed)) {
        throw Error(ErrorCode::processing_failed, "camera-to-DWG transform produced a non-finite value");
    }
    if (rejected_negative.load(std::memory_order_relaxed)) {
        throw Error(ErrorCode::processing_failed, "negative target-linear value rejected by policy");
    }
    for (const auto& stats : thread_stats) {
        result.stats.luma_clipped_low += stats.luma_clipped_low;
        result.stats.luma_clipped_high += stats.luma_clipped_high;
        result.stats.chroma_clipped += stats.chroma_clipped;
    }
    result.image.validate();
    return result;
}

template <NegativePolicy Policy, ChromaFilter Filter, bool Dither>
PackedYuvResult dispatch_sharpening(const CameraRgbF32& input,
                                    const CameraColorSolution& solution,
                                    double exposure_offset_stops,
                                    const DaVinciIntermediateLut& curve,
                                    std::size_t frame_index,
                                    std::size_t worker_threads,
                                    double capture_sharpening,
                                    double capture_sharpening_threshold) {
    if (capture_sharpening > 0.0) {
        return pack_fused<Policy, Filter, Dither, true>(
            input, solution, exposure_offset_stops, curve, frame_index, worker_threads,
            capture_sharpening, capture_sharpening_threshold);
    }
    return pack_fused<Policy, Filter, Dither, false>(
        input, solution, exposure_offset_stops, curve, frame_index, worker_threads,
        0.0, capture_sharpening_threshold);
}

template <NegativePolicy Policy, ChromaFilter Filter>
PackedYuvResult dispatch_dither(const CameraRgbF32& input,
                                const CameraColorSolution& solution,
                                double exposure_offset_stops,
                                const DaVinciIntermediateLut& curve,
                                bool dither,
                                std::size_t frame_index,
                                std::size_t worker_threads,
                                double capture_sharpening,
                                double capture_sharpening_threshold) {
    if (dither) {
        return dispatch_sharpening<Policy, Filter, true>(
            input, solution, exposure_offset_stops, curve, frame_index, worker_threads,
            capture_sharpening, capture_sharpening_threshold);
    }
    return dispatch_sharpening<Policy, Filter, false>(
        input, solution, exposure_offset_stops, curve, frame_index, worker_threads,
        capture_sharpening, capture_sharpening_threshold);
}

template <NegativePolicy Policy>
PackedYuvResult dispatch_filter(const CameraRgbF32& input,
                                const CameraColorSolution& solution,
                                double exposure_offset_stops,
                                const DaVinciIntermediateLut& curve,
                                ChromaFilter filter,
                                bool dither,
                                std::size_t frame_index,
                                std::size_t worker_threads,
                                double capture_sharpening,
                                double capture_sharpening_threshold) {
    if (filter == ChromaFilter::fast) {
        return dispatch_dither<Policy, ChromaFilter::fast>(
            input, solution, exposure_offset_stops, curve, dither, frame_index, worker_threads,
            capture_sharpening, capture_sharpening_threshold);
    }
    return dispatch_dither<Policy, ChromaFilter::quality>(
        input, solution, exposure_offset_stops, curve, dither, frame_index, worker_threads,
        capture_sharpening, capture_sharpening_threshold);
}

} // namespace

PackedYuvResult pack_dwg_log_to_yuv422p10(const TargetLogRgbF32& input,
                                          ChromaFilter filter,
                                          bool dither,
                                          std::size_t frame_index) {
    input.validate();
    if ((input.width & 1U) != 0U) {
        throw Error(ErrorCode::invalid_argument, "ProRes 422 requires an even frame width");
    }
    PackedYuvResult result;
    result.image.width = input.width;
    result.image.height = input.height;
    const auto pixels = static_cast<std::size_t>(input.width) * input.height;
    result.image.y.resize(pixels);
    result.image.cb.resize(pixels / 2U);
    result.image.cr.resize(pixels / 2U);

    std::vector<double> cb_row(input.width);
    std::vector<double> cr_row(input.width);
    for (std::uint32_t y = 0; y < input.height; ++y) {
        for (std::uint32_t x = 0; x < input.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * input.width + x;
            const double r = input.planes[0][pixel];
            const double g = input.planes[1][pixel];
            const double b = input.planes[2][pixel];
            const double luma = kr * r + kg * g + kb * b;
            cb_row[x] = (b - luma) / (2.0 * (1.0 - kb));
            cr_row[x] = (r - luma) / (2.0 * (1.0 - kr));
            result.image.y[pixel] = quantize(64.0 + 876.0 * luma, 64.0, 940.0,
                                             dither ? deterministic_noise(frame_index, 0, pixel) : 0.0,
                                             result.stats.luma_clipped_low, result.stats.luma_clipped_high);
        }
        for (std::uint32_t x = 0; x < input.width; x += 2U) {
            const auto chroma = static_cast<std::size_t>(y) * (input.width / 2U) + x / 2U;
            std::size_t cb_low = 0;
            std::size_t cb_high = 0;
            std::size_t cr_low = 0;
            std::size_t cr_high = 0;
            result.image.cb[chroma] = quantize(512.0 + 896.0 * filtered_chroma(cb_row, x, input.width, filter),
                                               64.0, 960.0,
                                               dither ? deterministic_noise(frame_index, 1, chroma) : 0.0,
                                               cb_low, cb_high);
            result.image.cr[chroma] = quantize(512.0 + 896.0 * filtered_chroma(cr_row, x, input.width, filter),
                                               64.0, 960.0,
                                               dither ? deterministic_noise(frame_index, 2, chroma) : 0.0,
                                               cr_low, cr_high);
            result.stats.chroma_clipped += cb_low + cb_high + cr_low + cr_high;
        }
    }
    result.image.validate();
    return result;
}

PackedYuvResult pack_camera_to_dwg_di_yuv422p10(
    const CameraRgbF32& input,
    const CameraColorSolution& solution,
    double exposure_offset_stops,
    NegativePolicy negative_policy,
    const DaVinciIntermediateLut& curve,
    ChromaFilter filter,
    bool dither,
    std::size_t frame_index,
    std::size_t worker_threads,
    double capture_sharpening,
    double capture_sharpening_threshold) {
    switch (negative_policy) {
    case NegativePolicy::preserve_by_curve:
        return dispatch_filter<NegativePolicy::preserve_by_curve>(
            input, solution, exposure_offset_stops, curve, filter, dither,
            frame_index, worker_threads, capture_sharpening, capture_sharpening_threshold);
    case NegativePolicy::clamp_zero:
        return dispatch_filter<NegativePolicy::clamp_zero>(
            input, solution, exposure_offset_stops, curve, filter, dither,
            frame_index, worker_threads, capture_sharpening, capture_sharpening_threshold);
    case NegativePolicy::error:
        return dispatch_filter<NegativePolicy::error>(
            input, solution, exposure_offset_stops, curve, filter, dither,
            frame_index, worker_threads, capture_sharpening, capture_sharpening_threshold);
    }
    throw Error(ErrorCode::invalid_argument, "unknown negative policy");
}

} // namespace mcraw
