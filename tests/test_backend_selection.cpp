#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include <mcraw/core/config.hpp>
#include <mcraw/core/error.hpp>
#include <mcraw/output/backend_selection.hpp>

TEST_CASE("automatic backend selection falls back to the CPU with a reason") {
    mcraw::EffectiveConfig config;
    config.backend = mcraw::VideoBackend::automatic;
    mcraw::BackendCapabilities capabilities;
    capabilities.vulkan_unavailable_reason = "test probe failure";

    const auto selection = mcraw::select_backend(config, capabilities);

    CHECK(selection.backend == mcraw::VideoBackend::cpu);
    CHECK(selection.used_fallback);
    CHECK(selection.reason == "test probe failure");
}

TEST_CASE("forced Vulkan backend never silently falls back") {
    mcraw::EffectiveConfig config;
    config.backend = mcraw::VideoBackend::vulkan;
    mcraw::BackendCapabilities capabilities;
    capabilities.vulkan_unavailable_reason = "encoder missing";

    CHECK_THROWS_AS(mcraw::select_backend(config, capabilities), mcraw::Error);
}

TEST_CASE("automatic selection uses Vulkan only when the complete backend is ready") {
    mcraw::EffectiveConfig config;
    config.backend = mcraw::VideoBackend::automatic;
    mcraw::BackendCapabilities capabilities;
    capabilities.vulkan_compiled = true;
    capabilities.vulkan_backend_available = true;
    capabilities.prores_ks_vulkan_available = true;

    const auto selection = mcraw::select_backend(config, capabilities);

    CHECK(selection.backend == mcraw::VideoBackend::vulkan);
    CHECK_FALSE(selection.used_fallback);
}

TEST_CASE("Vulkan RAW backend rejects unsupported demosaic before output") {
    mcraw::EffectiveConfig config;
    config.backend = mcraw::VideoBackend::automatic;
    config.demosaic = mcraw::DemosaicAlgorithm::amaze;
    mcraw::BackendCapabilities capabilities;
    capabilities.vulkan_compiled = true;
    capabilities.vulkan_backend_available = true;
    capabilities.prores_ks_vulkan_available = true;

    const auto selection = mcraw::select_backend(config, capabilities);
    CHECK(selection.backend == mcraw::VideoBackend::cpu);
    CHECK(selection.used_fallback);
    CHECK(selection.reason.find("only precise RCD") != std::string::npos);

    config.backend = mcraw::VideoBackend::vulkan;
    CHECK_THROWS_AS(mcraw::select_backend(config, capabilities), mcraw::Error);
}

TEST_CASE("GPU backend configuration is serialized without changing CPU defaults") {
    mcraw::EffectiveConfig config;
    const auto json = mcraw::config_to_json(config);

    CHECK(json.at("backend") == "cpu");
    CHECK(json.at("gpu_selector") == "auto");
    CHECK(json.at("async_depth") == 8);
    CHECK(json.at("fallback") == "prores_ks");
    CHECK_FALSE(json.contains("precision"));
    CHECK(json.at("gpu_performance_mode") == "precise");
}

TEST_CASE("GPU performance modes have stable serialized identities") {
    mcraw::EffectiveConfig config;
    CHECK(mcraw::config_to_json(config).at("gpu_performance_mode") == "precise");
    config.gpu_performance_mode = mcraw::GpuPerformanceMode::fast;
    CHECK(mcraw::config_to_json(config).at("gpu_performance_mode") == "fast");
}

TEST_CASE("all FFmpeg ProRes profiles are accepted and serialized") {
    for (const auto* profile : {"proxy", "lt", "standard", "hq", "4444", "4444xq"}) {
        mcraw::EffectiveConfig config;
        config.prores_profile = profile;
        CHECK_NOTHROW(config.validate());
        CHECK(mcraw::config_to_json(config).at("prores_profile") == profile);
    }

    mcraw::EffectiveConfig invalid;
    invalid.prores_profile = "unknown";
    CHECK_THROWS_AS(invalid.validate(), mcraw::Error);
}

TEST_CASE("device loss has a stable machine-readable error taxonomy") {
    CHECK(std::string_view(mcraw::error_code_name(mcraw::ErrorCode::device_lost)) ==
          "device_lost");
}
