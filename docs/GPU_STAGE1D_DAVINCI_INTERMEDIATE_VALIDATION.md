# GPU Stage 1D DaVinci Intermediate Validation

Date: 2026-07-15

Status: Stage 1D shader, shared LUT, negative policies, control-status fault
handling, golden and timestamp validation complete. This batch does not connect
the production writer or replace the Stage 0 TargetLog entry.

## Implemented boundary

```text
device-local planar sharpened TargetLinear FP32 ping buffer B
  -> davinci_intermediate.comp.glsl
  -> device-local planar TargetLog FP32 ping buffer A
```

The test chain records color/exposure, sharpening and DaVinci Intermediate in
one slot-owned command buffer. Ping A is reused for TargetLog only after an
explicit barrier makes the earlier color writes and sharpening reads complete.
Only the test route then barriers TargetLog for transfer readback. Production
resources continue to allocate no intermediate image readback.

## Curve and LUT contract

The shader follows the existing `DaVinciIntermediateLut` contract:

- values through the linear cut use the frozen linear slope;
- `(cut, 1]` uses the first 65,536-entry FP32 segment;
- `(1, 100]` uses the second 65,536-entry FP32 segment;
- values above 100 use the analytic DI formula;
- interpolation index, fraction and endpoint behavior match the CPU path.

The two tables occupy one immutable 524,288-byte pipeline-owned, device-local
buffer. They are copied once through a temporary staging buffer during resource
construction and shared by every slot.

## Frozen parameter and status ABI

The C++ and GLSL push-constant block is 16 bytes:

| Offset | Field |
|---:|---|
| 0 | `uint32 width` |
| 4 | `uint32 height` |
| 8 | `uint32 negative_policy` |
| 12 | `uint32 entries_per_segment` |

C++ assertions freeze both the block size and enum mapping:
`preserve_by_curve=0`, `clamp_zero=1`, `error=2`.

Each slot owns one four-byte host-visible control-status word. The shader uses
bit 0 for a negative rejected by `error` and bit 1 for a non-finite input or
output. The word is reset before dispatch and read only after that slot's fence
signals. A non-zero status raises `processing_failed`; pixel output is not
published. Status capacity, bytes read and failures are separate telemetry.

## Synchronization and telemetry

The chain uses compute write/read barriers from color to sharpening and from
sharpening to DI. The second barrier also protects reuse of ping A as the DI
output. DI output receives a compute-write to transfer-read barrier only for
golden readback, while the status word receives a shader-write to host-read
barrier.

Two timestamp queries per slot surround DI independently of the color and
sharpening timestamps. Telemetry exposes samples, total, mean, P50/P95/P99,
min, max and last GPU duration.

## Golden and fault-injection results

Hardware: RTX 3060, NVIDIA 576.02. Build type: MSVC Release.

| Corpus | Max abs error | RMSE | Gate | Result |
|---|---:|---:|---:|---|
| LUT cuts/endpoints, negative toe and >100 synthetic | 1.19209e-7 | 2.98023e-8 | max <=3e-5, RMSE <=2e-6 | pass |
| one-pixel negative/cut/high analytic boundary | <=3e-5 | <=2e-6 | max <=3e-5, RMSE <=2e-6 | pass |
| real Stage 0 frame 0, 4096x3072 | 5.96046e-8 | 1.17004e-8 | max <=3e-5, RMSE <=2e-6 | pass |

The synthetic corpus passes both `preserve_by_curve` and `clamp_zero` and is
bit-deterministic across repeated dispatches. `NegativePolicy::error` rejects a
negative channel through status bit 0. A finite Camera RGB input deliberately
overflowed by the color matrix is detected through status bit 1, proving the
shader-created non-finite fault path.

The LUT-boundary run measured 0.004096 ms for the small standalone DI pass. The
validation-enabled real-frame run measured 3.48774 ms. These single samples are
validation evidence, not benchmarks or end-to-end performance claims.

Validation produced only environment/settings warnings (GPU-AV plus core
checks and installed implicit capture layers); it reported no application
validation error for the LUT upload, descriptors, status atomics, barriers or
dispatch.

## Regression

The complete MSVC Release build passed. CTest reported 59 tests with no
failure; 55 ran and passed, while four opt-in 4K tests skipped without their
environment variables. The validation-enabled real Stage 1D test was then run
separately and passed.

## Reproduction

```powershell
cmake --build --preset msvc-release -- /m
ctest --preset msvc-release -R "Vulkan DaVinci Intermediate" --output-on-failure
ctest --preset msvc-release --output-on-failure

$env:MCRAW_STAGE1_REAL_SAMPLE = `
  (Resolve-Path "mcraw_sample\260710_142121_VIDEO_49mm.mcraw").Path
$env:MCRAW_VULKAN_VALIDATION = "1"
.\build\msvc-release\Release\mcraw_tests.exe `
  "Vulkan DaVinci Intermediate matches a real Stage 0 frame" `
  --success --reporter console
```

## Artifact identity

These hashes describe the validated dirty-tree candidate based on Stage 1C
commit `b43fd79745f74a56f1007e5ba5ff0432b0c21413`:

| Artifact | SHA-256 |
|---|---|
| `davinci_intermediate.comp.glsl` | `AB320CBC46DAFA57C67303A4B8DE412A0E66438F0403928546F35D4994AFF943` |
| generated `davinci_intermediate.comp.spv` | `EA44BA467701AB4214C210FCE194CF81F0129F83FE7A71F61DC9B4FC3C42EDFC` |
| Release `mcraw_tests.exe` | `5D43768B22DA998F6BEE981BE59DDE3B101B63392816D22ED2763277B35F124E` |

## Next boundary

Stage 1E connects this resident Camera RGB -> color -> sharpening -> DI chain
directly to the existing Vulkan RGB-to-YUV pass and encoder-owned AVVkFrame.
That batch removes TargetLog upload only from the new Stage 1 production route
and must retain the Stage 0 rollback path, queue/semaphore ownership, fallback
and cleanup semantics.
