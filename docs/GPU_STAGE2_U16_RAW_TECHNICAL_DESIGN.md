# GPU Stage 2 U16 RAW Technical Design

Date: 2026-07-15
Status: implementation contract; no Stage 2 pixel implementation is included

This document freezes Batch C from `GPU_PIPELINE_FORMAL_ACTION_GUIDE.md`.
Stage 1 remains the rollback point. Any implementation that changes a boundary
below must first update this document or record an ADR.

## 1. Scope and completion definition

The only production boundary moved by Stage 2 is:

```text
before: CPU calibration/RCD -> Camera RGB FP32 upload -> Stage 1
after:  CPU official decode -> RAW mosaic U16 upload
        -> GPU calibration -> GPU precise RCD -> Stage 1
```

CPU official MCRAW decode, metadata normalization, FP64 color-solution setup,
Stage 1 color/sharpen/DI/YUV, ProRes, audio, muxing and publication remain
unchanged. Stage 2 is complete only after stage goldens, all four Bayer phases,
production zero-readback checks, failure tests, exact transfer accounting and a
matched full-sample benchmark pass.

### Non-goals

- no compressed MCRAW GPU decode;
- no FP16, approximate demosaic, fast preset or pass fusion;
- no alternate demosaic algorithm on the Stage 2 precise route;
- no change to CPU calibration, librtprocess RCD, metadata, chroma, dither,
  fallback or publication semantics;
- no transfer/compute queue split without profiler evidence.

## 2. Frozen reference and input contract

The CPU truth is `calibrate_raw_for_demosaic()` followed by
`demosaic_unnormalized(..., DemosaicAlgorithm::rcd)`. The Stage 2 input is
`RawMosaicU16`: one contiguous top-to-bottom CFA plane with exactly
`width * height` native-endian `uint16_t` samples and an explicit CFA pattern.

Calibration uses FP64 black and white metadata exactly as the CPU reference:

```text
float((double(raw) - black[cfa_position]) /
      (white[cfa_position] - black[cfa_position])) * 65535.0f
```

Calibration does not clamp negative or super-white values. The following RCD
stage preserves librtprocess 0.12.0 semantics, including its existing
`LIM01(calibrated / 65536.0f)` algorithm-input boundary and non-negative final
planes. This is an RCD rule, not an added calibration clamp.

Stage 2 precise supports only `DemosaicAlgorithm::rcd`. `backend=auto` may use
the Stage 1 preflight fallback for another configured algorithm; forced Vulkan
must reject it explicitly before MOV writing starts.

## 3. Production API seam

Add `CpuPipelineOutput::raw_mosaic` and a semantic `VulkanRawMosaicInput` job
containing the decoded `RawMosaicU16`, four black levels, four white levels and
the existing per-frame Stage 1 parameters. Do not infer RAW semantics from an
untyped byte vector.

The writer adds a distinct U16 entry. Stage 1 Camera RGB and Stage 0 TargetLog
entries remain development rollback paths. The GPU path continues to use the
same FFmpeg-owned logical device, compute queue, AVVkFrame pool, frame timeline
semaphore and publication/failure machinery.

## 4. Resource and pass design

The first correct implementation is deliberately unfused:

```text
host-visible U16 mosaic
  -> calibrate_raw_fp32
  -> device-local calibrated CFA FP32
  -> RCD direction/green/chroma passes
  -> device-local planar Camera RGB FP32
  -> existing Stage 1 color/sharpen/DI/YUV
  -> FFmpeg AVVkFrame
```

Each resident slot owns one U16 upload buffer, all RCD scratch buffers and the
three Camera RGB outputs. Immutable pipelines and descriptor layouts are shared.
Production exposes no calibrated-CFA or Camera-RGB readback. Test-only methods
may synchronously read either boundary after a fence.

The RCD implementation may use more than one dispatch. Logical GPU timestamp
groups are `raw_calibration` and `rcd_demosaic`; optional sub-pass timestamps may
supplement but not replace the RCD aggregate.

## 5. Ownership and synchronization

1. Recycle a slot only after its fence signals and its AVVkFrame has left the
   preparation queue.
2. Upload U16 only into the recycled slot.
3. Record calibration, all RCD passes and the existing Stage 1 passes in that
   slot command buffer, with explicit compute barriers between dependencies.
4. Preserve the existing FFmpeg image layout/access/queue-family and timeline
   semaphore contract.
5. Keep all U16, calibrated, scratch and Camera RGB resources alive until the
   slot fence signals.
6. Do not add normal-path queue/device idle waits.

The current two-slot resident depth remains the initial production default.
Any larger depth requires measured throughput and VRAM evidence.

## 6. Golden and quality contract

Required synthetic coverage includes all four Bayer patterns, constant planes,
RGB impulses/steps, black/white boundaries, negative and super-white calibrated
values, borders/corners, diagonals, fine lines, zone plates and high-frequency
chroma. RCD frames must be at least 32x32, matching the CPU API.

Test-only readback compares:

| Boundary | Initial precise gate |
|---|---:|
| calibrated CFA | max abs <= 0.01 and RMSE <= 0.002 in the 0..65535 domain |
| Camera RGB RCD | max abs <= 2.0 in the 0..65535 domain; RMSE <= 0.05 |
| final Y, Cb, Cr | max absolute code difference <= 1 LSB |

The RCD limits are diagnostic starting gates, not permission for structural
artifacts. Reports also include P50/P95/P99, worst coordinates and separate
border/interior maxima. If normal FP32 operation order cannot meet them, stop
and update this contract with measured evidence; do not silently widen tests.

The calibration gate was frozen from the Stage 2B all-CFA synthetic result:
max abs `0.0078125`, RMSE `0.00137959`. The difference is the expected boundary
between the CPU's FP64 division followed by FP32 storage and the first portable
shader's FP32 arithmetic. This intermediate budget does not widen the final
one-LSB YUV gate.

Real-frame validation uses Stage 0 corpus frames 0, 120 and 239. Final quality
must retain the Stage 1 one-LSB YUV gate.

## 7. Telemetry and sidecar

Production records:

```text
pipeline.entry = "raw_mosaic_u16"
pipeline.precision = "fp32/precise"
pipeline.demosaic_location = "gpu_rcd_precise"
pipeline.color_solution_location = "cpu_fp64"
```

`u16_raw_upload_bytes` must equal `width * height * 2` per submitted frame.
Camera/TargetLog FP32 upload, calibrated/Camera RGB readback and YUV readback
must be zero. GPU scratch traffic is not reported as PCIe traffic. Add timestamp
summaries for `raw_calibration` and aggregate `rcd_demosaic` alongside Stage 1.

## 8. Failure and fallback semantics

- `backend=auto` may fall back only in existing whole-file preflight;
- forced Vulkan rejects unsupported demosaic, dimensions, shader/resource
  creation or device capability before MOV writing;
- invalid metadata remains a CPU-side validation error;
- shader/status, device-lost, cancellation or encoder failure aborts the file;
- drain, trailer, close and reopen validation still precede final rename;
- no per-frame switch between GPU RCD and CPU RCD is allowed.

## 9. Batch C rollback points

1. **2A - contract/API/resources:** explicit RAW job boundary, slot-owned U16
   upload/calibrated/scratch/RGB resources, test readback and sidecar identity.
2. **2B - calibration:** calibration shader, fixed ABI, all-CFA golden and GPU
   timestamp; production remains on Stage 1.
3. **2C - precise RCD:** unfused RCD passes, border parity, all-CFA synthetic and
   real-frame Camera RGB report; production remains on Stage 1.
4. **2D - resident chain:** direct RCD output into Stage 1 and AVVkFrame, exact
   U16 accounting, zero intermediate readback/upload and fault tests.
5. **2E - E2E/benchmark:** full regressions, validation, matched full-sample A/B,
   performance/quality report and Batch C decision.

Each rollback point must build, pass its targeted tests, pass `git diff --check`,
be committed independently and leave a clean worktree before the next point.

## 10. Stage 2 decision

Run one warm-up and at least three matched full conversions for accepted Stage 1
and Stage 2. Stage 2 is accepted when all numeric/E2E/failure/accounting gates
pass and the complete sample shows repeatable improvement over Stage 1. The
24 fps product target remains separate: a correct, faster Stage 2 may be an
accepted rollback point even if subsequent precise optimization is still needed.

If precise RCD quality cannot pass, production remains on Stage 1. An
approximate implementation may only return later under a separately identified
fast preset; it is never relabeled as precise RCD.
