#include <mcraw/core/execution_plan.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("automatic frame worker limits follow the measured GPU mode capacity") {
    mcraw::EffectiveConfig config;

    config.backend = mcraw::VideoBackend::vulkan;
    config.gpu_performance_mode = mcraw::GpuPerformanceMode::precise;
    REQUIRE(mcraw::automatic_frame_worker_limit(config) == 6U);

    config.gpu_performance_mode = mcraw::GpuPerformanceMode::fast;
    REQUIRE(mcraw::automatic_frame_worker_limit(config) == 2U);

    config.backend = mcraw::VideoBackend::automatic;
    REQUIRE(mcraw::automatic_frame_worker_limit(config) == 2U);

    config.backend = mcraw::VideoBackend::cpu;
    REQUIRE(mcraw::automatic_frame_worker_limit(config) == 6U);
}
