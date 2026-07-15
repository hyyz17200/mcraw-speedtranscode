# GPU Stage 3F E2E Benchmark and Batch D Decision

Date: 2026-07-15

Status: **GO**. Batch D is complete. Precise, balanced, and fast are explicit,
truthfully reported Vulkan presets; CPU/default and fallback policy are unchanged.

## Accepted modes

| Preset | Intermediate storage | DI | Dither | Demosaic |
|---|---|---|---|---|
| `vulkan-precise.json` | FP32 | FP32 LUT | deterministic | GPU precise RCD |
| `vulkan-balanced.json` | packed FP16, FP32 compute | FP32 LUT | deterministic | GPU precise RCD |
| `vulkan-fast.json` | packed FP16, FP32 compute | FP32 analytic | deterministic | GPU precise RCD |

The old public `precision` placeholder was removed from schema v1. Sidecars
record requested mode and actual storage, DI, dither, and demosaic implementations.

## Final matched benchmark

The committed presets used the same Release executable, RTX 3060 / NVIDIA
610.62, 4096x3072 240-frame sample, ProRes 422 HQ, quality chroma, audio, source
timing, one warm-up, and three official runs each.

| Mode | Median fps | Min-max fps | Median wall ms | Sampled VRAM delta median |
|---|---:|---:|---:|---:|
| precise | 34.776 | 34.667-35.059 | 6,901.324 | 2,879 MiB |
| balanced | 36.126 | 35.786-36.338 | 6,643.488 | 2,591 MiB |
| fast | 36.857 | 36.463-37.064 | 6,511.638 | 2,591 MiB |

Balanced is 3.88% faster than precise. Fast is 5.98% faster than precise and
2.02% faster than balanced. All are above the 24/30 fps product targets.

| Mode | RCD | Color | Sharpen | DI | YUV |
|---|---:|---:|---:|---:|---:|
| precise | 6.170 ms | 0.958 ms | 0.960 ms | 1.060 ms | 0.631 ms |
| balanced | 6.085 ms | 0.751 ms | 0.640 ms | 0.904 ms | 0.627 ms |
| fast | 6.081 ms | 0.705 ms | 0.661 ms | 0.494 ms | 0.646 ms |

Every official run uploaded exactly 6,039,797,760 U16 RAW bytes, uploaded no
FP16/FP32 RGB over PCIe, performed zero pixel/YUV readback, emitted 240 stage
timestamp samples, retained precise RCD, and reported deterministic dither.

## Quality and rejected experiments

- Balanced FP16 storage: real frames 0/120/239 had Y/Cb/Cr max and P99 1 LSB;
  worst RMSE was 0.201 LSB.
- Fast analytic DI: the same max/P99 1 LSB and worst RMSE 0.201 LSB.
- Dither removal: rejected because +0.063% median E2E was benchmark noise.
- Bilinear fast demosaic: rejected before performance testing; max error reached
  473 LSB with broad structural chroma error. The prototype was removed.

## Batch E profiler input

Fast already sustains 36.857 fps, above the 30 fps extension target. Official
CPU RAW decode was about 56-61 ms/frame in earlier profiling but is overlapped
across producer tasks; current RCD remains about 6.08 ms and encoder send remains
roughly 22 ms/frame. The evidence does not show CPU decode starving the pipeline.
Batch E should begin with a decoder ROI/profile decision, not immediate GPU
compression 6/7 implementation.

## Reproduction

```powershell
.\scripts\benchmark-gpu-stage0.ps1 -ValidateStage2Raw -Config config/vulkan-precise.json
.\scripts\benchmark-gpu-stage0.ps1 -ValidateStage2Raw -Config config/vulkan-balanced.json
.\scripts\benchmark-gpu-stage0.ps1 -ValidateStage2Raw -Config config/vulkan-fast.json
```

Batch D acceptance does not make Vulkan the default and does not close the NLE,
multi-vendor hardware, long-run, or validation-race release gates.
