#pragma once

#include <cstddef>
#include <vector>

#include <mcraw/core/config.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

[[nodiscard]] double davinci_intermediate_oetf(double linear);
[[nodiscard]] double davinci_intermediate_eotf(double encoded);

// Per-pipeline cache for the selected transfer curve. It deliberately contains
// no camera/sensor data: camera matrices are evaluated before this 1D curve.
class DaVinciIntermediateLut {
public:
    explicit DaVinciIntermediateLut(std::size_t entries_per_segment = 65'536U);

    [[nodiscard]] float encode(float linear) const noexcept;
    [[nodiscard]] std::size_t entries_per_segment() const noexcept;

private:
    [[nodiscard]] float interpolate(const std::vector<float>& values,
                                    float value,
                                    float minimum,
                                    float maximum) const noexcept;

    std::vector<float> low_segment_;
    std::vector<float> high_segment_;
};

[[nodiscard]] TargetLogRgbF32 encode_davinci_intermediate(
    const TargetLinearRgbF32& input,
    NegativePolicy policy);

} // namespace mcraw
