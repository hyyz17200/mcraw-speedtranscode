#include <mcraw/processing/pipeline.hpp>

#include <algorithm>
#include <utility>

#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/demosaic.hpp>

namespace mcraw {

CpuPipeline::CpuPipeline(EffectiveConfig config,
                         std::size_t worker_threads,
                         CpuPipelineOutput output)
    : config_(std::move(config)),
      worker_threads_(std::clamp<std::size_t>(worker_threads, 1U, 256U)),
      output_(output) {
    config_.validate();
}

std::string_view to_string(CpuPipelineOutput value) noexcept {
    switch (value) {
    case CpuPipelineOutput::packed_yuv: return "packed_yuv";
    case CpuPipelineOutput::target_log_rgb: return "target_log_rgb";
    case CpuPipelineOutput::camera_rgb: return "camera_rgb";
    case CpuPipelineOutput::raw_mosaic: return "raw_mosaic";
    }
    return "unknown";
}

ProcessedFrame CpuPipeline::process(const McrawReader& reader,
                                    std::size_t frame_index,
                                    StageTimings& timings) const {
    ProcessedFrame result;
    result.timestamp_ns = reader.frames().at(frame_index).timestamp_ns;
    CameraRgbF32 camera_rgb;
    {
        RawDemosaicF32 calibrated;
        DecodedRawFrame decoded;
        {
            StageTimer timer(timings, "official_raw_decode_and_metadata");
            decoded = reader.load_reference_frame_with_metadata(frame_index);
        }
        result.metadata = std::move(decoded.metadata);
        if (output_ == CpuPipelineOutput::raw_mosaic) {
            // Stage 2 keeps official decode and metadata on the CPU, but moves
            // calibration and demosaic behind the Vulkan writer boundary.
            result.raw_mosaic = std::move(decoded.raw);
        } else {
            {
                StageTimer timer(timings, "black_white_calibration");
                calibrated = calibrate_raw_for_demosaic(
                    decoded.raw, result.metadata, worker_threads_);
            }
            {
                StageTimer timer(timings, "demosaic");
                // Planes stay in the 0..65535 librtprocess domain; the 1/65535
                // scale folds into the per-frame matrix inside the fused pack.
                camera_rgb = demosaic_unnormalized(calibrated, config_.demosaic,
                                                   worker_threads_);
            }
        }
    }
    {
        StageTimer timer(timings, "color_solution");
        result.color_solution = build_camera_color_solution(result.metadata);
    }
    if (output_ == CpuPipelineOutput::raw_mosaic) {
        return result;
    } else if (output_ == CpuPipelineOutput::camera_rgb) {
        result.camera_rgb = std::move(camera_rgb);
    } else if (output_ == CpuPipelineOutput::target_log_rgb) {
        StageTimer timer(timings, "camera_to_dwg_di_rgb");
        auto target_linear = camera_to_dwg(
            camera_rgb, result.color_solution, config_.exposure_offset_stops,
            1.0 / 65535.0, worker_threads_);
        target_linear = sharpen_target_linear(
            std::move(target_linear), config_.capture_sharpening,
            config_.capture_sharpening_threshold, worker_threads_);
        result.target_log = encode_davinci_intermediate_lut(
            std::move(target_linear), config_.negative_policy,
            di_curve_, worker_threads_);
    } else {
        StageTimer timer(timings, "fused_camera_to_dwg_di_yuv422p10");
        result.packed = pack_camera_to_dwg_di_yuv422p10(
            camera_rgb, result.color_solution, config_.exposure_offset_stops,
            config_.negative_policy, di_curve_, config_.chroma_filter,
            config_.deterministic_dither, frame_index, worker_threads_,
            config_.capture_sharpening, config_.capture_sharpening_threshold,
            1.0 / 65535.0);
    }
    return result;
}

} // namespace mcraw
