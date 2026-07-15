# GPU Stage 2E E2E Benchmark and Batch C Decision

Date: 2026-07-15

Status: **GO**. Batch C is complete and the precise U16 RAW Vulkan pipeline is
accepted as the current production-performance candidate. The CPU/default
backend and explicit fallback policy remain unchanged pending release gates.

## Decision

The clean `867c0b1` candidate achieved a 37.747 fps median on the frozen RTX
3060, 4096x3072, 240-frame sample. It is 173.710% faster than accepted Stage 1G
and 449.992% faster than the rebuilt Stage 0 baseline.

| Candidate | Median fps | Min-max fps | Median wall time |
|---|---:|---:|---:|
| Rebuilt Stage 0 | 6.863 | 6.774-7.114 | 34,969.527 ms |
| Accepted Stage 1G | 13.791 | 13.429-13.873 | 17,402.977 ms |
| Stage 2E | 37.747 | 37.530-37.935 | 6,358.192 ms |

The Stage 2 official-run spread is 1.072% of its median. It exceeds the 24 fps
minimum by 57.277% and the 30 fps extended target by 25.822% on this system.

## Matched conditions and identity

All candidates used the same frozen config and input hashes. Stage 2 ran one
warm-up plus three official conversions with forced Vulkan, precise FP32 RCD,
quality chroma, deterministic dither, ProRes 422 HQ, audio and source timing.

| Item | Value |
|---|---|
| Candidate commit | `867c0b1999e42f71f37159b0ca54f837456b0fe4` |
| Dirty | `false` |
| Executable SHA-256 | `ADA692D47A77A11EC38BD6D1D976A0308E467D86305856F3E3D01E91216549BC` |
| Config SHA-256 | `C0EE7C9E58BDFF969D512F7FEE705BC1AFCE5B162899A735BAED3614D0BE82E9` |
| Input SHA-256 | `2B4066B21E63458B9BCFCB9B503B58D241FFE3AE99E021BF330E3F612F17F706` |
| Output bytes | `1,402,785,253` |
| Output SHA-256 | `4A6973BD5295037477FA8D0AD3A0C6ABB813B724162F5E5E6F7EBBCA51FB0F56` |

## Official runs

| Run | fps | wall ms | CPU mean % | GPU mean % | VRAM delta MiB |
|---|---:|---:|---:|---:|---:|
| 1 | 37.747 | 6,358.192 | 9.89 | 79.92 | 2,955 |
| 2 | 37.530 | 6,394.829 | 9.75 | 72.00 | 2,876 |
| 3 | 37.935 | 6,326.600 | 9.55 | 72.85 | 2,868 |

Median-of-run GPU stage means were calibration 1.973 ms, aggregate precise RCD
6.443 ms, Camera-to-DWG 0.904 ms, sharpening 0.915 ms, DI 1.034 ms and YUV
0.603 ms. Frame preparation was 2.870 ms and encoder send was 21.799 ms.

The much faster producer now fills the bounded job queue: the median run had
223 job waits, three slot waits, zero packet waits, job depth 10, prepared-frame
depth two and packet depth one. This is honest backpressure, not a stall; output
throughput is already above both product targets. Median sampled VRAM delta was
2,876 MiB, 844 MiB above Stage 1G, consistent with two slot-owned RAW/RCD
working sets.

## Correctness and production invariants

- Real frames 0, 120 and 239 passed final Y, Cb and Cr at exactly 1 LSB, both
  normally and with Vulkan validation enabled.
- Every official run entered at `raw_mosaic_u16`, uploaded exactly
  6,039,797,760 U16 bytes, uploaded zero FP32 RGB, and reported zero pixel/YUV
  readback.
- Every run reported 240 timestamp samples for calibration, RCD, color,
  sharpening, DI and YUV; it read exactly 960 control-status bytes and reported
  zero status failures.
- Each output contained 240 ProRes HQ `yuv422p10le` video packets and 377 PCM
  48 kHz stereo audio packets. Start/end A/V offsets remained +0.018743 ms and
  +40.288409 ms. FFmpeg decoded the complete final MOV with no error.
- Release CTest passed 72/72 tests; seven real-sample/high-memory tests remained
  opt-in. The Stage 2 real-frame test passed separately with 20 assertions.

## Reproduction

```powershell
.\scripts\benchmark-gpu-stage0.ps1 `
  -ValidateStage2Raw `
  -OutputDirectory .\test-output\gpu-stage2e `
  -OfficialRuns 3

$env:MCRAW_STAGE2_REAL_SAMPLE = `
  (Resolve-Path .\mcraw_sample\260710_142121_VIDEO_49mm.mcraw).Path
$env:MCRAW_VULKAN_VALIDATION = "1"
.\build\msvc-release\Release\mcraw_tests.exe `
  "Vulkan resident U16 RAW chain matches real Stage 0 final YUV frames"
```

## Batch C closure

Stages 2A through 2E are implemented as independent rollback points. Numeric
quality, all-CFA calibration/RCD, resident ownership, forced/fallback failure,
transfer accounting, complete MOV and matched performance gates all pass.
Batch D may now evaluate separately identified precise/fast optimizations; no
approximation or default-backend change is implied by this acceptance.
