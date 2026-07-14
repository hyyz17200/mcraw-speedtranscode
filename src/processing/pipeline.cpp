#include <mcraw/processing/pipeline.hpp>

#include <algorithm>
#include <utility>

#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/demosaic.hpp>

namespace mcraw {

CpuPipeline::CpuPipeline(EffectiveConfig config,
                         std::size_t worker_threads,
                         bool output_target_log)
    : config_(std::move(config)),
      worker_threads_(std::clamp<std::size_t>(worker_threads, 1U, 256U)),
      output_target_log_(output_target_log) {
    config_.validate();
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
    {
        StageTimer timer(timings, "color_solution");
        result.color_solution = build_camera_color_solution(result.metadata);
    }
    if (output_target_log_) {
        StageTimer timer(timings, "camera_to_dwg_di_rgb");
        const auto target_linear = camera_to_dwg(
            camera_rgb, result.color_solution, config_.exposure_offset_stops,
            1.0 / 65535.0);
        const auto sharpened = sharpen_target_linear(
            target_linear, config_.capture_sharpening,
            config_.capture_sharpening_threshold);
        result.target_log = encode_davinci_intermediate(
            sharpened, config_.negative_policy);
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
