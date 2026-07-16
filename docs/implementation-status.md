# v0.1 Implementation Status

## GPU Stage 3F / Batch D accepted

- Published `config/vulkan-precise.json` and `vulkan-fast.json`; the CPU backend
  remains default. The former balanced preset was removed after Batch D.
- Final public-preset medians are 34.776 and 36.857 fps. Fast reduces sampled
  VRAM delta by about 288 MiB versus precise.
- Both public modes retain U16-only upload, zero pixel/YUV readback, precise RCD, quality
  chroma, and deterministic dither. See `GPU_STAGE3F_E2E_BENCHMARK.md`.

## GPU Stage 3B FP16 storage experiment

- `gpu_performance_mode` provides stable precise/fast identities and
  sidecars report actual intermediate storage, DI, dither, and demosaic choices.
- The former balanced experiment stores post-RCD color/sharpen/DI planes as packed FP16 while
  preserving FP32 computation, precise RCD, and FP32 final quantization.
- Real frames 0/120/239 remain at max 1 LSB with RMSE below 0.202 LSB.
- Matched median throughput improved from 34.901 to 35.577 fps (+1.938%); this
  accepted storage path is retained by fast.
- See `GPU_STAGE3_PERFORMANCE_MODES_TECHNICAL_DESIGN.md` and
  `GPU_STAGE3B_FP16_STORAGE_VALIDATION.md`.

## GPU Stage 3C fast analytic DI

- Fast mode retains the accepted FP16 storage path and evaluates the DI curve
  analytically in FP32; the former balanced LUT comparison is historical only.
- Fixed real frames remain max/P99 1 LSB and RMSE below 0.202 LSB.
- Median throughput improved from 35.577 to 36.770 fps (+3.353%); DI stage mean
  fell from 0.907 to 0.497 ms.
- See `GPU_STAGE3C_ANALYTIC_DI_VALIDATION.md`.

## GPU Stage 3D dither decision

- Disabling deterministic dither reduced isolated YUV time but improved median
  end-to-end throughput by only 0.063%, inside run noise.
- The experiment is NO-GO; every public performance mode retains deterministic
  dither. See `GPU_STAGE3D_DITHER_EXPERIMENT.md`.

## GPU Stage 3E demosaic decision

- A separately named bilinear prototype exceeded the fast quality budgets by
  wide margins (up to 473 LSB), so it was removed before merge.
- Every public GPU performance mode retains precise RCD. See
  `GPU_STAGE3E_FAST_DEMOSAIC_EXPERIMENT.md`.

## Implemented source boundary

- CMake/vcpkg/MSVC 2022 build skeleton
- Independent MCRAW index and compressed-payload reader
- MotionCam official CPU decompression adapter
- Metadata normalization and source-visible warnings
- RAW black/white-level handling and RCD/AMaZE/IGV/DCB/LMMSE; RCD remains the default
- Linear DWG Capture Sharpening with default amount `0.4`; the transcoder does not denoise
- Dual-matrix color handling, ForwardMatrix, Bradford, DWG, and DaVinci Intermediate
- Per-conversion DI LUT, fused Camera→DWG→DI→YCbCr processing, quality 4:2:2, dither, and legal-range quantization
- OpenMP row parallelism, CPU/RAM-aware bounded multi-frame parallelism, and ordered muxing
- FFmpeg ProRes/MOV/PCM/timestamps and sidecar output
- Seven CLI subcommands and unit tests

## Windows 10 / MSVC 2022 acceptance results

- MSVC 2022 Release: main library, CLI, and test programs build successfully
- Unit tests: 18/18 passed
- `mcraw_sample/`: one sample completed `inspect`, first/last-frame RAW decompression, and hashing
- Sample audio: 48 kHz, stereo, all 377 packets and source timestamps read successfully
- CPU compute benchmark: about 3.7–4.0 fps with automatic 16 threads / 8 in-flight frames; average whole-system CPU utilization about 78.6%
- Compute throughput increased about 15× over the original reference implementation at about 0.26 fps
- The first and last 4K frames each compared 25,165,824 10-bit samples; maximum difference on the fused path was 1 LSB, with difference ratios of about 0.00056% and 0.00051%
- Two-frame ProRes 422 HQ + PCM MOV round trip passed
- The optimized full 240-frame ProRes 422 HQ + PCM MOV conversion and full-stream FFmpeg decode passed: 95.251 s, 2.52 fps; the original implementation took 931.2 s, for about 9.8× end-to-end acceleration
- ffprobe: `apch`, `yuv422p10le`, decoded frames in video range, BT.2020 NCL, 90 kHz video time base, and 48 kHz PCM; 240-frame video duration 7.990744 s and audio duration 8.031021 s
- Optimized full output: 1,402,859,919 bytes, with no leftover partial file; the sidecar records 240 frames, 377 audio packets, and audio ending +40.288409 ms relative to the video end
- The complete PCM SHA-256 before and after optimization was
  `04c5eb506259a5eb2f956226ce627cc4b3e773faf8c9d67d645fed5d8468736a`

## 2026-07-14 quality-option comparison

Compute-only measurement using the same 4096×3072 sample, 16 frames, Release
build, 16 CPU threads, 8 in-flight frames, and 2 threads per frame:

| Configuration | Throughput (fps) | Throughput relative to RCD | Mean demosaic time |
|---|---:|---:|---:|
| RCD (mean of two runs) | 4.477 | Baseline | 405.0 ms |
| AMaZE | 3.097 | -30.8% | 1219.0 ms |
| DCB (mean of two runs, 2 iterations, enhance disabled) | 3.493 | -22.0% | 835.2 ms |
| LMMSE (mean of two runs, 2 iterations) | 3.086 | -31.1% | 1159.2 ms |

Enabling Capture Sharpening `0.25` on RCD alone produced 3.955 fps, about
11.1% below RCD in the same run. Stage timings overlap under multi-frame
parallelism, so throughput difference is the main measure of total cost.

The complete 240-frame comparison outputs both passed full-stream FFmpeg decode:
AMaZE reached 2.217 fps, while RCD + Capture Sharpening `0.25` reached 2.655
fps. Both were 4096×3072 ProRes 422 HQ `yuv422p10le` files retaining 48 kHz
stereo PCM.

## Still requiring manual external acceptance

- DaVinci Resolve chart, chroma siting, and the manual Input Color Space workflow
- Manual playback and image inspection of the complete 240-frame output (automatic decode and stream-parameter checks have passed)

## 2026-07-14 GPU next-stage Stage 0

 - Formally adopted an action target of 24 fps minimum, 30 fps extended, and a maximum 1 LSB difference for final `fp32/precise` YUV.
 - Established a fixed corpus contract: first, middle, and last frames of a real 4096×3072 / 240-frame compression 7 sample, plus definitions for four CFA patterns, calibration ranges, saturated colors, and high-frequency patterns.
 - Added calibrated mosaic, Camera RGB, sharpened TargetLinear, TargetLog, and YUV extract stages at the same boundaries as the production `CpuPipeline`.
 - Vulkan RGB→YUV now uses timestamp queries; CPU fence wall time is no longer presented as GPU execution time. Sidecar/CLI output includes sample count and total/mean/P50/P95/P99/min/max.
 - All 21 artifact hashes from the initial repeated capture were stable.
 - The initial forced-Vulkan full benchmark completed one warm-up and three official runs: median 7.133 fps, range 7.098–7.412 fps; 240 GPU timestamps per run; RGB→YUV GPU mean run median 13.007 ms/frame; device-global mean GPU utilization about 18.3%; job queue peak 7, packet queue peak 1, and backpressure 0.
 - Release build, 43 CTest cases, GPU-assisted RGB→YUV golden test, 30-frame CPU/Vulkan full-stream decode, automatic fallback, and forced-Vulkan cleanup all passed.

Stage 0 was committed as rollback point `622070c`. The post-commit capture
manifest points to that commit, records `dirty=false`, and retains the same
executable/config/corpus and 21 artifact hashes. The Stage 1 Camera RGB input,
shader passes, uniforms, golden tests, synchronization, and benchmark contract are defined in
`GPU_STAGE1_CAMERA_RGB_TECHNICAL_DESIGN.md`。

## 2026-07-15 GPU Stage 1A foundation

- `CpuPipelineOutput` replaced the implicit boolean producer seam and clearly separates CPU packed YUV, TargetLog RGB, and Camera RGB; the CLI still uses the accepted Stage 0 TargetLog path.
- Added independent `VulkanCameraPipelineResources`: each slot has three host-visible Camera RGB upload buffers, two sets of three-plane device-local FP32 ping-pong buffers, an independent command buffer, and a fence. It continues to reuse the FFmpeg-owned device, compute queue, and queue lock.
- Test-only readback must be explicitly enabled. A transfer barrier verifies bit-for-bit consistency across upload → device-local intermediate → readback; production configurations allocate no readback resources.
- Writer telemetry/sidecar output now records the actual `pipeline.entry`, precision, and demosaic/color-solution location, and separately counts TargetLog and Camera RGB FP32 upload bytes.
- This batch added no color, sharpening, or DI shader and does not change Stage 0 output pixels or fallback behavior.

## 2026-07-15 GPU Stage 1B color/exposure

- Added an independent `camera_to_dwg.comp.glsl` FP32 pass. The CPU continues to calculate the FP64 color solution; the GPU receives only the final 3×3 row-major matrix and an independent exposure scale.
- The 64-byte push-constant ABI is frozen by C++ size/offset assertions. Each slot uses a six-binding descriptor set with three Camera input planes and three ping-A TargetLinear output planes.
- Test-only readback uses explicit compute→transfer→host barriers; production still allocates no readback buffer.
- Synthetic golden max/RMSE were `2.38419e-7 / 1.87853e-8`; the real 4096×3072 Stage 0 first frame was `2.38419e-7 / 1.57234e-8`. Both passed the `2e-5 / 1e-6` gates.
- The color pass has an independent GPU timestamp summary; one validation-enabled 4K sample measured `11.9165 ms`, for shader-validation data only and not a performance commitment.
- This batch still does not switch the production writer; see
  `GPU_STAGE1B_COLOR_VALIDATION.md`。

## 2026-07-15 GPU Stage 1C sharpening

- Added an independent `sharpen_target_linear.comp.glsl` FP32 pass. It reads device-local ping A and writes ping B, with no in-place update, clamp, FP16, or pass fusion.
- It preserves the CPU semantics for BT.2020 luma, four-neighbor cross blur, edge-coordinate clamping, soft threshold, and equal RGB delta; the 16-byte push-constant ABI is frozen.
- Color→sharpen uses a compute write/read barrier. Only the test route adds a barrier from ping B to transfer readback; production still performs no intermediate-image readback.
- Synthetic edge/threshold/negative golden max/RMSE were `5.96046e-8 / 2.00631e-9`; the real 4096×3072 Stage 0 first frame was `2.38419e-7 / 1.94043e-8`. Both passed the `3e-5 / 2e-6` gates.
- The sharpening pass has an independent GPU timestamp summary; one validation-enabled 4K sample measured `4.3264 ms`, for shader-validation data only and not a performance commitment.
- This batch still does not switch the production writer; see
  `GPU_STAGE1C_SHARPENING_VALIDATION.md`。

## 2026-07-15 GPU Stage 1D DaVinci Intermediate

- Added an independent `davinci_intermediate.comp.glsl` FP32 precise pass. It reads sharpened ping B and, after an explicit barrier, reuses ping A to write TargetLog.
- The two existing 65,536-entry FP32 LUTs are uploaded once through staging to a 524,288-byte pipeline-owned device-local buffer shared by all slots; values above 100 continue to use analytic DI.
- The `preserve_by_curve`, `clamp_zero`, and `error` policy ABI is frozen. Each slot has an independent 4-byte status word that detects error-policy negatives and shader-created non-finite values; it is read only after the fence signals and terminates with `processing_failed`.
- LUT-boundary synthetic golden max/RMSE were `1.19209e-7 / 2.98023e-8`; the real 4096×3072 Stage 0 first frame was `5.96046e-8 / 1.17004e-8`. Both passed the `3e-5 / 2e-6` gates; 1×1, repeated-determinism, and two fault-injection tests also passed.
- The DI pass has an independent GPU timestamp summary; one validation-enabled 4K sample measured `3.48774 ms`, for shader-validation data only and not a performance commitment.
- This batch still does not switch the production writer; see
  `GPU_STAGE1D_DAVINCI_INTERMEDIATE_VALIDATION.md`。

## 2026-07-15 GPU Stage 1E resident chain

- The production Vulkan route now uses `camera_rgb_f32`: one slot command buffer chains color/exposure, sharpening, DI, and RGB-to-YUV, then writes directly to the encoder-owned `AVVkFrame`; the Stage 0 TargetLog overload remains only as a rollback point.
- Camera RGB upload is counted exactly per frame; production TargetLog upload and TargetLinear/TargetLog/YUV pixel readback are all zero. Each frame reads only an additional 4-byte control status.
- Ping-pong buffer reuse, status reset/read, `AVVkFrame` layout/access, and the timeline semaphore all use explicit barriers/ownership. The broad source stage found by validation was narrowed to the actual compute shader stage, and retesting found zero application validation errors.
- Synthetic resident final Y/Cb/Cr all had 0 LSB error; negative-policy fault injection, decodable MOV output, partial cleanup, automatic fallback, and forced invalid-device tests all passed.
- In a 30-frame 4096×3072 E2E run, Camera RGB upload was 4,529,848,320 bytes, TargetLog upload was 0, and each of the four GPU passes produced 30 timestamps. CPU/Vulkan full-stream decode and audio/PTS checks passed. See `GPU_STAGE1E_RESIDENT_CHAIN_VALIDATION.md`.

## 2026-07-15 GPU Stage 1F E2E and benchmark

- Final Y/Cb/Cr for real Stage 0 frames 0/120/239 all passed with a maximum 1 LSB difference. The 240-frame Stage 1 MOV passed ProRes HQ, `yuv422p10le`, full-stream video decode, 48 kHz stereo audio, PTS, and transfer/status/timestamp invariant checks.
- A matched A/B on the same RTX 3060 with the same input/configuration, one warm-up, and three official runs per candidate produced a reconstructed Stage 0 median of 6.863 fps and a Stage 1 median of 4.963 fps. Relative performance was `-27.685%`, below the `+20%` gate; the decision was **NO-GO**.
- Stage 1 GPU job-queue peak was 10, backpressure waits were 227 per run, median ProRes submit/wait mean was 181.777 ms/frame, and median VRAM delta was 3,782 MiB. Stage 0 values were 5, 0, 0.004 ms/frame, and 1,433 MiB. The evidence points to resident slots/VRAM/barriers/occupancy and encoder contention, not CPU demosaic.
- Stage 1A–1F are complete as independent rollback points, but Stage 1 did not pass performance acceptance. Vulkan remains opt-in; do not directly fuse shaders or enter Stage 2. See
  `GPU_STAGE1F_E2E_BENCHMARK.md`。

## 2026-07-15 GPU Stage 1G performance recovery

- New telemetry separates job/slot/packet backpressure, frame packing, encoder send/receive, frame allocation, and queue lock/submit. It shows that Stage 1F `prores_submit_wait` was mainly queue-level visibility; slots, the frame pool, queue lock, packets, and muxing were not primary bottlenecks.
- Vulkan frame preparation and ProRes submission now run in parallel as two bounded workers. The per-frame CPU finite-value scan of 151 MB of Camera RGB before upload was removed; existing DI control status now fails non-finite values before publication, and a production E2E fault test was added.
- ProRes `async_depth=8` is decoupled from the resident preparation ring. The production path retains encoder depth eight but allocates only two expensive FP32 resident slots. Prepared-queue peak was 2, and all backpressure values were 0 across three runs.
- Clean `042e179` on the same RTX 3060 with the same input/configuration, one warm-up, and three official runs produced a median of `13.791 fps` (13.429–13.873), `+100.943%` versus reconstructed Stage 0 at `6.863 fps`, and `+177.871%` versus the old Stage 1 at `4.963 fps`; it passed the Stage 1 `+20%` gate.
- Median VRAM delta fell from 3,782 MiB to 2,032 MiB; the final MOV hash was exactly the same as Stage 1F. The default Release suite was 58 passed / 5 opt-in skipped out of 63 tests; maximum Y/Cb/Cr error for real frames 0/120/239 was 1 LSB. See `GPU_STAGE1G_PERFORMANCE_RECOVERY.md`.

## 2026-07-15 GPU Stage 2A technical contract

- Batch C is frozen as five independent rollback points: technical/API/resources, calibration, precise RCD, the production resident chain, and E2E/benchmark.
- The Stage 2 production boundary moves only from Camera RGB FP32 back to U16 RAW. Official decode, the CPU FP64 color solution, Stage 1, ProRes, audio, and release semantics remain unchanged.
- Calibration preserves negative values and super-white. The subsequent RCD precisely matches librtprocess's existing `LIM01(calibrated / 65536)` input boundary; the two are not combined into a new early clamp.
- The input/output, ownership, synchronization, golden, ≤1 LSB final-quality, transfer/timestamp telemetry, failure/fallback, and matched-benchmark contracts are defined in `GPU_STAGE2_U16_RAW_TECHNICAL_DESIGN.md`.

## 2026-07-15 GPU Stage 2B calibration

- Added a packed U16 CFA calibration compute pass. Each 32-bit storage word unpacks two U16 values; shader 16-bit storage capability is not required, and input size remains exactly `width * height * 2` bytes.
- A fixed 48-byte push ABI carries dimensions and four black/white groups. Output is device-local FP32 CFA; negative values and super-white are not clamped, and test readback is available only when explicit test resources are enabled.
- For 64×32 synthetic inputs, four CFA patterns, and fractional black/white values, CPU-reference maximum absolute error was `0.0078125` and RMSE was `0.00137959`. The calibration intermediate gates were therefore frozen at `0.01/0.002`; final YUV `≤1 LSB` remains unchanged.
- Added `raw_calibration` GPU timestamp summary. Production remains at Stage 1, with no change to upload, sidecar, or release paths. See `GPU_STAGE2B_CALIBRATION_VALIDATION.md` for evidence.

## 2026-07-15 GPU Stage 2C precise RCD prototype

- Following the librtprocess 0.12.0 dependency structure, the prototype uses eight compute dispatches covering initialization, VH/LPF, G, PQ, opposite-color, green-position color, and the 9-pixel border. Five slot-owned scratch planes retain packed-half indexing, with explicit compute barriers between passes.
- Against CPU RCD on 64×64 synthetic inputs with four CFA patterns: max `0.009765625`, border max `0.00390625`, and RMSE `0.00129745`, passing the tightened `0.02/0.005` gates.
- Real 4096×3072 frame 0: among 37,748,736 channel samples, P99 was `0.00390625`, RMSE `0.0363643`, 274 samples were >2, and maximum `136.61328125` occurred at R/x=2485/y=59. It was identified as a stable FP32 near-equal branch in directional classification, not a structural misalignment.
- The technical contract jointly constrains this outlier behavior using max/RMSE/P99/outlier count. Final YUV `≤1 LSB` is not relaxed; if Stage 2D/E cannot meet it, production remains at Stage 1. See
  `GPU_STAGE2C_RCD_VALIDATION.md`。

## 2026-07-15 GPU Stage 2D resident RAW production chain

- The production Vulkan input changed from CPU Camera RGB to decoded U16 CFA plus normalized metadata. Calibration, eight-pass precise RCD, and the Stage 1 color/sharpen/DI/YUV chain run in one resident command buffer and deliver directly to an FFmpeg `AVVkFrame`.
- Each slot owns its U16 upload, calibrated CFA, three Camera RGB planes, and five RCD scratch planes; the production interface exposes no calibrated/RGB readback.
- Telemetry now identifies `raw_mosaic_u16` / `gpu_rcd_precise`, counts U16 upload precisely, and adds `raw_calibration` and `rcd_demosaic` GPU timestamps. Tests confirmed that FP32 RGB upload and YUV readback are both zero.
- Vulkan production accepts precise RCD only: `auto` falls back to CPU before creating output for other demosaic modes, while forced Vulkan rejects them explicitly. Per-frame silent switching is prohibited.
- Validation-layer RAW E2E (33 assertions), backend selection (4 assertions), and Release CTest (71/71) all passed (6 existing real-sample opt-in tests skipped). See
  `GPU_STAGE2D_RESIDENT_CHAIN_VALIDATION.md`。

## 2026-07-15 GPU Stage 2E acceptance and Batch C closure

- Final Y/Cb/Cr maximum error for real sample frames 0/120/239 was `1 LSB` in all cases. Both normal and Vulkan validation-layer runs passed; the Stage 2 real test had 20 assertions.
- Clean `867c0b1` completed a full 240-frame warm-up plus three official runs with a median of `37.747 fps` (37.530–37.935), a `173.710%` improvement over accepted Stage 1G, exceeding both the 24 fps minimum and 30 fps extended targets.
- Each official run uploaded exactly 6,039,797,760 U16 bytes, with 0 FP32 RGB upload and 0 YUV readback; all six GPU stages produced 240 timestamps, with 960 bytes of status reads and 0 failures.
- The final MOV contains 240 ProRes HQ `yuv422p10le` frames plus 377 PCM stereo packets. Full FFmpeg decode completed without errors, and A/V offsets matched Stage 1.
- Release CTest passed 72/72 (7 real/high-memory opt-in tests skipped). Batch C is formally **GO**. See `GPU_STAGE2E_E2E_BENCHMARK.md` for detailed evidence; the default backend remains CPU, while release gates and the fast preset belong to later Batches D/F.
