#pragma once

#include <cstddef>

#include <mcraw/core/config.hpp>

namespace mcraw {

[[nodiscard]] std::size_t automatic_frame_worker_limit(
    const EffectiveConfig& config) noexcept;

} // namespace mcraw
