#include <mcraw/core/execution_plan.hpp>

namespace mcraw {

std::size_t automatic_frame_worker_limit(const EffectiveConfig& config) noexcept {
    if (config.backend != VideoBackend::cpu &&
        config.gpu_performance_mode == GpuPerformanceMode::fast) {
        return 2U;
    }
    return 6U;
}

} // namespace mcraw
