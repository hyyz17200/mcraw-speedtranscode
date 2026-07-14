#include <mcraw/processing/log_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mcraw/core/error.hpp>

namespace mcraw {

double davinci_intermediate_oetf(double linear) {
    if (!std::isfinite(linear)) {
        throw Error(ErrorCode::processing_failed, "DaVinci Intermediate input is not finite");
    }
    if (linear <= log_curve_detail::di_linear_cut) return linear * log_curve_detail::di_m;
    return (std::log2(linear + log_curve_detail::di_a) + log_curve_detail::di_b) *
           log_curve_detail::di_c;
}

double davinci_intermediate_eotf(double encoded) {
    if (!std::isfinite(encoded)) {
        throw Error(ErrorCode::processing_failed, "DaVinci Intermediate code is not finite");
    }
    if (encoded <= log_curve_detail::di_log_cut) return encoded / log_curve_detail::di_m;
    return std::exp2(encoded / log_curve_detail::di_c - log_curve_detail::di_b) -
           log_curve_detail::di_a;
}

DaVinciIntermediateLut::DaVinciIntermediateLut(std::size_t entries_per_segment) {
    if (entries_per_segment < 2U) {
        throw Error(ErrorCode::invalid_argument,
                    "DaVinci Intermediate LUT requires at least two entries per segment");
    }
    low_segment_.resize(entries_per_segment);
    high_segment_.resize(entries_per_segment);
    const auto populate = [](std::vector<float>& values, double minimum, double maximum) {
        const double denominator = static_cast<double>(values.size() - 1U);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double t = static_cast<double>(i) / denominator;
            values[i] = static_cast<float>(davinci_intermediate_oetf(
                std::lerp(minimum, maximum, t)));
        }
    };
    populate(low_segment_, log_curve_detail::di_linear_cut, 1.0);
    populate(high_segment_, 1.0, 100.0);
}

TargetLogRgbF32 encode_davinci_intermediate(const TargetLinearRgbF32& input,
                                            NegativePolicy policy) {
    input.validate();
    TargetLogRgbF32 output{input.width, input.height, {}};
    for (auto& plane : output.planes) plane.resize(input.planes[0].size());
    for (std::size_t channel = 0; channel < 3; ++channel) {
        for (std::size_t i = 0; i < input.planes[channel].size(); ++i) {
            double value = input.planes[channel][i];
            if (value < 0.0) {
                if (policy == NegativePolicy::clamp_zero) value = 0.0;
                if (policy == NegativePolicy::error) {
                    throw Error(ErrorCode::processing_failed, "negative target-linear value rejected by policy");
                }
            }
            output.planes[channel][i] = static_cast<float>(davinci_intermediate_oetf(value));
        }
    }
    return output;
}

} // namespace mcraw
