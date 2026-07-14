#pragma once

#include <array>
#include <cstddef>

#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

struct Matrix3d {
    std::array<double, 9> v{};

    [[nodiscard]] static Matrix3d identity() noexcept;
    [[nodiscard]] double determinant() const noexcept;
    [[nodiscard]] Matrix3d inverse() const;
    [[nodiscard]] Matrix3d operator*(const Matrix3d& other) const noexcept;
    [[nodiscard]] std::array<double, 3> operator*(const std::array<double, 3>& vector) const noexcept;
};

struct WhitePointSolution {
    double x{};
    double y{};
    double cct{};
    double interpolation_weight{};
    std::size_t iterations{};
};

struct CameraColorSolution {
    WhitePointSolution white_point;
    Matrix3d camera_to_xyz_d50;
    Matrix3d xyz_d50_to_target;
    Matrix3d camera_to_target;
    bool used_forward_matrix{};
};

[[nodiscard]] double correlated_color_temperature(double x, double y);
[[nodiscard]] WhitePointSolution solve_camera_neutral(const NormalizedCameraMetadata& metadata);
[[nodiscard]] CameraColorSolution build_camera_color_solution(const NormalizedCameraMetadata& metadata);
[[nodiscard]] TargetLinearRgbF32 camera_to_dwg(
    const CameraRgbF32& input,
    const CameraColorSolution& solution,
    double exposure_offset_stops = 0.0);

} // namespace mcraw

