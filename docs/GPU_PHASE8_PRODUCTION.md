# GPU Phase 8 Productionization Report

Date: 2026-07-14

## Production behavior

- `config/vulkan-gpu-pipeline.json` is the opt-in production-shaped preset.
  The stable CPU pipeline remains the default.
- `backend=auto` performs a full preflight: selected device creation, a
  full-resolution FFmpeg Vulkan frame context and encoder open, then a small
  application-shader-to-ProRes encode/drain/flush smoke frame. Any failure is
  recorded as the fallback reason before the CPU backend is selected.
- `backend=vulkan` never falls back. A preflight or runtime failure terminates
  conversion, removes the partial output through the existing cleanup path,
  and never switches encoder inside one MOV.
- Vulkan device loss has the stable `device_lost` error category. Worker
  failures cancel both bounded queues, stop accepting frames, and propagate to
  the caller.
- A final MOV is still published only after encoder drain, mux trailer, close,
  and reopen validation. Transfer counters retain their strict meaning:
  `gpu_resident=true`, `direct_frames=N`, `upload_frames=0`, and
  `readback_frames=0`.

## Real-sample result

The complete available 4096x3072 sample contains 240 frames and audio. On the
tested RTX 3060 / NVIDIA 576.02 system, the direct Vulkan path completed in
35.145 seconds wall time (34.524 seconds measured pipeline time):

| Result | Value |
|---|---:|
| Video frames / packets | 240 / 240 |
| Direct / YUV upload / readback frames | 240 / 0 / 0 |
| FFmpeg decode result | clean |
| ProRes profile / pixel format | HQ / yuv422p10le |
| MOV duration | 8.031021 s |
| Audio packets | 377 |
| End A/V delta | 40.288409 ms |
| GPU queue peak | 5 / 10 |
| Packet queue peak | 1 / 16 |
| Backpressure waits | 0 |

The same validation script also passed a 30-frame CPU reference conversion,
30-frame direct Vulkan conversion, full software decode of both files, an
invalid-device `auto` CPU fallback with an explicit reason, and an
invalid-device forced-Vulkan failure that produced no final MOV.

The current full-sample throughput is approximately 6.95 fps. The matched
eight-frame profiling comparison and the reason for the TargetLog producer
optimization are recorded in `GPU_PHASE7_OPTIMIZATION.md`. This is not a 4K30
result: RAW decode, calibration, demosaic, camera color, sharpening, and DI
encoding remain on CPU, and FP32 RGB is still staged to the Vulkan shader.

## Compatibility matrix

Blank claims are intentionally not inferred from FFmpeg compatibility.

| Reader/NLE | Decode | Seek | Color | Duration | Audio sync |
|---|---:|---:|---:|---:|---:|
| ffmpeg/ffprobe | Pass | Pass | Metadata inspected | Pass | Pass |
| DaVinci Resolve | Blocked: scripting disabled | Not tested | Not tested | Not tested | Not tested |
| Adobe Premiere Pro | Not tested | Not tested | Not tested | Not tested | Not tested |
| VLC | Pass | MOV-index playback pass | Not visually tested | Pass | Not monitored |

The current Batch F candidate and exact evidence are recorded in
`GPU_BATCH_F_COMPATIBILITY.md`. NLE claims remain intentionally open.

## Definition-of-Done status

The implementation tasks are complete, but production release approval is not
claimed. The remaining gates from the guide are:

- Resolve, Premiere, and a desktop player compatibility runs;
- actual AMD or Intel hardware coverage and a second NVIDIA driver generation;
- resolution or formal waiver of the pinned FFmpeg ProRes DCT shader's
  GPU-assisted validation race recorded in `GPU_PHASE3_VALIDATION.md`;
- a one-hour real conversion and batch/resource-growth run (the existing
  30,000-frame synthetic direct test covers 1,000 logical seconds, not one
  hour);
- an agreed project performance threshold; current 4096x3072 throughput is
  below real time.

Until those gates close, Vulkan is documented and configured as opt-in while
the CPU backend remains the production default and fallback.
