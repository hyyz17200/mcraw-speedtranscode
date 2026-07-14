#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

namespace log_curve_detail {

inline constexpr double di_a = 0.0075;
inline constexpr double di_b = 7.0;
inline constexpr double di_c = 0.07329248;
inline constexpr double di_m = 10.44426855;
inline constexpr double di_linear_cut = 0.00262409;
inline constexpr double di_log_cut = 0.02740668;

} // namespace log_curve_detail

[[nodiscard]] double davinci_intermediate_oetf(double linear);
[[nodiscard]] double davinci_intermediate_eotf(double encoded);

// Per-pipeline cache for the selected transfer curve. It deliberately contains
// no camera/sensor data: camera matrices are evaluated before this 1D curve.
class DaVinciIntermediateLut {
public:
    explicit DaVinciIntermediateLut(std::size_t entries_per_segment = 65'536U);

    [[nodiscard]] inline float encode(float linear) const noexcept {
        if (!std::isfinite(linear)) return std::numeric_limits<float>::quiet_NaN();
        if (linear <= cut_) return linear * slope_;
        if (linear <= 1.0F) {
            return interpolate(low_segment_.data(), (linear - cut_) * low_scale_);
        }
        if (linear <= 100.0F) {
            return interpolate(high_segment_.data(), (linear - 1.0F) * high_scale_);
        }
        return static_cast<float>(davinci_intermediate_oetf(static_cast<double>(linear)));
    }

    [[nodiscard]] inline std::size_t entries_per_segment() const noexcept {
        return low_segment_.size();
    }

private:
    // The per-sample hot path avoids divisions, size loads, and std::lerp
    // edge-case branches: segment scales are precomputed and the clamped
    // index guarantees index + 1 stays inside the table.
    [[nodiscard]] inline float interpolate(const float* values, float scaled) const noexcept {
        const auto index = std::min(static_cast<std::size_t>(scaled), last_index_);
        const float fraction = scaled - static_cast<float>(index);
        const float base = values[index];
        return base + fraction * (values[index + 1U] - base);
    }

    std::vector<float> low_segment_;
    std::vector<float> high_segment_;
    float cut_{};
    float slope_{};
    float low_scale_{};
    float high_scale_{};
    std::size_t last_index_{};
};

[[nodiscard]] TargetLogRgbF32 encode_davinci_intermediate(
    const TargetLinearRgbF32& input,
    NegativePolicy policy);

// Production split-pipeline path: consumes the target-linear storage, applies
// the cached curve in parallel, and returns the same planes as TargetLog RGB.
[[nodiscard]] TargetLogRgbF32 encode_davinci_intermediate_lut(
    TargetLinearRgbF32 input,
    NegativePolicy policy,
    const DaVinciIntermediateLut& curve,
    std::size_t worker_threads = 1);

} // namespace mcraw
