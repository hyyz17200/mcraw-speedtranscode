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
        if (linear <= static_cast<float>(log_curve_detail::di_linear_cut)) {
            return linear * static_cast<float>(log_curve_detail::di_m);
        }
        if (linear <= 1.0F) {
            return interpolate(low_segment_, linear,
                               static_cast<float>(log_curve_detail::di_linear_cut), 1.0F);
        }
        if (linear <= 100.0F) return interpolate(high_segment_, linear, 1.0F, 100.0F);
        return static_cast<float>(davinci_intermediate_oetf(static_cast<double>(linear)));
    }

    [[nodiscard]] inline std::size_t entries_per_segment() const noexcept {
        return low_segment_.size();
    }

private:
    [[nodiscard]] inline float interpolate(const std::vector<float>& values,
                                           float value,
                                           float minimum,
                                           float maximum) const noexcept {
        const float scaled = (value - minimum) * static_cast<float>(values.size() - 1U) /
                             (maximum - minimum);
        const auto index = static_cast<std::size_t>(scaled);
        if (index + 1U >= values.size()) return values.back();
        const float fraction = scaled - static_cast<float>(index);
        return std::lerp(values[index], values[index + 1U], fraction);
    }

    std::vector<float> low_segment_;
    std::vector<float> high_segment_;
};

[[nodiscard]] TargetLogRgbF32 encode_davinci_intermediate(
    const TargetLinearRgbF32& input,
    NegativePolicy policy);

} // namespace mcraw
