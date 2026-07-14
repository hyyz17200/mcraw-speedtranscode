#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>
#include <mcraw/core/metadata.hpp>

namespace {

nlohmann::json container_metadata() {
    return {
        {"blackLevel", {64, 65, 66, 67}},
        {"whiteLevel", 1023},
        {"sensorArrangment", "rggb"},
        {"colorMatrix1", {1,0,0, 0,1,0, 0,0,1}},
        {"colorMatrix2", {0.9,0.05,0.05, 0.02,0.96,0.02, 0.01,0.03,0.96}}
    };
}

nlohmann::json frame_metadata() {
    return {{"width", 4}, {"height", 2}, {"compressionType", 7},
            {"asShotNeutral", {0.95, 1.0, 1.08}}};
}

} // namespace

TEST_CASE("MotionCam historical spelling and documented illuminant convention are visible") {
    const auto value = mcraw::normalize_metadata(container_metadata(), frame_metadata());
    REQUIRE(value.width == 4);
    REQUIRE(value.cfa == mcraw::CfaPattern::rggb);
    REQUIRE(value.black_level[2] == 66);
    REQUIRE(value.white_level[3] == 1023);
    REQUIRE(value.illuminant1_cct == 6504.0);
    REQUIRE(value.illuminant2_cct == 2856.0);
    REQUIRE(value.warnings.size() == 3);
}

TEST_CASE("unknown CFA fails instead of guessing") {
    auto container = container_metadata();
    container["sensorArrangment"] = "xtrans";
    REQUIRE_THROWS_AS(mcraw::normalize_metadata(container, frame_metadata()), mcraw::Error);
}

TEST_CASE("MotionCam named illuminants preserve their matrix ordering") {
    auto container = container_metadata();
    container["colorIlluminant1"] = "standarda";
    container["colorIlluminant2"] = "d65";
    const auto value = mcraw::normalize_metadata(container, frame_metadata());
    REQUIRE(value.illuminant1_cct == 2856.0);
    REQUIRE(value.illuminant2_cct == 6504.0);
}

TEST_CASE("per-CFA-position noise profile is normalized and retained") {
    auto frame = frame_metadata();
    frame["noiseProfile"] = {0.01, 0.001, 0.02, 0.002, 0.03, 0.003, 0.04, 0.004};
    const auto value = mcraw::normalize_metadata(container_metadata(), frame);
    REQUIRE(value.noise_profile.has_value());
    REQUIRE(value.noise_profile->at(0).scale == 0.01);
    REQUIRE(value.noise_profile->at(3).offset == 0.004);
}
