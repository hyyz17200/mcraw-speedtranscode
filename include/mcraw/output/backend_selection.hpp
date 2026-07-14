#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <mcraw/core/config.hpp>

namespace mcraw {

struct BackendCapabilities {
    bool cpu_available{true};
    bool vulkan_compiled{};
    bool vulkan_backend_available{};
    bool prores_ks_vulkan_available{};
    std::string vulkan_unavailable_reason;
    std::string ffmpeg_version;
    std::string ffmpeg_configuration;
};

struct BackendSelection {
    VideoBackend backend{VideoBackend::cpu};
    bool used_fallback{};
    std::string reason;
};

// Task 2 intentionally exposes a conservative stub. Task 4 replaces the
// Vulkan fields with runtime device and linked-libavcodec probes.
[[nodiscard]] BackendCapabilities probe_backend_capabilities();
[[nodiscard]] BackendCapabilities probe_backend_capabilities(
    std::string_view gpu_selector, int width, int height, std::size_t pool_size);
[[nodiscard]] BackendSelection select_backend(const EffectiveConfig& config,
                                               const BackendCapabilities& capabilities);

} // namespace mcraw
