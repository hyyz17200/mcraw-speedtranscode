# GPU Stage 1B Camera RGB Color/Exposure Validation

Date: 2026-07-15

Status: Stage 1B shader, ABI, golden and timestamp validation complete. This
batch does not switch the production writer from the Stage 0 TargetLog entry.

## Implemented boundary

```text
planar unnormalized Camera RGB FP32
  -> camera_to_dwg.comp.glsl
  -> device-local planar TargetLinear FP32 ping buffer A
```

CPU continues to own `CameraNeutral`, dual-illuminant interpolation,
ForwardMatrix/Bradford selection and FP64 `camera_to_target` construction. The
GPU receives the final row-major 3x3 matrix and a separately computed exposure
scale. Input normalization remains `1/65535`.

The shader uses one invocation per pixel and does not clip negative or
super-white values. Input and parameters receive explicit finite-value checks
before test dispatch.

## Frozen parameter ABI

The C++ and GLSL push-constant block is 64 bytes:

| Offset | Field |
|---:|---|
| 0 | `uint32 width` |
| 4 | `uint32 height` |
| 8 | `float exposure_scale` |
| 12 | reserved |
| 16 | padded matrix row 0 (`vec4`) |
| 32 | padded matrix row 1 (`vec4`) |
| 48 | padded matrix row 2 (`vec4`) |

C++ `sizeof` and row-offset assertions prevent silent ABI drift. Each slot has
one six-binding descriptor set: three host-visible Camera RGB inputs and three
device-local TargetLinear outputs in ping buffer A.

## Synchronization and telemetry

The color pass records into the slot command buffer and uses the existing
FFmpeg device queue lock. A compute-write to transfer-read buffer barrier is
used only for the golden readback route, followed by a transfer-write to
host-read barrier. Production resources are still constructed without
readback buffers.

Two timestamp queries per slot surround the color dispatch. Telemetry exposes
samples, total, mean, P50/P95/P99, min, max and last GPU duration. CPU fence
wall time is not reported as shader execution time.

## Golden results

Hardware: RTX 3060, NVIDIA 576.02. Build type: MSVC Release.

| Corpus | Max abs error | RMSE | Gate | Result |
|---|---:|---:|---:|---|
| 128x36 negative/super-white/matrix synthetic | 2.38419e-7 | 1.87853e-8 | max <=2e-5, RMSE <=1e-6 | pass |
| real Stage 0 frame 0, 4096x3072 | 2.38419e-7 | 1.57234e-8 | max <=2e-5, RMSE <=1e-6 | pass |

The synthetic case repeated three dispatches across two slots and produced
three timestamp samples. The validation-enabled real-frame run measured
11.9165 ms for the standalone 4K color shader. This single validation sample is
not a benchmark or an end-to-end performance claim.

Validation produced only environment/settings warnings (GPU-AV plus core
checks and installed implicit capture layers); it reported no application
validation error for the Stage 1B resources or dispatch.

## Reproduction

```powershell
cmake --build --preset msvc-release --target mcraw_tests -- /m
ctest --preset msvc-release -R "Vulkan Camera RGB color pass" --output-on-failure

$env:MCRAW_STAGE1_REAL_SAMPLE = `
  (Resolve-Path "mcraw_sample\260710_142121_VIDEO_49mm.mcraw").Path
$env:MCRAW_VULKAN_VALIDATION = "1"
.\build\msvc-release\Release\mcraw_tests.exe `
  "Vulkan Camera RGB color pass matches a real Stage 0 frame" `
  --success --reporter console
```

The 4K real-frame test is opt-in because it allocates the real Camera RGB,
TargetLinear reference/output and Vulkan upload/ping/readback resources.

## Artifact identity

These hashes describe the validated dirty-tree candidate based on rollback
commit `c7727868c8ca1783d2f2651805c8ae28b9ee8133`:

| Artifact | SHA-256 |
|---|---|
| `camera_to_dwg.comp.glsl` | `D4330AF8BB7430ED0D45F41B25B139122C3AE4E2DFC6CD8F16827FA2F6F31745` |
| generated `camera_to_dwg.comp.spv` | `2E72294855E4E5D2E5462DCB003AB7398840AEE0F3422E8AAB459FFA1987AE91` |
| Release `mcraw_tests.exe` | `DA19BC5721DC9C69C74E6D98EE4838A84822781F9BB9BD60C0973D6A3CD54A88` |

## Next boundary

Stage 1C adds only the non-in-place TargetLinear capture-sharpening pass from
ping A to ping B, with edge/threshold golden tests and its own timestamps. It
must not fuse color and sharpening or enable FP16.
