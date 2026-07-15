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
            {"entry", pipeline.pipeline_entry},
            {"precision", pipeline.pipeline_precision},
            {"demosaic_location", pipeline.demosaic_location},
            {"color_solution_location", pipeline.color_solution_location},
            {"async_depth", pipeline.async_depth},
            {"used_fallback", pipeline.used_fallback},
            {"fallback_reason", pipeline.fallback_reason},
            {"gpu_resident", pipeline.gpu_resident},
            {"upload_frames", pipeline.upload_frames},
            {"readback_frames", pipeline.readback_frames},
            {"direct_frames", pipeline.direct_frames},
            {"rgb_upload_bytes", pipeline.rgb_upload_bytes},
            {"transfers", {
                {"compressed_input_upload_bytes", pipeline.compressed_input_upload_bytes},
                {"u16_raw_upload_bytes", pipeline.u16_raw_upload_bytes},
                {"fp16_rgb_upload_bytes", pipeline.fp16_rgb_upload_bytes},
                {"fp32_rgb_upload_bytes", pipeline.fp32_rgb_upload_bytes},
                {"target_log_fp32_upload_bytes",
                 pipeline.target_log_fp32_upload_bytes},
                {"camera_rgb_fp32_upload_bytes",
                 pipeline.camera_rgb_fp32_upload_bytes},
                {"control_status_read_bytes",
                 pipeline.control_status_read_bytes},
                {"compressed_packet_download_bytes",
                 pipeline.compressed_packet_download_bytes},
                {"gpu_image_to_image_counted_as_pcie", false}
            }},
            {"video_packets", pipeline.video_packets},
            {"queues", {
                {"gpu_capacity", pipeline.gpu_queue_capacity},
                {"gpu_max_depth", pipeline.gpu_queue_max_depth},
                {"prepared_frame_capacity", pipeline.prepared_frame_queue_capacity},
                {"prepared_frame_max_depth", pipeline.prepared_frame_queue_max_depth},
                {"resident_slot_count", pipeline.resident_slot_count},
                {"packet_capacity", pipeline.packet_queue_capacity},
                {"packet_max_depth", pipeline.packet_queue_max_depth},
                {"backpressure_waits", pipeline.backpressure_waits},
                {"backpressure_wait_ms", pipeline.backpressure_wait_ms},
                {"job_backpressure_waits", pipeline.job_queue_backpressure_waits},
                {"job_backpressure_wait_ms", pipeline.job_queue_backpressure_wait_ms},
                {"packet_backpressure_waits", pipeline.packet_queue_backpressure_waits},
                {"packet_backpressure_wait_ms", pipeline.packet_queue_backpressure_wait_ms},
                {"slot_backpressure_waits", pipeline.slot_backpressure_waits},
                {"slot_backpressure_wait_ms", pipeline.slot_backpressure_wait_ms},
                {"job_latency", {
                    {"samples", pipeline.job_queue_latency_samples},
                    {"total_ms", pipeline.job_queue_latency_total_ms},
                    {"mean_ms", pipeline.job_queue_latency_mean_ms},
                    {"max_ms", pipeline.job_queue_latency_max_ms}
                }}
            }},
            {"scheduling", {
                {"frame_pack", {
                    {"samples", pipeline.frame_pack_samples},
                    {"total_ms", pipeline.frame_pack_total_ms},
                    {"mean_ms", pipeline.frame_pack_mean_ms},
                    {"max_ms", pipeline.frame_pack_max_ms}
                }},
                {"encoder_send", {
                    {"samples", pipeline.encoder_send_samples},
                    {"total_ms", pipeline.encoder_send_total_ms},
                    {"mean_ms", pipeline.encoder_send_mean_ms},
                    {"max_ms", pipeline.encoder_send_max_ms}
                }},
                {"encoder_receive", {
                    {"samples", pipeline.encoder_receive_samples},
                    {"total_ms", pipeline.encoder_receive_total_ms},
                    {"mean_ms", pipeline.encoder_receive_mean_ms},
                    {"max_ms", pipeline.encoder_receive_max_ms}
                }},
                {"frame_allocation", {
                    {"samples", pipeline.frame_allocation_samples},
                    {"total_ms", pipeline.frame_allocation_total_ms},
                    {"mean_ms", pipeline.frame_allocation_mean_ms},
                    {"max_ms", pipeline.frame_allocation_max_ms}
                }},
                {"queue_lock_wait", {
                    {"samples", pipeline.queue_lock_wait_samples},
                    {"total_ms", pipeline.queue_lock_wait_total_ms},
                    {"mean_ms", pipeline.queue_lock_wait_mean_ms},
                    {"max_ms", pipeline.queue_lock_wait_max_ms}
                }},
                {"queue_submit", {
                    {"samples", pipeline.queue_submit_samples},
                    {"total_ms", pipeline.queue_submit_total_ms},
                    {"mean_ms", pipeline.queue_submit_mean_ms},
                    {"max_ms", pipeline.queue_submit_max_ms}
                }}
            }},
            {"mux", {
                {"bytes", pipeline.mux_bytes},
                {"megabytes_per_second", pipeline.mux_megabytes_per_second}
            }},
            {"gpu", {
                {"name", pipeline.gpu_name},
                {"uuid", pipeline.gpu_uuid},
                {"driver", pipeline.gpu_driver},
                {"timestamps_supported", pipeline.gpu_timestamps_supported},
                {"stages", {
                    {"camera_to_dwg", {
                        {"samples", pipeline.camera_to_dwg_gpu_timestamp_samples},
                        {"total_ms", pipeline.camera_to_dwg_gpu_total_ms},
                        {"mean_ms", pipeline.camera_to_dwg_gpu_mean_ms},
                        {"p50_ms", pipeline.camera_to_dwg_gpu_p50_ms},
                        {"p95_ms", pipeline.camera_to_dwg_gpu_p95_ms},
                        {"p99_ms", pipeline.camera_to_dwg_gpu_p99_ms},
                        {"min_ms", pipeline.camera_to_dwg_gpu_min_ms},
                        {"max_ms", pipeline.camera_to_dwg_gpu_max_ms}
                    }},
                    {"capture_sharpening", {
                        {"samples", pipeline.capture_sharpening_gpu_timestamp_samples},
                        {"total_ms", pipeline.capture_sharpening_gpu_total_ms},
                        {"mean_ms", pipeline.capture_sharpening_gpu_mean_ms},
                        {"p50_ms", pipeline.capture_sharpening_gpu_p50_ms},
                        {"p95_ms", pipeline.capture_sharpening_gpu_p95_ms},
                        {"p99_ms", pipeline.capture_sharpening_gpu_p99_ms},
                        {"min_ms", pipeline.capture_sharpening_gpu_min_ms},
                        {"max_ms", pipeline.capture_sharpening_gpu_max_ms}
                    }},
                    {"davinci_intermediate", {
                        {"samples", pipeline.davinci_intermediate_gpu_timestamp_samples},
                        {"total_ms", pipeline.davinci_intermediate_gpu_total_ms},
                        {"mean_ms", pipeline.davinci_intermediate_gpu_mean_ms},
                        {"p50_ms", pipeline.davinci_intermediate_gpu_p50_ms},
                        {"p95_ms", pipeline.davinci_intermediate_gpu_p95_ms},
                        {"p99_ms", pipeline.davinci_intermediate_gpu_p99_ms},
                        {"min_ms", pipeline.davinci_intermediate_gpu_min_ms},
                        {"max_ms", pipeline.davinci_intermediate_gpu_max_ms}
                    }},
                    {"rgb_to_yuv_422", {
                        {"samples", pipeline.rgb_to_yuv_gpu_timestamp_samples},
                        {"total_ms", pipeline.rgb_to_yuv_gpu_total_ms},
                        {"mean_ms", pipeline.rgb_to_yuv_gpu_mean_ms},
                        {"p50_ms", pipeline.rgb_to_yuv_gpu_p50_ms},
                        {"p95_ms", pipeline.rgb_to_yuv_gpu_p95_ms},
                        {"p99_ms", pipeline.rgb_to_yuv_gpu_p99_ms},
                        {"min_ms", pipeline.rgb_to_yuv_gpu_min_ms},
                        {"max_ms", pipeline.rgb_to_yuv_gpu_max_ms}
                    }}
                }},
                {"control_status_failures", pipeline.control_status_failures}
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
