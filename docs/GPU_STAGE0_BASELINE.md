# GPU Stage 0 Baseline and Profiler Contract

Date: 2026-07-14

Status: instrumentation and the initial dirty-tree capture are complete. A
committed rollback-point recapture remains required before Stage 1 starts.

## Frozen product semantics

- Minimum product target: 24 fps at 4096x3072 ProRes HQ.
- Extended target: 30 fps.
- Reference mode: `fp32/precise`.
- Precise final Y/Cb/Cr maximum error: at most 1 10-bit LSB.
- CPU remains the default reference and fallback. Vulkan remains opt-in.

`config/gpu-stage0-corpus.json` is the machine-readable corpus and quality
contract. `config/vulkan-stage0-baseline.json` forces Vulkan so a benchmark can
never silently become a CPU fallback result.

## Golden boundaries

The baseline capture extracts first, middle, and last real frames at these
boundaries:

1. compressed MotionCam payload;
2. official decoded U16 mosaic;
3. calibrated FP32 mosaic in the production librtprocess 0..65535 domain;
4. unnormalized Camera RGB FP32 from production RCD;
5. exposed and capture-sharpened TargetLinear FP32;
6. DaVinci Intermediate TargetLog FP32;
7. deterministic 10-bit Y/Cb/Cr planes.

The production-named `extract-frame` stages are intentionally separate from
the older diagnostic stages. Their configuration and arithmetic boundaries
match `CpuPipeline`, including exposure, sharpening, negative policy, chroma
filter, and deterministic dither.

Run:

```powershell
.\scripts\capture-gpu-stage0-baseline.ps1
```

The command verifies the 2.4 GB source SHA-256 before extraction, records the
commit and dirty state, hashes the executable/config/shaders, captures runtime
capabilities, and writes every artifact hash to
`test-output/gpu-stage0-baseline/baseline-manifest.json`.

## GPU profiler contract

Vulkan RGB-to-YUV now uses timestamp queries around its command-buffer pass.
CPU fence completion remains separately named `last_dispatch_wall_ms`; it is
not reported as shader execution time. Production sidecars report GPU
timestamp sample count, total, mean, P50/P95/P99, minimum, and maximum under:

```text
pipeline.gpu.stages.rgb_to_yuv_422
```

Every future Vulkan pass must add a sibling named stage with the same summary.
The production path may explicitly report timestamp support as unavailable,
but such a run cannot satisfy the Stage 0 benchmark gate.

Transfer telemetry is also type-specific. The current path reports FP32 RGB
upload bytes and compressed packet download bytes; compressed input, U16 RAW,
and FP16 RGB upload counters are explicitly zero. GPU image-to-image traffic is
explicitly excluded from PCIe accounting. The old `rgb_upload_bytes` remains as
a compatibility alias for the current FP32 value.

## Matched benchmark

Run:

```powershell
.\scripts\benchmark-gpu-stage0.ps1
```

It performs one warm-up and at least three full forced-Vulkan conversions of
the fixed 240-frame sample. It rejects fallback or missing GPU timestamps,
samples process CPU/RAM and NVIDIA device utilization/VRAM when available, retains
each sidecar and stderr log, reports median/min/max throughput and wall time,
hashes the final MOV, and embeds ffprobe packet/stream JSON.

External PCIe profilers remain authoritative for link utilization. The
sidecar's `rgb_upload_bytes` is the exact producer upload accounting; Vulkan
image-to-image traffic is not counted as PCIe traffic.

## Stage 0 go/no-go

Do not start Stage 1 until a clean committed rollback point has produced:

- a manifest with `dirty=false` and stable hashes on a repeat capture;
- one warm-up plus three full official runs with median and spread;
- one GPU timestamp sample per output frame;
- stable frame/packet count and PTS in ffprobe output;
- no validation-layer application error in the targeted validation run;
- unchanged Phase 8 fallback, forced-failure, cleanup, audio, and MOV behavior.

If repeated full runs fluctuate enough to hide a 20% Stage 1 improvement, fix
the benchmark environment or telemetry before moving the pipeline boundary.

## Initial capture (dirty-tree implementation validation)

The first run after adding this contract produced 21 hashed artifacts (three
frames times seven boundaries, 1,767,327,738 bytes) and completed one warm-up
plus three full forced-Vulkan conversions. It is evidence that the Stage 0
machinery works, not the final frozen rollback point because its manifest
correctly records `dirty=true`.

| Measurement | Initial result |
|---|---:|
| Throughput median | 7.133 fps |
| Throughput min / max | 7.098 / 7.412 fps |
| Wall-time median | 33.645 s |
| RGB-to-YUV GPU timestamp mean, median of runs | 13.007 ms/frame |
| GPU timestamp samples per run | 240 |
| Exact FP32 RGB upload per run | 36,238,786,560 bytes |
| Compressed packet download per run | 1,401,236,366 bytes |
| GPU job queue peak | 7 |
| Packet queue peak | 1 |
| Backpressure waits | 0 |
| Process CPU utilization mean | 80.2% of 16 logical processors |
| Process RAM peak | 4,158,873,600 bytes |
| `nvidia-smi` device utilization mean / sampled peak | 18.3% / 65% |
| `nvidia-smi` device VRAM baseline / peak delta | 1,212–1,229 / 1,462 MiB |
| Final MOV size | 1,402,786,745 bytes |
| Video / audio packets / duration | 240 / 377 / 8.031021 s |

The official-run range is about 4.4% of the minimum throughput and does not
hide the Stage 1 go/no-go target of a reproducible 20% improvement. The low
mean device GPU utilization, zero backpressure, and roughly 13 ms RGB-to-YUV pass
continue to support the existing conclusion that the CPU producer is starving
the Vulkan encoder pipeline.

The `nvidia-smi` values are device-global rather than process-isolated. The
report records the pre-run VRAM baseline and peak delta so a run is only valid
for VRAM analysis when other GPU workloads are controlled.
