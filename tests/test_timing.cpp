#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/timing.hpp>

TEST_CASE("stage timing reports deterministic percentiles") {
    mcraw::StageTimings timings;
    for (int value = 1; value <= 100; ++value) timings.add("stage", static_cast<double>(value));
    const auto summary = timings.summary("stage");
    REQUIRE(summary.samples == 100);
    REQUIRE(summary.mean_ms == Catch::Approx(50.5));
    REQUIRE(summary.p50_ms == Catch::Approx(50.5));
    REQUIRE(summary.p95_ms == Catch::Approx(95.05));
}

