# Batch E-C: updated official decoder capacity

Date: 2026-07-16

## Identity and method

- Application base commit: `5c734bb` plus the benchmark tool in this change.
- Official decoder: `2c49edb17277c07989ff90bd3a3bf557c2f68b4a`.
- Build: MSVC Release, AVX2 enabled.
- Hardware: Ryzen 7 3700X (8C/16T), 64 GiB RAM, RTX 3060 12 GiB,
  NVIDIA 610.62.
- Long decoder corpus: `260623_135705_VIDEO_25mm.mcraw`, 4096x3072,
  compression 7, 826 frames, SHA-256
  `5114DC5108C871813EF4711F050DFFBBB24B951BAE8275661D26660E78004E5A`.
- Full-pipeline corpus: `260710_142121_VIDEO_49mm.mcraw`, 4096x3072,
  compression 7, 240 frames, SHA-256
  `2B4066B21E63458B9BCFCB9B503B58D241FFE3AE99E021BF330E3F612F17F706`.
- Each worker/mode group ran one full warm-up followed by three full measured
  passes. Tables report the median and measured range.

`mcraw-decoder-benchmark` exposes two deliberately separate measurements:

1. `decoder-only` preloads every compressed payload, reuses one U16 output
   vector per persistent worker, and calls the frozen official compression 7
   primitive. Its process working set includes the 7.43 GB in-memory corpus and
   is therefore not used as a production memory estimate.
2. `load-frame` uses one persistent official `motioncam::Decoder` per worker and
   includes real file reads, metadata JSON parse, decompression, and exact byte
   output. It does not preload payloads; its working set is the capacity datum.

Raw CSV artifacts and the exact executable hashes are retained under the local
`test-output/batch-ec` run directory. The three CSV SHA-256 values are:

| Artifact | SHA-256 |
|---|---|
| `compression7-decoder-only.csv` | `EE67D27FDEF74D904F42E8F3A752F912889BF2A7F485297751760A81D65318D3` |
| `compression7-load-frame.csv` | `3D14462A213E2F28018AC513CF9DD94F30D31DA53219188F988173172C0945B6` |
| `compression7-full-pipeline.csv` | `AA846E0AEBEB824C6E6CFAC9D3B99A50EC8BDED48FAE57FE22D96393633BC701` |

## Decoder-only capacity

| Workers | Median fps | Range | Median CPU cores | Median P95 | Capacity |
|---:|---:|---:|---:|---:|---:|
| 1 | 108.53 | 108.36-108.58 | 1.00 | 9.39 ms | 1,365.6 MP/s |
| 2 | 206.65 | 205.92-208.53 | 2.00 | 10.60 ms | 2,600.3 MP/s |
| 4 | 387.50 | 382.47-389.62 | 4.00 | 13.17 ms | 4,875.8 MP/s |
| 6 | 487.71 | 479.59-493.29 | 5.96 | 15.76 ms | 6,136.8 MP/s |
| 8 | 533.57 | 522.69-534.28 | 7.94 | 18.39 ms | 6,713.8 MP/s |

## Real `loadFrame` capacity

| Workers | Median fps | Range | Median CPU cores | Median P95 | Peak working set |
|---:|---:|---:|---:|---:|---:|
| 1 | 88.39 | 87.93-88.42 | 1.00 | 11.51 ms | 46.65 MiB |
| 2 | 168.86 | 168.03-170.41 | 2.00 | 12.51 ms | 80.50 MiB |
| 4 | 290.86 | 290.29-295.32 | 3.96 | 15.50 ms | 157.60 MiB |
| 6 | 355.54 | 355.38-355.85 | 5.93 | 19.12 ms | 227.23 MiB |
| 8 | 367.04 | 364.95-367.14 | 7.73 | 25.10 ms | 301.10 MiB |

The updated upstream decoder is substantially faster than the historical
`release/0.2` pre-evaluation. Six workers exceed the 90/120/130 fps capacity
tiers by large margins. Eight workers add only 3.2% over six while increasing
peak working set by 32.5% and P95 latency by 31.3%.

## Full Vulkan pipeline

The same executable/config/input was used for each worker matrix. `cpu_threads`
was fixed at 16; only `max_parallel_frames` and the already published precise or
fast preset changed. Every measured output contained 240 video packets and was
deleted after its sidecar and summary values were captured.

| Mode | Workers | Median process fps | Range | Median conversion core | Median decode P95 |
|---|---:|---:|---:|---:|---:|
| precise | 1 | 34.87 | 33.69-35.95 | 6,494.43 ms | 26.79 ms |
| precise | 2 | 33.48 | 33.34-33.53 | 6,784.90 ms | 33.25 ms |
| precise | 4 | 34.68 | 33.07-38.20 | 6,485.86 ms | 45.84 ms |
| precise | 6 | 37.77 | 37.67-37.89 | 5,985.95 ms | 60.98 ms |
| precise | 8 | 37.60 | 37.35-37.95 | 6,011.95 ms | 79.94 ms |
| fast | 1 | 35.22 | 35.16-35.26 | 6,416.48 ms | 26.75 ms |
| fast | 2 | 39.43 | 39.33-39.66 | 5,691.34 ms | 31.21 ms |
| fast | 4 | 39.39 | 39.23-39.43 | 5,700.91 ms | 43.71 ms |
| fast | 6 | 39.31 | 39.11-39.45 | 5,720.28 ms | 62.44 ms |
| fast | 8 | 39.04 | 38.93-39.33 | 5,724.83 ms | 78.05 ms |

Precise is stable and best at six workers; eight does not improve conversion
core or process wall. Fast saturates at two workers; four through eight are
within 0.4 fps while increasing decode latency and resident RAW work. Slot wait
at every saturated setting confirms the decoder is feeding downstream faster
than the shared GPU pipeline consumes frames.

## Go/no-go decisions

- The 90/120/130 fps compression 7 capacity tiers are satisfied by the updated
  official decoder. Six workers remain the precise automatic candidate.
- Fast should automatically use two workers on this production shape because
  additional workers do not improve end-to-end throughput.
- Batch E-D M0-M5 repository-owned compression 7 fast decoder is **no-go**.
  The updated official baseline is not an end-to-end bottleneck, so the guide's
  conditional prerequisite is false. No custom scalar/SSE4.1/AVX2 decoder is
  implemented.
- Compression 6 fast optimization is **no-go** without a real compression 6
  corpus, as required by Batch E-E.

The automatic execution plan implements those measured defaults: six frame
workers for precise (and the CPU reference), two for a requested Vulkan/auto
fast mode. An explicit `max_parallel_frames` still overrides the recommendation
for capacity testing and different hardware.

## Remaining format gate

Both local real samples are compression 7. There is no real compression 6 file
from which to freeze first/middle/last payload, metadata, and official U16
hashes or to run the same corruption and capacity matrix. The upstream legacy
implementation is pinned and built, but Batch E-F's compression 6 corpus gate
cannot be truthfully declared complete until such a sample is supplied.
