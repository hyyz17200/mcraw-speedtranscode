#include <mcraw/processing/raw_chroma_denoise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

std::size_t index_of(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return static_cast<std::size_t>(y) * width + x;
}

std::size_t cfa_position(std::uint32_t x, std::uint32_t y) {
    return static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
}

double model_variance(const NoiseModel& model, double signal) {
    const double normalized = std::clamp(signal, 0.0, 1.0);
    return std::max(0.0, model.scale * normalized + model.offset);
}

double green_estimate(const RawNormalizedF32& input, std::uint32_t x, std::uint32_t y) {
    return 0.25 * (
        static_cast<double>(input.pixels[index_of(x - 1U, y, input.width)]) +
        static_cast<double>(input.pixels[index_of(x + 1U, y, input.width)]) +
        static_cast<double>(input.pixels[index_of(x, y - 1U, input.width)]) +
        static_cast<double>(input.pixels[index_of(x, y + 1U, input.width)]));
}

double green_estimate_variance(const RawNormalizedF32& input,
                               const std::array<NoiseModel, 4>& profile,
                               std::uint32_t x,
                               std::uint32_t y) {
    const std::array<std::array<std::uint32_t, 2>, 4> positions{{
        {x - 1U, y}, {x + 1U, y}, {x, y - 1U}, {x, y + 1U}
    }};
    double sum = 0.0;
    for (const auto& position : positions) {
        const auto sample = static_cast<double>(
            input.pixels[index_of(position[0], position[1], input.width)]);
        sum += model_variance(profile[cfa_position(position[0], position[1])], sample);
    }
    // Variance of the mean of four independent green samples.
    return sum / 16.0;
}

} // namespace

RawNormalizedF32 denoise_raw_chroma(const RawNormalizedF32& input,
                                    const NormalizedCameraMetadata& metadata,
                                    double strength,
                                    std::size_t worker_threads) {
    input.validate();
    metadata.validate_for_raw();
    if (input.width != metadata.width || input.height != metadata.height ||
        input.cfa != metadata.cfa) {
        throw Error(ErrorCode::invalid_argument,
                    "RAW image and normalized metadata do not agree");
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 2.0) {
        throw Error(ErrorCode::invalid_argument,
                    "raw chroma denoise strength must be between 0 and 2");
    }
    if (strength == 0.0) return input;
    if (!metadata.noise_profile) {
        throw Error(ErrorCode::invalid_metadata,
                    "raw chroma denoise requires per-frame noiseProfile metadata");
    }
    if (input.width < 8U || input.height < 8U) {
        throw Error(ErrorCode::invalid_argument,
                    "raw chroma denoise requires at least an 8x8 frame");
    }

    RawNormalizedF32 output = input;
    const auto& profile = *metadata.noise_profile;
    const int thread_count = static_cast<int>(std::max<std::size_t>(1U, worker_threads));

#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t row = 3; row < static_cast<std::int64_t>(input.height) - 3; ++row) {
        const auto y = static_cast<std::uint32_t>(row);
        for (std::uint32_t x = 3U; x + 3U < input.width; ++x) {
            const unsigned color = cfa_color(input.cfa, x, y);
            if (color == 1U) continue;

            const std::array<std::array<int, 2>, 5> offsets{{
                {0, 0}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}
            }};
            std::array<double, 5> chroma{};
            std::array<double, 5> green{};
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                const auto sx = static_cast<std::uint32_t>(
                    static_cast<int>(x) + offsets[i][0]);
                const auto sy = static_cast<std::uint32_t>(
                    static_cast<int>(y) + offsets[i][1]);
                green[i] = green_estimate(input, sx, sy);
                chroma[i] = static_cast<double>(
                    input.pixels[index_of(sx, sy, input.width)]) - green[i];
            }

            const auto center_index = index_of(x, y, input.width);
            const double color_variance = model_variance(
                profile[cfa_position(x, y)], static_cast<double>(input.pixels[center_index]));
            const double noise_variance = std::max(
                1.0e-12, color_variance + green_estimate_variance(input, profile, x, y));

            std::array<double, 5> weights{};
            double weight_sum = 0.0;
            double mean = 0.0;
            for (std::size_t i = 0; i < weights.size(); ++i) {
                const double green_delta = green[i] - green[0];
                const double chroma_delta = chroma[i] - chroma[0];
                weights[i] = 1.0 / (1.0 + green_delta * green_delta /
                                             (9.0 * noise_variance) +
                                             chroma_delta * chroma_delta /
                                             (16.0 * noise_variance));
                weight_sum += weights[i];
                mean += weights[i] * chroma[i];
            }
            mean /= weight_sum;

            double local_variance = 0.0;
            for (std::size_t i = 0; i < weights.size(); ++i) {
                const double delta = chroma[i] - mean;
                local_variance += weights[i] * delta * delta;
            }
            local_variance /= weight_sum;

            const double effective_noise = strength * strength * noise_variance;
            const double detail_gain = local_variance /
                (local_variance + effective_noise + 1.0e-20);
            const double filtered_chroma = mean + detail_gain * (chroma[0] - mean);
            output.pixels[center_index] = static_cast<float>(green[0] + filtered_chroma);
        }
    }
    output.validate();
    return output;
}

} // namespace mcraw
