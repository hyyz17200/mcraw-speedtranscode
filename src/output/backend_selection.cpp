#include <mcraw/output/backend_selection.hpp>

#include <iterator>

#include <mcraw/core/error.hpp>
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
#include <mcraw/output/ffmpeg_vulkan_context.hpp>
#include <mcraw/output/vulkan_prores_encoder.hpp>
#include <mcraw/vulkan/vulkan_rgb_to_yuv.hpp>
#include <mcraw/vulkan/vulkan_runtime.hpp>
#endif

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
            "the Vulkan encoder is present but a device/frame-context preflight was not requested";
    }
#else
    result.vulkan_unavailable_reason = "Vulkan support is not compiled into this build";
#endif
    return result;
}

BackendCapabilities probe_backend_capabilities(std::string_view gpu_selector,
                                               int width,
                                               int height,
                                               std::size_t pool_size,
                                               ChromaFilter chroma_filter,
                                               bool deterministic_dither,
                                               GpuPrecision precision) {
    auto result = probe_backend_capabilities();
#if defined(MCRAW_HAS_VULKAN) && MCRAW_HAS_VULKAN
    if (!result.prores_ks_vulkan_available) return result;
    try {
        VulkanRuntime runtime({std::string(gpu_selector), false});
        // The functional smoke below is intentionally small.  The full-sized
        // frame pool and encoder are owned by the real writer; constructing
        // them here would duplicate the expensive preflight allocation.
        static_cast<void>(width);
        static_cast<void>(height);
        static_cast<void>(pool_size);
        constexpr int smoke_width = 64;
        constexpr int smoke_height = 32;
        FfmpegVulkanFrameContext smoke_frames(runtime, {smoke_width, smoke_height, 4});
        VulkanRgbToYuvFrameWriter smoke_writer(
            runtime, smoke_frames,
            {smoke_width, smoke_height, chroma_filter, deterministic_dither,
             precision, 2});
        VulkanProResEncoder smoke_encoder(
            smoke_frames,
            {smoke_width, smoke_height, {1, 90'000}, {30, 1}, "hq", 2});
        TargetLogRgbF32 neutral;
        neutral.width = smoke_width;
        neutral.height = smoke_height;
        for (auto& plane : neutral.planes) {
            plane.assign(static_cast<std::size_t>(smoke_width) * smoke_height, 0.18F);
        }
        FrameMetadata metadata{smoke_width, smoke_height, 0, 3'000, {1, 90'000},
                               AVCOL_PRI_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                               AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG,
                               AVCHROMA_LOC_UNSPECIFIED};
        smoke_encoder.send(smoke_writer.pack(neutral, 0, metadata));
        auto packets = smoke_encoder.drain();
        auto tail = smoke_encoder.flush();
        packets.insert(packets.end(),
                       std::make_move_iterator(tail.begin()),
                       std::make_move_iterator(tail.end()));
        smoke_writer.wait();
        if (packets.size() != 1U || packets.front().packet == nullptr ||
            packets.front().packet->size <= 0) {
            throw Error(ErrorCode::encode_failed,
                        "Vulkan backend smoke test did not produce one ProRes packet");
        }
        result.vulkan_backend_available = true;
        result.vulkan_unavailable_reason.clear();
    } catch (const std::exception& error) {
        result.vulkan_unavailable_reason = error.what();
    }
#else
    static_cast<void>(gpu_selector);
    static_cast<void>(width);
    static_cast<void>(height);
    static_cast<void>(pool_size);
    static_cast<void>(chroma_filter);
    static_cast<void>(deterministic_dither);
    static_cast<void>(precision);
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
        if (config.demosaic == DemosaicAlgorithm::rcd) {
            return {VideoBackend::vulkan, false, "Vulkan ProRes backend is available"};
        }
        const auto reason = "the Vulkan RAW pipeline supports only precise RCD demosaic; requested " +
            std::string(to_string(config.demosaic));
        if (config.backend == VideoBackend::vulkan) {
            throw Error(ErrorCode::unsupported_format, reason);
        }
        if (config.fallback == GpuFallback::none) {
            throw Error(ErrorCode::unsupported_format,
                        "automatic backend selection cannot fall back to CPU: " + reason);
        }
        if (!capabilities.cpu_available) {
            throw Error(ErrorCode::encode_failed,
                        "the Vulkan RAW pipeline cannot honor the requested demosaic and the CPU fallback is unavailable: " +
                            reason);
        }
        return {VideoBackend::cpu, true, reason};
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
