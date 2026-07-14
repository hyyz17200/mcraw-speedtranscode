#include <mcraw/output/sidecar.hpp>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

nlohmann::json matrix_json(const Matrix3d& matrix) {
    return matrix.v;
}

} // namespace

void write_sidecar(const std::filesystem::path& path,
                   const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   const EffectiveConfig& config,
                   const NormalizedCameraMetadata& first_frame_metadata,
                   const CameraColorSolution& color_solution,
                   const StageTimings& timings,
                   std::size_t frames_written,
                   const AvSyncReport& av_sync,
                   const PipelineBackendReport& pipeline,
                   const std::vector<std::string>& runtime_warnings) {
    if (pipeline.gpu_resident &&
        (pipeline.upload_frames != 0U || pipeline.readback_frames != 0U)) {
        throw Error(ErrorCode::invalid_argument,
                    "GPU-resident telemetry cannot report CPU upload or readback frames");
    }
    nlohmann::json document = {
        {"schema", "mcraw-transcoder-sidecar-v1"},
        {"application", {{"name", "mcraw-transcoder"}, {"version", "0.1.0"}}},
        {"input", input.string()},
        {"output", output.string()},
        {"frames_written", frames_written},
        {"effective_config", config_to_json(config)},
        {"first_frame_metadata", metadata_to_json(first_frame_metadata)},
        {"color", {
            {"profile", "DaVinciIntermediate_DWG"},
            {"white_point_xy", {color_solution.white_point.x, color_solution.white_point.y}},
            {"white_point_cct", color_solution.white_point.cct},
            {"matrix_weight", color_solution.white_point.interpolation_weight},
            {"neutral_iterations", color_solution.white_point.iterations},
            {"used_forward_matrix", color_solution.used_forward_matrix},
            {"camera_to_xyz_d50", matrix_json(color_solution.camera_to_xyz_d50)},
            {"xyz_d50_to_dwg", matrix_json(color_solution.xyz_d50_to_target)},
            {"camera_to_dwg", matrix_json(color_solution.camera_to_target)}
        }},
        {"mov_color_metadata", {
            {"range", "video"},
            {"matrix", "bt2020_ncl"},
            {"primaries", "unspecified"},
            {"transfer", "unspecified"},
            {"manual_input_color_space", "DaVinci Wide Gamut / DaVinci Intermediate"}
        }},
        {"timings", timings.to_json()},
        {"av_sync", {
            {"audio_present", av_sync.audio_present},
            {"audio_chunks", av_sync.audio_chunks},
            {"audio_minus_video_start_ms", av_sync.start_delta_ms},
            {"audio_minus_video_end_ms", av_sync.end_delta_ms}
        }},
        {"pipeline", {
            {"requested_backend", pipeline.requested_backend},
            {"backend", pipeline.backend},
            {"async_depth", pipeline.async_depth},
            {"used_fallback", pipeline.used_fallback},
            {"fallback_reason", pipeline.fallback_reason},
            {"gpu_resident", pipeline.gpu_resident},
            {"upload_frames", pipeline.upload_frames},
            {"readback_frames", pipeline.readback_frames},
            {"video_packets", pipeline.video_packets},
            {"gpu", {
                {"name", pipeline.gpu_name},
                {"uuid", pipeline.gpu_uuid},
                {"driver", pipeline.gpu_driver}
            }},
            {"ffmpeg", {
                {"version", pipeline.ffmpeg_version},
                {"configuration", pipeline.ffmpeg_configuration}
            }}
        }},
        {"warnings", runtime_warnings}
    };

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw Error(ErrorCode::io_failed, "cannot create sidecar: " + path.string());
    stream << document.dump(2) << '\n';
    if (!stream) throw Error(ErrorCode::io_failed, "failed while writing sidecar: " + path.string());
}

} // namespace mcraw
