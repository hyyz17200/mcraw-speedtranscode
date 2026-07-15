# GPU Stage 1 Camera RGB Technical Design

Date: 2026-07-15
Status: implementation contract; no Stage 1 pixel implementation is included in this document

This document freezes the first post-Stage-0 pipeline move described by
`GPU_PIPELINE_FORMAL_ACTION_GUIDE.md`. If an implementation needs to change a
boundary below, update this document or record an ADR before changing code.

## 1. Scope and completion definition

The only production boundary moved by Stage 1 is:

```text
before: CPU TargetLog RGB FP32 -> Vulkan YUV/ProRes
after:  CPU Camera RGB FP32    -> Vulkan color/sharpen/DI/YUV/ProRes
```

CPU official decode, black/white calibration, librtprocess demosaic, metadata
normalization, `CameraNeutral` solving, dual-illuminant interpolation,
ForwardMatrix/Bradford selection, audio, timestamp generation, muxing and file
publication remain unchanged.

Stage 1 is complete only when the FP32 precise path, stage golden tests, full
regression suite, fault tests, sidecar contract and matched full-sample
benchmark are all complete. A short smoke run or a correct color shader alone
is not Stage 1 completion.

### Non-goals

- no FP16, mixed precision, approximate DI, alternate demosaic or fast preset;
- no pass fusion on first implementation;
- no U16 RAW upload, GPU calibration or GPU RCD;
- no GPU MCRAW decompression;
- no change to CPU reference arithmetic, metadata, chroma siting, quantization,
  deterministic dither, fallback selection or publication gates;
- no runtime CPU/Vulkan encoder switch inside one MOV.

## 2. Frozen CPU reference and input contract

The authoritative production sequence is the current `CpuPipeline` split path:

1. `demosaic_unnormalized()` produces `CameraRgbF32`;
2. `build_camera_color_solution()` performs FP64 setup;
3. `camera_to_dwg(..., exposure_offset_stops, 1.0 / 65535.0)`;
4. `sharpen_target_linear()`;
5. `encode_davinci_intermediate_lut()`;
6. `pack_dwg_log_to_yuv422p10()`.

The Stage 1 input is the existing `CameraRgbF32`/`PlanarRgbF32` ABI:

| Field | Frozen meaning |
|---|---|
| dimensions | non-zero `uint32`, width even for the downstream 4:2:2 writer |
| layout | three separate contiguous planes in R, G, B order |
| element | IEEE-754 binary32 (`float`) |
| plane length | exactly `width * height` |
| sample domain | unnormalized librtprocess working scale, nominally 0..65535 |
| row order | top-to-bottom, `index = y * width + x` |
| exceptional values | shape is checked by `PlanarRgbF32::validate()`; Stage 1 production must add an explicit finite-value check before upload |

Negative and super-white values must not be clipped at the Camera RGB or color
pass boundary. The only normalization is the existing `1/65535` factor applied
with exposure during Camera RGB to DWG conversion.

`CameraColorSolution` remains CPU-owned. FP64 neutral solving, matrix inversion,
ForwardMatrix/Bradford work and exposure setup are never moved into a shader in
Stage 1. Per frame, the GPU receives only the final row-major 3x3
`camera_to_target` matrix and validated policy/scalar parameters.

## 3. Production API seam

Replace the current boolean `output_target_log` seam with an explicit producer
boundary (CPU YUV, TargetLog RGB, or Camera RGB). A Vulkan Stage 1
`ProcessedFrame` carries:

- `CameraRgbF32 camera_rgb`;
- the CPU-computed `CameraColorSolution` or a narrower validated Stage 1
  parameter object derived from it;
- source timestamp and frame index.

Add a distinct writer overload/job alternative for Camera RGB. Do not infer
the input color space from the `PlanarRgbF32` alias: the job variant must encode
the semantic type explicitly. The old TargetLog Vulkan entry stays available
as the Stage 0 rollback/reference path until Stage 1 is accepted.

The Stage 1 implementation should extend or narrowly generalize the current
`VulkanRgbToYuvFrameWriter`. It must continue using the exact FFmpeg-owned
logical device, frame pool and compute queue already used by
`prores_ks_vulkan`; a second Vulkan device or an application-owned output frame
pool is forbidden.

## 4. Shader and resource design

The first correct implementation uses four separately timestamped logical
passes in one slot-owned command buffer:

```text
host-visible planar Camera RGB buffers
  -> camera_to_dwg_fp32
  -> target-linear ping buffer A
  -> sharpen_target_linear_fp32
  -> target-linear ping buffer B
  -> davinci_intermediate_fp32
  -> TargetLog buffer A (A is safe to reuse here)
  -> existing rgb_to_yuv_422
  -> FFmpeg AVVkFrame storage images
```

Each slot owns three Camera RGB upload buffers and two sets of three
device-local FP32 storage buffers. The DI LUT is immutable, pipeline-owned and
shared by slots. Reusing buffer A for DI output is allowed only after an
explicit shader-write-to-shader-write/read barrier and after the color output
is no longer needed. Production has no intermediate image readback.

### 4.1 Color/exposure pass

One invocation processes one pixel. It loads planar Camera RGB, evaluates the
row-major 3x3 matrix in the same documented channel order, applies
`exp2(exposure_offset_stops) * (1/65535)`, checks for a non-finite result and
writes planar TargetLinear FP32.

The CPU uploads matrix and exposure/scale separately; first implementation must
not pre-fuse them or change operation order merely to save instructions.

### 4.2 Capture-sharpening pass

The shader freezes the current CPU semantics:

- BT.2020 luma coefficients `kr=0.2627`, `kb=0.0593`, `kg=1-kr-kb`;
- four-neighbour cross blur with edge coordinates clamped to the nearest pixel;
- `detail = center - 0.25 * (left + right + up + down)`;
- no change when `abs(detail) <= threshold`;
- otherwise add the same signed soft-threshold delta to R, G and B;
- no post-sharpen clipping.

The pass reads A and writes B. It must not update in place because neighbouring
invocations would then observe order-dependent values. When amount is zero,
the implementation may skip this dispatch and route A to DI, but telemetry must
report that the pass was disabled rather than invent a GPU duration.

### 4.3 DaVinci Intermediate pass

The Stage 1 precise shader follows the current production LUT contract, not a
new approximate curve:

- the two 65,536-entry FP32 LUT segments are generated from the existing CPU
  `DaVinciIntermediateLut` and uploaded once;
- the linear toe, segment selection and interpolation boundaries remain those
  in `log_curve.hpp`;
- values above the LUT high range use the analytic DI formula;
- `preserve_by_curve`, `clamp_zero` and `error` remain distinct policies.

For `NegativePolicy::error` and any shader-created non-finite value, shaders set
a per-slot control-status flag. The flag is read only after that slot's fence
signals (including during final drain). Detection aborts the Vulkan pipeline
and causes normal partial-file cleanup; an invalid final MOV must never be
published. This small control-status transfer is reported separately and is
not described as a pixel/image readback.

### 4.4 RGB-to-YUV pass

Factor the current RGB-to-YUV descriptor path so it can consume the
device-resident TargetLog buffers directly. Its filter, deterministic noise,
rounding, legal-range clamp, Y/Cb/Cr storage-image writes and frame-index seed
remain unchanged. Stage 1 must not upload TargetLog a second time.

### 4.5 Parameters

Use fixed-width, explicitly padded C++/GLSL structures with compile-time C++
size/offset assertions. The logical parameter set is:

| Owner | Parameters |
|---|---|
| frame | width, height, frame index |
| color | 3x3 row-major matrix, exposure stops or validated exposure scalar, input scale |
| sharpen | amount, threshold, enabled |
| DI | negative policy and immutable LUT metadata/constants |
| YUV | quality/fast chroma filter and deterministic-dither flag |

Do not pass `CameraColorSolution` directly as a raw uniform block: it contains
FP64 diagnostic/setup data and has no stable shader ABI.

## 5. Ownership and synchronization contract

The existing direct-frame contract remains authoritative:

1. recycle a slot only after its fence signals;
2. upload Camera RGB only into that recycled slot;
3. allocate and lock an AVVkFrame from the exact encoder frame pool;
4. record all Stage 1 passes and the existing output-image transition in one
   command buffer, with storage-buffer barriers between logical passes;
5. wait on each AVVkFrame timeline semaphore's current value and signal
   `sem_value + 1` in the queue submission;
6. update FFmpeg layout/access/queue-family/semaphore metadata before unlock;
7. return that exact AVVkFrame to `VulkanProResEncoder`;
8. retain upload/intermediate buffers and image views until the slot fence
   signals.

All queue submission continues through the FFmpeg Vulkan queue lock. Normal
operation must not add `vkQueueWaitIdle()` or `vkDeviceWaitIdle()`. Fence waits
are allowed for bounded slot reuse, drain, error handling and destruction.

## 6. Golden and validation contract

Test-only readback may expose Camera-to-DWG, sharpened TargetLinear and
TargetLog results. Production code must not expose or enable those readbacks.

### Required corpus

- Stage 0 real frames 0, 120 and 239;
- `color-and-frequency-v1` for negative toe, saturation, fine lines, diagonal
  edges, zone plate and high-frequency chroma;
- constant neutral and RGB impulse/step images;
- 1x1 and edge-heavy cases for color/DI, plus minimum-even-width YUV cases;
- negative-policy cases and finite/non-finite failure injection.

The Stage 0 Camera RGB, TargetLinear, TargetLog and YUV artifacts are the fixed
real-frame boundaries. Synthetic expected values are generated only by the
unchanged CPU functions listed in section 2.

### Numeric gates

For every finite intermediate plane, record max absolute error, RMSE,
P50/P95/P99 absolute error and worst coordinates. Initial precise acceptance is:

| Boundary | Gate |
|---|---|
| TargetLinear after color | max abs <= 2e-5 and RMSE <= 1e-6 |
| TargetLinear after sharpening | max abs <= 3e-5 and RMSE <= 2e-6 |
| TargetLog after DI | max abs <= 3e-5 and RMSE <= 2e-6 |
| final 10-bit Y, Cb and Cr | max absolute code difference <= 1 LSB |

The intermediate gates are additional diagnostics; they never permit final YUV
to exceed 1 LSB. If a frozen threshold is infeasible because of demonstrated
FP32 operation-order behavior, stop and update this design with measured error
coordinates and a reviewed budget. Do not silently widen a test.

Golden tests also verify unchanged dimensions, plane sizes, border behavior,
negative-policy results and deterministic repeated output. Validation layers,
including the existing GPU-assisted targeted run, must report no new
application errors.

## 7. Telemetry and sidecar

Add sibling GPU timestamp summaries under `pipeline.gpu.stages`:

```text
camera_to_dwg
capture_sharpening
davinci_intermediate
rgb_to_yuv_422
```

Each dispatched pass reports samples, total, mean, P50/P95/P99, min and max.
The full Camera-RGB-to-YUV command-buffer time may be reported separately but
must not replace per-pass timestamps. CPU submission/fence wall time remains
separately named.

The production sidecar additionally records:

```text
pipeline.entry = "camera_rgb_f32"
pipeline.precision = "fp32/precise"
pipeline.demosaic_location = "cpu"
pipeline.color_solution_location = "cpu_fp64"
```

Transfer accounting keeps compressed input, U16 RAW, FP16 RGB and readback at
zero. FP32 upload remains 150,994,944 bytes per 4096x3072 frame, but is labeled
as Camera RGB rather than TargetLog RGB. GPU buffer/image traffic is not PCIe
traffic. Control-status bytes, if any, have their own counter.

## 8. Failure and fallback semantics

- `backend=auto` may fall back only during the existing whole-file preflight;
- forced Vulkan fails explicitly if any Stage 1 shader, buffer, descriptor,
  timestamp or required format capability is unavailable;
- after MOV writing starts, shader failure, status failure, device lost,
  cancellation or encoder failure aborts the entire Vulkan path;
- drain, trailer, close and reopen validation remain required before rename;
- all partial files and Vulkan resources follow the existing cleanup path;
- Stage 0 TargetLog entry is a development rollback point, not a per-frame
  runtime fallback.

## 9. Implementation batches

Keep each batch reviewable as one new GPU semantic plus its golden evidence:

1. **1A - API/resources/test harness:** explicit Camera RGB job boundary,
   per-slot ping-pong storage, test-only readback and sidecar entry identity.
2. **1B - color/exposure:** color shader, parameter ABI, golden report and GPU
   timestamp; production may temporarily continue through CPU TargetLog.
3. **1C - sharpening:** non-in-place pass, border/threshold golden and timestamp.
4. **1D - DI:** shared LUT, policies, status flag, golden and fault injection.
5. **1E - resident chain:** direct device-resident handoff into existing YUV and
   AVVkFrame path; remove TargetLog upload from the Stage 1 production route.
6. **1F - E2E and benchmark:** regressions, validation, full matched A/B report
   and go/no-go decision.

No batch introduces FP16 or pass fusion. Each batch leaves a buildable,
testable rollback point.

## 10. Stage 1 go/no-go

Run one warm-up and at least three full official conversions for both the
frozen Stage 0 executable/config and the Stage 1 candidate under matched
conditions. Verify executable, config, shader and input hashes. Report all
Stage 0 categories plus the four new GPU timestamps and actual entry identity.

Stage 1 is accepted when:

- every numeric, E2E, audio, PTS, MOV, cleanup, fallback and device-lost gate
  passes;
- production has no TargetLinear/TargetLog/YUV readback and no TargetLog upload;
- Camera RGB FP32 upload accounting is exact;
- the full 240-frame sample improves median end-to-end throughput by at least
  20% over Stage 0, with run spread too small to hide the result;
- queue/GPU data show that the new producer supplies the encoder more
  continuously.

If improvement is below 20%, freeze the correct Stage 1 implementation and
profile color, sharpening, DI, barriers, occupancy, VRAM and encoder contention.
Do not immediately fuse passes. If demosaic is then the absolute producer
bottleneck, use the evidence to prioritize Stage 2.
