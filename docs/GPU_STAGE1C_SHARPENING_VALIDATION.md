# GPU Stage 1C TargetLinear Sharpening Validation

Date: 2026-07-15

Status: Stage 1C shader, non-in-place resource chain, golden and timestamp
validation complete. This batch does not switch the production writer from the
Stage 0 TargetLog entry.

## Implemented boundary

```text
device-local planar TargetLinear FP32 ping buffer A
  -> sharpen_target_linear.comp.glsl
  -> device-local planar TargetLinear FP32 ping buffer B
```

The test chain records Camera RGB color/exposure, an explicit
compute-write-to-compute-read barrier, and sharpening in one slot-owned command
buffer. Only the test route then barriers ping B for transfer readback.
Production resources still allocate no readback buffers.

The shader preserves the frozen CPU semantics: BT.2020 luma coefficients,
four-neighbour cross blur, clamped edge coordinates, signed soft threshold,
the same delta added to all three channels, and no negative or super-white
clipping. It always reads ping A and writes ping B; it never updates its input
in place.

## Frozen parameter ABI

The C++ and GLSL push-constant block is 16 bytes:

| Offset | Field |
|---:|---|
| 0 | `uint32 width` |
| 4 | `uint32 height` |
| 8 | `float amount` |
| 12 | `float threshold` |

A C++ size assertion prevents silent ABI drift. Each slot has a separate
six-binding sharpening descriptor set: three read-only ping A planes and three
write-only ping B planes.

## Synchronization and telemetry

The resident test chain uses a compute shader-write to compute shader-read
buffer barrier between color and sharpening. The golden readback route adds a
compute-write to transfer-read barrier for ping B and a transfer-write to
host-read barrier for the test buffers.

Two timestamp queries per slot surround the sharpening dispatch independently
of the color timestamps. Telemetry exposes samples, total, mean, P50/P95/P99,
min, max and last GPU duration. CPU fence wall time is not reported as shader
execution time.

## Golden results

Hardware: RTX 3060, NVIDIA 576.02. Build type: MSVC Release.

| Corpus | Max abs error | RMSE | Gate | Result |
|---|---:|---:|---:|---|
| 40x40 edge/threshold/negative/super-white synthetic | 5.96046e-8 | 2.00631e-9 | max <=3e-5, RMSE <=2e-6 | pass |
| real Stage 0 frame 0, 4096x3072 | 2.38419e-7 | 1.94043e-8 | max <=3e-5, RMSE <=2e-6 | pass |

The synthetic run measured 0.003168 ms for the standalone sharpening pass.
The validation-enabled real-frame run measured 4.3264 ms. These single samples
are validation evidence, not benchmarks or end-to-end performance claims.

The amount-zero test is bit-exact against the Stage 1B color output. Invalid
negative amount and threshold parameters are rejected. Validation produced
only environment/settings warnings (GPU-AV plus core checks and installed
implicit capture layers); it reported no application validation error for the
Stage 1C descriptors, barriers or dispatch.

## Reproduction

```powershell
cmake --build --preset msvc-release --target mcraw_tests -- /m
ctest --preset msvc-release -R "Vulkan TargetLinear" --output-on-failure

$env:MCRAW_STAGE1_REAL_SAMPLE = `
  (Resolve-Path "mcraw_sample\260710_142121_VIDEO_49mm.mcraw").Path
$env:MCRAW_VULKAN_VALIDATION = "1"
.\build\msvc-release\Release\mcraw_tests.exe `
  "Vulkan TargetLinear sharpening matches a real Stage 0 frame" `
  --success --reporter console
```

The 4K real-frame test is opt-in because it allocates the real Camera RGB,
TargetLinear CPU reference and Vulkan upload/ping/readback resources.

## Artifact identity

These hashes describe the validated dirty-tree candidate based on Stage 1B
commit `de113b18b1b26e9d60c8c35cc01c60f7643551ae`:

| Artifact | SHA-256 |
|---|---|
| `sharpen_target_linear.comp.glsl` | `A5441B0788FE06E504D018D4440FE9055B4DEB8405D1B5F82DA5064D10FD01ED` |
| generated `sharpen_target_linear.comp.spv` | `D37F30F43086627985A87D09B1684C70739D518E7496935EF7F4CA946F607762` |
| Release `mcraw_tests.exe` | `1ACC9E05755126D2F067CE327704B2B084F99A0E5F9C3A15D0AEF2D33056036C` |

## Next boundary

Stage 1D adds only the FP32 precise DaVinci Intermediate pass, immutable shared
LUT, negative policies, per-slot control-status flag, golden tests and fault
injection. It must not connect the production writer, fuse passes or enable
FP16.
