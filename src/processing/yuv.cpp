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
float filtered_chroma_fixed(const std::vector<float>& row,
                            std::uint32_t x,
                            std::uint32_t width) {
    if constexpr (Filter == ChromaFilter::fast) return row[x];
    static constexpr std::array<float, 5> taps{-1.0F / 16.0F, 4.0F / 16.0F, 10.0F / 16.0F,
                                                4.0F / 16.0F, -1.0F / 16.0F};
    float result = 0.0F;
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

// Row-sized scratch planes for the fused path. Splitting the per-pixel work
// into separate row passes keeps every arithmetic loop free of branches and
// loop-carried state so the compiler can vectorize it; only the LUT encode
// and dither-noise passes stay scalar.
struct PackRowScratch {
    explicit PackRowScratch(std::size_t width)
        : linear_r(width), linear_g(width), linear_b(width),
          encoded_r(width), encoded_g(width), encoded_b(width),
          cb(width), cr(width), luma_noise(width) {}
    std::vector<float> linear_r;
    std::vector<float> linear_g;
    std::vector<float> linear_b;
    std::vector<float> encoded_r;
    std::vector<float> encoded_g;
    std::vector<float> encoded_b;
    std::vector<float> cb;
    std::vector<float> cr;
    std::vector<float> luma_noise;
};

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
    std::vector<PackingStats> thread_stats(bounded_threads);
    std::atomic_bool non_finite{false};
    std::atomic_bool rejected_negative{false};
    const double exposure = std::exp2(exposure_offset_stops);
    const auto& matrix = solution.camera_to_target.v;
    // Exposure is a per-frame scalar, so it folds into the matrix and the
    // sharpening luma weights instead of multiplying every pixel.
    std::array<float, 9> m{};
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i] = static_cast<float>(matrix[i] * exposure);
    }
    const std::array<float, 3> luma_weights{
        static_cast<float>((kr * matrix[0] + kg * matrix[3] + kb * matrix[6]) * exposure),
        static_cast<float>((kr * matrix[1] + kg * matrix[4] + kb * matrix[7]) * exposure),
        static_cast<float>((kr * matrix[2] + kg * matrix[5] + kb * matrix[8]) * exposure)
    };
    const auto krf = static_cast<float>(kr);
    const auto kgf = static_cast<float>(kg);
    const auto kbf = static_cast<float>(kb);
    const auto cb_scale = static_cast<float>(1.0 / (2.0 * (1.0 - kb)));
    const auto cr_scale = static_cast<float>(1.0 / (2.0 * (1.0 - kr)));
    const auto sharpen_amount = static_cast<float>(capture_sharpening);
    const auto sharpen_threshold = static_cast<float>(capture_sharpening_threshold);
    const auto width = input.width;

    const auto pack_row = [&](std::uint32_t y,
                              PackRowScratch& scratch,
                              PackingStats& stats,
                              bool& non_finite_local,
                              [[maybe_unused]] bool& rejected_local,
                              [[maybe_unused]] const float* previous_luma,
                              [[maybe_unused]] const float* current_luma,
                              [[maybe_unused]] const float* next_luma) {
        const auto row_offset = static_cast<std::size_t>(y) * width;
        const float* __restrict camera_r = input.planes[0].data() + row_offset;
        const float* __restrict camera_g = input.planes[1].data() + row_offset;
        const float* __restrict camera_b = input.planes[2].data() + row_offset;
        float* __restrict linear_r = scratch.linear_r.data();
        float* __restrict linear_g = scratch.linear_g.data();
        float* __restrict linear_b = scratch.linear_b.data();
        // Camera RGB -> exposed target linear (vectorizable).
        for (std::uint32_t x = 0; x < width; ++x) {
            const float r = camera_r[x];
            const float g = camera_g[x];
            const float b = camera_b[x];
            linear_r[x] = m[0] * r + m[1] * g + m[2] * b;
            linear_g[x] = m[3] * r + m[4] * g + m[5] * b;
            linear_b[x] = m[6] * r + m[7] * g + m[8] * b;
        }
        if constexpr (Sharpen) {
            // Neutral detail from the cached luma rows (vectorizable interior).
            const auto sharpen_at = [&](std::uint32_t x, std::uint32_t left, std::uint32_t right) {
                const float detail = current_luma[x] - 0.25F * (
                    current_luma[left] + current_luma[right] +
                    previous_luma[x] + next_luma[x]);
                const float over = std::abs(detail) - sharpen_threshold;
                const float delta = over > 0.0F
                    ? std::copysign(sharpen_amount * over, detail) : 0.0F;
                linear_r[x] += delta;
                linear_g[x] += delta;
                linear_b[x] += delta;
            };
            sharpen_at(0U, 0U, std::min(1U, width - 1U));
            for (std::uint32_t x = 1; x + 1U < width; ++x) sharpen_at(x, x - 1U, x + 1U);
            if (width > 1U) sharpen_at(width - 1U, width - 2U, width - 1U);
        }
        // Input guard, negative policy, and log encode (scalar LUT pass).
        const auto encode_plane = [&](const float* __restrict linear,
                                      float* __restrict encoded) {
            for (std::uint32_t x = 0; x < width; ++x) {
                float value = linear[x];
                if (!std::isfinite(value)) {
                    non_finite_local = true;
                    value = 0.0F;
                }
                if constexpr (Policy == NegativePolicy::clamp_zero) {
                    value = std::max(0.0F, value);
                } else if constexpr (Policy == NegativePolicy::error) {
                    if (value < 0.0F) {
                        rejected_local = true;
                        value = 0.0F;
                    }
                }
                encoded[x] = curve.encode(value);
            }
        };
        encode_plane(linear_r, scratch.encoded_r.data());
        encode_plane(linear_g, scratch.encoded_g.data());
        encode_plane(linear_b, scratch.encoded_b.data());
        if constexpr (Dither) {
            for (std::uint32_t x = 0; x < width; ++x) {
                scratch.luma_noise[x] = static_cast<float>(
                    deterministic_noise(frame_index, 0, row_offset + x));
            }
        }
        // Encoded RGB -> Y'CbCr rows and legal-range luma codes (vectorizable).
        const float* __restrict encoded_r = scratch.encoded_r.data();
        const float* __restrict encoded_g = scratch.encoded_g.data();
        const float* __restrict encoded_b = scratch.encoded_b.data();
        float* __restrict cb_row = scratch.cb.data();
        float* __restrict cr_row = scratch.cr.data();
        std::uint16_t* __restrict y_out = result.image.y.data() + row_offset;
        std::size_t luma_low = 0;
        std::size_t luma_high = 0;
        for (std::uint32_t x = 0; x < width; ++x) {
            const float r = encoded_r[x];
            const float g = encoded_g[x];
            const float b = encoded_b[x];
            const float luma = krf * r + kgf * g + kbf * b;
            cb_row[x] = (b - luma) * cb_scale;
            cr_row[x] = (r - luma) * cr_scale;
            const float value = 64.0F + 876.0F * luma;
            luma_low += value < 64.0F ? 1U : 0U;
            luma_high += value > 940.0F ? 1U : 0U;
            const float clipped = std::clamp(value, 64.0F, 940.0F);
            const float noise = Dither ? scratch.luma_noise[x] : 0.0F;
            // Legal-range codes are positive and noise is in [-0.5, 0.5), so
            // adding 0.5 before truncation is exactly llround.
            y_out[x] = static_cast<std::uint16_t>(clipped + noise + 0.5F);
        }
        stats.luma_clipped_low += luma_low;
        stats.luma_clipped_high += luma_high;
        const auto chroma_offset = static_cast<std::size_t>(y) * (width / 2U);
        std::uint16_t* cb_out = result.image.cb.data() + chroma_offset;
        std::uint16_t* cr_out = result.image.cr.data() + chroma_offset;
        std::size_t chroma_clipped = 0;
        for (std::uint32_t x = 0; x < width; x += 2U) {
            const float filtered_cb = filtered_chroma_fixed<Filter>(scratch.cb, x, width);
            const float filtered_cr = filtered_chroma_fixed<Filter>(scratch.cr, x, width);
            const float cb_noise = Dither ? static_cast<float>(
                deterministic_noise(frame_index, 1, chroma_offset + x / 2U)) : 0.0F;
            const float cr_noise = Dither ? static_cast<float>(
                deterministic_noise(frame_index, 2, chroma_offset + x / 2U)) : 0.0F;
            const float cb_value = 512.0F + 896.0F * filtered_cb;
            const float cr_value = 512.0F + 896.0F * filtered_cr;
            chroma_clipped += cb_value < 64.0F ? 1U : 0U;
            chroma_clipped += cb_value > 960.0F ? 1U : 0U;
            chroma_clipped += cr_value < 64.0F ? 1U : 0U;
            chroma_clipped += cr_value > 960.0F ? 1U : 0U;
            cb_out[x / 2U] = static_cast<std::uint16_t>(
                std::clamp(cb_value, 64.0F, 960.0F) + cb_noise + 0.5F);
            cr_out[x / 2U] = static_cast<std::uint16_t>(
                std::clamp(cr_value, 64.0F, 960.0F) + cr_noise + 0.5F);
        }
        stats.chroma_clipped += chroma_clipped;
    };

    const auto fill_luma_row = [&](std::vector<float>& row_values, std::uint32_t y) {
        const auto row_offset = static_cast<std::size_t>(y) * width;
        const float* __restrict camera_r = input.planes[0].data() + row_offset;
        const float* __restrict camera_g = input.planes[1].data() + row_offset;
        const float* __restrict camera_b = input.planes[2].data() + row_offset;
        float* __restrict values = row_values.data();
        for (std::uint32_t x = 0; x < width; ++x) {
            values[x] = luma_weights[0] * camera_r[x] +
                        luma_weights[1] * camera_g[x] +
                        luma_weights[2] * camera_b[x];
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
        if (begin < end && thread < bounded_threads) {
            PackRowScratch scratch(width);
            auto& stats = thread_stats[thread];
            bool non_finite_local = false;
            bool rejected_local = false;
            if constexpr (Sharpen) {
                std::vector<float> previous(width);
                std::vector<float> current(width);
                std::vector<float> next(width);
                fill_luma_row(current, begin);
                if (begin == 0U) previous = current;
                else fill_luma_row(previous, begin - 1U);
                if (begin + 1U < input.height) fill_luma_row(next, begin + 1U);
                else next = current;
                for (auto y = begin; y < end; ++y) {
                    pack_row(y, scratch, stats, non_finite_local, rejected_local,
                             previous.data(), current.data(), next.data());
                    if (y + 1U < end) {
                        previous.swap(current);
                        current.swap(next);
                        if (y + 2U < input.height) fill_luma_row(next, y + 2U);
                        else next = current;
                    }
                }
            } else {
                for (auto y = begin; y < end; ++y) {
                    pack_row(y, scratch, stats, non_finite_local, rejected_local,
                             nullptr, nullptr, nullptr);
                }
            }
            if (non_finite_local) non_finite.store(true, std::memory_order_relaxed);
            if (rejected_local) rejected_negative.store(true, std::memory_order_relaxed);
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
