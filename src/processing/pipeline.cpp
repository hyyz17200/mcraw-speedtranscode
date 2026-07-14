#include <mcraw/processing/pipeline.hpp>

#include <algorithm>
#include <utility>

#include <mcraw/processing/calibration.hpp>
#include <mcraw/processing/demosaic.hpp>

namespace mcraw {

CpuPipeline::CpuPipeline(EffectiveConfig config, std::size_t worker_threads)
    : config_(std::move(config)),
      worker_threads_(std::clamp<std::size_t>(worker_threads, 1U, 256U)) {
    config_.validate();
}

ProcessedFrame CpuPipeline::process(const McrawReader& reader,
                                    std::size_t frame_index,
                                    StageTimings& timings) const {
    ProcessedFrame result;
    result.timestamp_ns = reader.frames().at(frame_index).timestamp_ns;
    CameraRgbF32 camera_rgb;
    {
        RawNormalizedF32 normalized;
        DecodedRawFrame decoded;
        {
            StageTimer timer(timings, "official_raw_decode_and_metadata");
            decoded = reader.load_reference_frame_with_metadata(frame_index);
        }
        result.metadata = std::move(decoded.metadata);
        {
            StageTimer timer(timings, "black_white_calibration");
            normalized = calibrate_raw(decoded.raw, result.metadata, worker_threads_);
        }
        {
            StageTimer timer(timings, "demosaic");
            camera_rgb = demosaic(normalized, config_.demosaic, worker_threads_);
        }
    }
    {
        StageTimer timer(timings, "color_solution");
        result.color_solution = build_camera_color_solution(result.metadata);
    }
    {
        StageTimer timer(timings, "fused_camera_to_dwg_di_yuv422p10");
        result.packed = pack_camera_to_dwg_di_yuv422p10(
            camera_rgb, result.color_solution, config_.exposure_offset_stops,
            config_.negative_policy, di_curve_, config_.chroma_filter,
            config_.deterministic_dither, frame_index, worker_threads_);
    }
    return result;
}

} // namespace mcraw
