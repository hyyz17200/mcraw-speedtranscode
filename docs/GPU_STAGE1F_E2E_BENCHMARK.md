# GPU Stage 1F E2E and Benchmark Decision

> Historical decision: superseded by the accepted Stage 1G performance
> recovery in `GPU_STAGE1G_PERFORMANCE_RECOVERY.md`. This report remains the
> evidence for the rejected serialized implementation.

Date: 2026-07-15

Status: Stage 1 implementation and correctness validation complete; performance
acceptance is **NO-GO**. The Camera RGB Vulkan route remains an opt-in,
reviewable rollback point and must not replace the CPU/default path.

## Decision

The frozen gate requires at least 20% higher median full-sample end-to-end
throughput than Stage 0. The matched A/B measured a 27.685% regression:

| Candidate | Median fps | Min-max fps | Median wall time |
|---|---:|---:|---:|
| Stage 0, rebuilt from `622070c` | 6.863 | 6.774-7.114 | 34,969.527 ms |
| Stage 1, `7491bac` | 4.963 | 4.934-5.052 | 48,357.423 ms |

```text
measured improvement = -27.685%
required improvement = +20.000%
decision             = NO-GO
```

The Stage 1 spread is 2.371% of its median and the Stage 0 spread is 4.954%,
so run variation cannot hide the result. Against the historical frozen Stage 0
median of 7.133 fps, Stage 1 is 30.425% slower. Neither candidate approaches
the 24 fps product minimum.

## Matched conditions and identity

Both candidates used the same RTX 3060/NVIDIA 576.02 system, idle benchmark
window, 4096x3072 240-frame sample, forced Vulkan config, async depth 8, RCD,
capture sharpening 0.4, precise FP32, ProRes 422 HQ, source timestamps and
audio. Each ran one warm-up followed by three official conversions.

| Item | SHA-256 |
|---|---|
| Input sample | `2B4066B21E63458B9BCFCB9B503B58D241FFE3AE99E021BF330E3F612F17F706` |
| Shared config | `C0EE7C9E58BDFF969D512F7FEE705BC1AFCE5B162899A735BAED3614D0BE82E9` |
| Rebuilt Stage 0 executable | `256BB76FA91DC5CBBAFB885F80FCA9E17577A064E8D053D5EFD3F569C7392C4E` |
| Stage 1 executable | `C27EB9ECC411FFBD4B4EE79FAB39BDAB457DB6D5B75C49EE318E552BCE021559` |

The rebuilt Stage 0 executable comes from detached source commit
`622070c8a62200aa828efcd7b95b57f9189a1519`, but does not reproduce the old
manifest executable hash
`994A78F2A8C7A9BDA4B9A30E976DDA4B0E28852A2E06406F3EDBA1D53CA10776`.
The MSVC build is not bit-reproducible. For that reason the matched decision
uses the newly rebuilt Stage 0 measured in the same session, while the old
frozen benchmark is reported only as a cross-check.

## Official runs

| Candidate/run | fps | wall ms | GPU mean % | CPU mean % | VRAM delta MiB |
|---|---:|---:|---:|---:|---:|
| Stage 0 / 1 | 6.774 | 35,427.741 | 25.919 | 73.754 | 1,433 |
| Stage 0 / 2 | 7.114 | 33,734.750 | 24.817 | 78.017 | 1,423 |
| Stage 0 / 3 | 6.863 | 34,969.527 | 23.787 | 76.301 | 1,474 |
| Stage 1 / 1 | 5.052 | 47,510.607 | 26.621 | 22.678 | 3,736 |
| Stage 1 / 2 | 4.963 | 48,357.423 | 26.807 | 23.262 | 3,782 |
| Stage 1 / 3 | 4.934 | 48,643.928 | 24.674 | 22.891 | 3,819 |

## Bottleneck evidence

Stage 1 successfully removes CPU color/sharpen/DI work, but moves that work
onto the same GPU/compute resources used by the Vulkan ProRes encoder. The
producer is then throttled at submission:

| Median-of-runs diagnostic | Stage 0 | Stage 1 |
|---|---:|---:|
| Demosaic mean | 397.812 ms/frame | 233.683 ms/frame |
| ProRes submit/wait mean | 0.004 ms/frame | 181.777 ms/frame |
| GPU job queue peak | 5 | 10 |
| Backpressure waits | 0 | 227 |
| Packet queue peak | 1 | 1 |
| Device-global GPU mean | 24.817% | 26.621% |
| VRAM peak delta | 1,433 MiB | 3,782 MiB |

Stage 1 pass means were stable across the official runs: Camera-to-DWG
13.84 ms, sharpening 2.70 ms, DI 3.24 ms and RGB-to-YUV 1.89 ms. The 181.8 ms
submit/wait time, full job queue, 227 backpressure events, low packet depth and
2.35 GiB additional VRAM are the dominant evidence. This points to slot depth,
memory footprint, barriers/occupancy and encoder contention rather than CPU
demosaic as the next profiling boundary. The design requires freezing here;
shader fusion and Stage 2 are not authorized by this result.

## Correctness and production invariants

- Real Stage 0 frames 0, 120 and 239 each passed final Y, Cb and Cr maximum
  error at exactly 1 LSB.
- All three Stage 1 official runs entered at `camera_rgb_f32`, uploaded exactly
  36,238,786,560 Camera RGB bytes, uploaded zero TargetLog bytes, read 960
  control-status bytes, and reported zero status failures.
- Every official run reported 240 samples for Camera-to-DWG, sharpening, DI and
  RGB-to-YUV; pixel/image readback and YUV upload remained zero.
- Final Stage 0 and Stage 1 MOVs each contain 240 ProRes HQ `yuv422p10le`
  packets plus 48 kHz stereo audio. Both complete video streams passed FFmpeg
  decode. Their A/V offsets match exactly: +0.018743 ms at start and
  +40.288409 ms at end.
- The Stage 1 output is 1,402,785,678 bytes with SHA-256
  `AEB9E1C347D6F542BE38B3BDEDFBA58BC14996B0A38A523EC379E499AC8347EB`.
- Stage 1E auto fallback, forced invalid-device cleanup, status fault injection,
  MOV reopen validation and shader validation results remain valid.

## Regression

The final MSVC Release CTest run reported 62 tests and no failures: 57 passed
and five opt-in 4K/real-frame tests skipped. The real Stage 1F YUV test was run
separately with the fixed sample and passed 23 assertions. Full final MOV decode
was also run separately for both benchmark candidates.

## Reproduction

```powershell
# Build the detached Stage 0 source commit separately, then run both candidates.
.\scripts\benchmark-gpu-stage0.ps1 `
  -Executable .\test-output\stage1f-stage0-build\Release\mcraw-speedtranscode.exe `
  -OutputDirectory .\test-output\gpu-stage1f-stage0-rebuilt
.\scripts\benchmark-gpu-stage0.ps1 `
  -Executable .\build\msvc-release\Release\mcraw-speedtranscode.exe `
  -OutputDirectory .\test-output\gpu-stage1f-stage1
.\scripts\compare-gpu-stage1.ps1

$env:MCRAW_STAGE1_REAL_SAMPLE = `
  (Resolve-Path .\mcraw_sample\260710_142121_VIDEO_49mm.mcraw).Path
.\build\msvc-release\Release\mcraw_tests.exe `
  "Vulkan resident Camera RGB chain matches real Stage 0 final YUV frames" `
  --success --reporter console
```

## Stage 1 closure

Stages 1A through 1F are implemented, tested and committed as separate rollback
points. Stage 1 is technically correct but fails performance acceptance, so the
formal Stage 1 action item remains unaccepted. The next authorized work is a
focused performance investigation of resident slot count/VRAM, queue
backpressure, pass barriers/occupancy and ProRes encoder contention, followed
by another matched Stage 1F run.
