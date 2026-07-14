#include <mcraw/processing/color.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

Matrix3d from_metadata(const std::optional<MatrixMetadata>& value, const Matrix3d& fallback) {
    return value ? Matrix3d{value->values} : fallback;
}

Matrix3d interpolate(const Matrix3d& a, const Matrix3d& b, double weight) {
    Matrix3d result;
    for (std::size_t i = 0; i < result.v.size(); ++i) {
        result.v[i] = std::lerp(a.v[i], b.v[i], weight);
    }
    return result;
}

double interpolation_weight(const NormalizedCameraMetadata& metadata, double cct) {
    if (!metadata.color_matrix2) return 0.0;
    const double reciprocal1 = 1.0 / metadata.illuminant1_cct;
    const double reciprocal2 = 1.0 / metadata.illuminant2_cct;
    const double denominator = reciprocal2 - reciprocal1;
    if (std::abs(denominator) < 1.0e-15) {
        throw Error(ErrorCode::invalid_metadata, "dual illuminants have indistinguishable CCT values");
    }
    return std::clamp(((1.0 / cct) - reciprocal1) / denominator, 0.0, 1.0);
}

Matrix3d color_matrix(const NormalizedCameraMetadata& metadata, double weight) {
    const Matrix3d first{metadata.color_matrix1->values};
    if (!metadata.color_matrix2) return first;
    return interpolate(first, Matrix3d{metadata.color_matrix2->values}, weight);
}

Matrix3d calibration_matrix(const NormalizedCameraMetadata& metadata, double weight) {
    const auto identity = Matrix3d::identity();
    const auto first = from_metadata(metadata.calibration_matrix1, identity);
    if (!metadata.color_matrix2) return first;
    const auto second = from_metadata(metadata.calibration_matrix2, identity);
    return interpolate(first, second, weight);
}

std::optional<Matrix3d> forward_matrix(const NormalizedCameraMetadata& metadata, double weight) {
    if (!metadata.forward_matrix1 && !metadata.forward_matrix2) return std::nullopt;
    if (metadata.forward_matrix1 && metadata.forward_matrix2) {
        return interpolate(Matrix3d{metadata.forward_matrix1->values},
                           Matrix3d{metadata.forward_matrix2->values}, weight);
    }
    return Matrix3d{(metadata.forward_matrix1 ? metadata.forward_matrix1 : metadata.forward_matrix2)->values};
}

std::array<double, 2> planckian_xy(double temperature) {
    const double t = std::clamp(temperature, 1667.0, 25000.0);
    double x = 0.0;
    if (t <= 4000.0) {
        x = -0.2661239e9 / (t * t * t) - 0.2343580e6 / (t * t) + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
    }
    double y = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }
    return {x, y};
}

std::array<double, 2> xy_to_uv(double x, double y) {
    const double denominator = -2.0 * x + 12.0 * y + 3.0;
    if (std::abs(denominator) < 1.0e-15) {
        throw Error(ErrorCode::processing_failed, "chromaticity cannot be converted to UCS");
    }
    return {4.0 * x / denominator, 6.0 * y / denominator};
}

std::array<double, 3> xy_to_xyz(double x, double y) {
    if (!(x > 0.0 && y > 0.0 && x + y < 1.0)) {
        throw Error(ErrorCode::processing_failed, "invalid white-point chromaticity");
    }
    return {x / y, 1.0, (1.0 - x - y) / y};
}

Matrix3d diagonal(const std::array<double, 3>& values) {
    return Matrix3d{{values[0], 0.0, 0.0, 0.0, values[1], 0.0, 0.0, 0.0, values[2]}};
}

Matrix3d bradford_adaptation(double source_x, double source_y, double target_x, double target_y) {
    const Matrix3d bradford{{
        0.8951, 0.2664, -0.1614,
       -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296
    }};
    const auto source_lms = bradford * xy_to_xyz(source_x, source_y);
    const auto target_lms = bradford * xy_to_xyz(target_x, target_y);
    std::array<double, 3> scale{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(source_lms[i]) < 1.0e-15) {
            throw Error(ErrorCode::processing_failed, "Bradford source white is singular");
        }
        scale[i] = target_lms[i] / source_lms[i];
    }
    return bradford.inverse() * diagonal(scale) * bradford;
}

constexpr double d50_x = 0.34567;
constexpr double d50_y = 0.35850;
constexpr double d65_x = 0.31270;
constexpr double d65_y = 0.32900;

const Matrix3d xyz_d65_to_dwg{{
     1.51667204, -0.28147805, -0.14696363,
    -0.46491710,  1.25142378,  0.17488461,
     0.06484905,  0.10913934,  0.76141462
}};

} // namespace

Matrix3d Matrix3d::identity() noexcept {
    return {{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}}};
}

double Matrix3d::determinant() const noexcept {
    return v[0] * (v[4] * v[8] - v[5] * v[7])
         - v[1] * (v[3] * v[8] - v[5] * v[6])
         + v[2] * (v[3] * v[7] - v[4] * v[6]);
}

Matrix3d Matrix3d::inverse() const {
    const double d = determinant();
    if (!std::isfinite(d) || std::abs(d) < 1.0e-12) {
        throw Error(ErrorCode::invalid_metadata, "matrix is singular or ill-conditioned");
    }
    Matrix3d result{{{
        (v[4] * v[8] - v[5] * v[7]) / d,
        (v[2] * v[7] - v[1] * v[8]) / d,
        (v[1] * v[5] - v[2] * v[4]) / d,
        (v[5] * v[6] - v[3] * v[8]) / d,
        (v[0] * v[8] - v[2] * v[6]) / d,
        (v[2] * v[3] - v[0] * v[5]) / d,
        (v[3] * v[7] - v[4] * v[6]) / d,
        (v[1] * v[6] - v[0] * v[7]) / d,
        (v[0] * v[4] - v[1] * v[3]) / d
    }}};
    const double norm = std::max({
        std::abs(v[0]) + std::abs(v[1]) + std::abs(v[2]),
        std::abs(v[3]) + std::abs(v[4]) + std::abs(v[5]),
        std::abs(v[6]) + std::abs(v[7]) + std::abs(v[8])
    });
    const double inverse_norm = std::max({
        std::abs(result.v[0]) + std::abs(result.v[1]) + std::abs(result.v[2]),
        std::abs(result.v[3]) + std::abs(result.v[4]) + std::abs(result.v[5]),
        std::abs(result.v[6]) + std::abs(result.v[7]) + std::abs(result.v[8])
    });
    if (!std::isfinite(norm * inverse_norm) || norm * inverse_norm > 1.0e10) {
        throw Error(ErrorCode::invalid_metadata, "matrix condition number exceeds the safe limit");
    }
    return result;
}

Matrix3d Matrix3d::operator*(const Matrix3d& other) const noexcept {
    Matrix3d result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t k = 0; k < 3; ++k) {
                result.v[row * 3 + column] += v[row * 3 + k] * other.v[k * 3 + column];
            }
        }
    }
    return result;
}

std::array<double, 3> Matrix3d::operator*(const std::array<double, 3>& vector) const noexcept {
    return {
        v[0] * vector[0] + v[1] * vector[1] + v[2] * vector[2],
        v[3] * vector[0] + v[4] * vector[1] + v[5] * vector[2],
        v[6] * vector[0] + v[7] * vector[1] + v[8] * vector[2]
    };
}

double correlated_color_temperature(double x, double y) {
    const auto target = xy_to_uv(x, y);
    auto distance = [&](double temperature) {
        const auto xy = planckian_xy(temperature);
        const auto uv = xy_to_uv(xy[0], xy[1]);
        const double du = uv[0] - target[0];
        const double dv = uv[1] - target[1];
        return du * du + dv * dv;
    };

    double lo = 1667.0;
    double hi = 25000.0;
    constexpr double phi = 0.6180339887498948482;
    double c = hi - phi * (hi - lo);
    double d = lo + phi * (hi - lo);
    double fc = distance(c);
    double fd = distance(d);
    for (int i = 0; i < 96; ++i) {
        if (fc < fd) {
            hi = d;
            d = c;
            fd = fc;
            c = hi - phi * (hi - lo);
            fc = distance(c);
        } else {
            lo = c;
            c = d;
            fc = fd;
            d = lo + phi * (hi - lo);
            fd = distance(d);
        }
    }
    return (lo + hi) * 0.5;
}

WhitePointSolution solve_camera_neutral(const NormalizedCameraMetadata& metadata) {
    metadata.validate_for_color();
    double x = d65_x;
    double y = d65_y;
    double weight = 0.0;
    constexpr std::size_t max_iterations = 50;
    for (std::size_t iteration = 1; iteration <= max_iterations; ++iteration) {
        const double cct = correlated_color_temperature(x, y);
        weight = interpolation_weight(metadata, cct);
        const Matrix3d xyz_to_camera = calibration_matrix(metadata, weight) * color_matrix(metadata, weight);
        auto xyz = xyz_to_camera.inverse() * metadata.camera_neutral;
        const double sum = xyz[0] + xyz[1] + xyz[2];
        if (!std::isfinite(sum) || std::abs(sum) < 1.0e-15) {
            throw Error(ErrorCode::processing_failed, "CameraNeutral produced an invalid white point");
        }
        const double next_x = xyz[0] / sum;
        const double next_y = xyz[1] / sum;
        if (!(next_x > 0.0 && next_y > 0.0 && next_x + next_y < 1.0)) {
            throw Error(ErrorCode::processing_failed, "CameraNeutral iteration left the chromaticity domain");
        }
        if (std::max(std::abs(next_x - x), std::abs(next_y - y)) < 1.0e-10) {
            const double final_cct = correlated_color_temperature(next_x, next_y);
            return {next_x, next_y, final_cct, interpolation_weight(metadata, final_cct), iteration};
        }
        x = next_x;
        y = next_y;
    }
    throw Error(ErrorCode::processing_failed, "CameraNeutral to xy iteration did not converge");
}

CameraColorSolution build_camera_color_solution(const NormalizedCameraMetadata& metadata) {
    const auto white = solve_camera_neutral(metadata);
    const auto cc = calibration_matrix(metadata, white.interpolation_weight);
    Matrix3d camera_to_xyz_d50{};
    bool used_forward = false;
    if (const auto fm = forward_matrix(metadata, white.interpolation_weight)) {
        const auto reference_neutral = cc.inverse() * metadata.camera_neutral;
        std::array<double, 3> reciprocal{};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!std::isfinite(reference_neutral[i]) || std::abs(reference_neutral[i]) < 1.0e-12) {
                throw Error(ErrorCode::invalid_metadata, "ReferenceNeutral is singular");
            }
            reciprocal[i] = 1.0 / reference_neutral[i];
        }
        camera_to_xyz_d50 = *fm * diagonal(reciprocal) * cc.inverse();
        used_forward = true;
    } else {
        const auto camera_to_xyz_at_white = (cc * color_matrix(metadata, white.interpolation_weight)).inverse();
        camera_to_xyz_d50 = bradford_adaptation(white.x, white.y, d50_x, d50_y) * camera_to_xyz_at_white;
    }
    const auto xyz_d50_to_target = xyz_d65_to_dwg * bradford_adaptation(d50_x, d50_y, d65_x, d65_y);
    return {white, camera_to_xyz_d50, xyz_d50_to_target,
            xyz_d50_to_target * camera_to_xyz_d50, used_forward};
}

TargetLinearRgbF32 camera_to_dwg(const CameraRgbF32& input,
                                 const CameraColorSolution& solution,
                                 double exposure_offset_stops,
                                 double input_scale,
                                 std::size_t worker_threads) {
    input.validate();
    if (!std::isfinite(input_scale) || input_scale <= 0.0) {
        throw Error(ErrorCode::invalid_argument,
                    "camera RGB input scale must be finite and positive");
    }
    TargetLinearRgbF32 output{input.width, input.height, {}};
    for (auto& plane : output.planes) plane.resize(input.planes[0].size());
    const double exposure = std::exp2(exposure_offset_stops) * input_scale;
    const int thread_count = static_cast<int>(
        std::clamp<std::size_t>(worker_threads, 1U, 256U));
    const auto pixels = static_cast<std::int64_t>(input.planes[0].size());
#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t raw_index = 0; raw_index < pixels; ++raw_index) {
        const auto i = static_cast<std::size_t>(raw_index);
        const auto transformed = solution.camera_to_target * std::array<double, 3>{
            input.planes[0][i], input.planes[1][i], input.planes[2][i]
        };
        for (std::size_t channel = 0; channel < 3; ++channel) {
            output.planes[channel][i] = static_cast<float>(transformed[channel] * exposure);
        }
    }
    return output;
}

TargetLinearRgbF32 sharpen_target_linear(TargetLinearRgbF32 input,
                                         double amount,
                                         double threshold,
                                         std::size_t worker_threads) {
    input.validate();
    if (!std::isfinite(amount) || amount < 0.0 ||
        !std::isfinite(threshold) || threshold < 0.0) {
        throw Error(ErrorCode::invalid_argument,
                    "target-linear sharpening parameters must be finite and non-negative");
    }
    if (amount <= 0.0) return input;
    constexpr double kr = 0.2627;
    constexpr double kb = 0.0593;
    constexpr double kg = 1.0 - kr - kb;
    const int thread_count = static_cast<int>(
        std::clamp<std::size_t>(worker_threads, 1U, 256U));
    const auto pixel_count = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> luma(pixel_count);
#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t raw_pixel = 0;
         raw_pixel < static_cast<std::int64_t>(pixel_count); ++raw_pixel) {
        const auto pixel = static_cast<std::size_t>(raw_pixel);
        luma[pixel] = static_cast<float>(
            kr * input.planes[0][pixel] + kg * input.planes[1][pixel] +
            kb * input.planes[2][pixel]);
    }
    const auto luma_at = [&](std::uint32_t x, std::uint32_t y) {
        return luma[static_cast<std::size_t>(y) * input.width + x];
    };
#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (std::int64_t raw_y = 0;
         raw_y < static_cast<std::int64_t>(input.height); ++raw_y) {
        const auto y = static_cast<std::uint32_t>(raw_y);
        for (std::uint32_t x = 0; x < input.width; ++x) {
            const auto left = x == 0U ? 0U : x - 1U;
            const auto right = std::min(x + 1U, input.width - 1U);
            const auto up = y == 0U ? 0U : y - 1U;
            const auto down = std::min(y + 1U, input.height - 1U);
            const double detail = luma_at(x, y) - 0.25 * (
                luma_at(left, y) + luma_at(right, y) +
                luma_at(x, up) + luma_at(x, down));
            if (std::abs(detail) <= threshold) continue;
            const auto pixel = static_cast<std::size_t>(y) * input.width + x;
            const float delta = static_cast<float>(amount * std::copysign(
                std::abs(detail) - threshold, detail));
            for (auto& plane : input.planes) plane[pixel] += delta;
        }
    }
    input.validate();
    return input;
}

} // namespace mcraw
