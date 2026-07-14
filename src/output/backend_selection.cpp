#include <mcraw/output/backend_selection.hpp>

#include <mcraw/core/error.hpp>

#if defined(MCRAW_HAS_FFMPEG) && MCRAW_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#endif

namespace mcraw {

BackendCapabilities probe_backend_capabilities() {
    BackendCapabilities result;
#if defined(MCRAW_HAS_FFMPEG) && MCRAW_HAS_FFMPEG
    result.cpu_available = avcodec_find_encoder_by_name("prores_ks") != nullptr;
    result.prores_ks_vulkan_available =
        avcodec_find_encoder_by_name("prores_ks_vulkan") != nullptr;
    result.ffmpeg_version = av_version_info();
    result.ffmpeg_configuration = avcodec_configuration();
#else
    result.cpu_available = false;
#endif
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
    result.vulkan_compiled = true;
    if (!result.prores_ks_vulkan_available) {
        result.vulkan_unavailable_reason =
            "the linked FFmpeg libraries have no prores_ks_vulkan encoder";
    } else {
        result.vulkan_unavailable_reason =
            "the Vulkan encoder is present but runtime integration is not implemented yet";
    }
#else
    result.vulkan_unavailable_reason = "Vulkan support is not compiled into this build";
#endif
    return result;
}

BackendSelection select_backend(const EffectiveConfig& config,
                                const BackendCapabilities& capabilities) {
    config.validate();
    if (config.backend == VideoBackend::cpu) {
        if (!capabilities.cpu_available) {
            throw Error(ErrorCode::encode_failed, "CPU ProRes backend is unavailable");
        }
        return {VideoBackend::cpu, false, "CPU backend explicitly selected"};
    }
    if (capabilities.vulkan_compiled && capabilities.vulkan_backend_available &&
        capabilities.prores_ks_vulkan_available) {
        return {VideoBackend::vulkan, false, "Vulkan ProRes backend is available"};
    }
    const auto reason = capabilities.vulkan_unavailable_reason.empty()
        ? std::string("Vulkan ProRes backend is unavailable")
        : capabilities.vulkan_unavailable_reason;
    if (config.backend == VideoBackend::vulkan) {
        throw Error(ErrorCode::unsupported_format,
                    "Vulkan backend was forced but is unavailable: " + reason);
    }
    if (config.fallback == GpuFallback::none) {
        throw Error(ErrorCode::unsupported_format,
                    "automatic backend selection cannot fall back to CPU: " + reason);
    }
    if (!capabilities.cpu_available) {
        throw Error(ErrorCode::encode_failed,
                    "Vulkan backend is unavailable and the CPU fallback is unavailable: " + reason);
    }
    return {VideoBackend::cpu, true, reason};
}

} // namespace mcraw
