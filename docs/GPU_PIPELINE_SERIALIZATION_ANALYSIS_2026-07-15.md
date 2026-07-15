# GPU Pipeline Serialization Analysis and Fix Plan

Date: 2026-07-15
Status: **Analysis only — no code changes made.** Fix plan below is proposed, not accepted.
Author context: investigation of "whole pipeline is much slower than each individual stage".
Hardware baseline: RTX 3060 (12 GB), NVIDIA 610.62, 4096x3072 ProRes 422 HQ, `config/vulkan-fast.json`.

## How to use this document (for agents)

- Every claim below cites a file and line. Line numbers are valid as of commit `a97afbf`.
  Re-verify with the grep commands in Appendix A before acting; do not trust line numbers blindly.
- Primary evidence artifacts (checked in, do not regenerate unless re-benchmarking):
  - `test-output/full-file-parallel-benchmark/benchmark-report.json` (826-frame full file, concurrency 1/2/4)
  - `test-output/full-file-parallel-benchmark/c1-official-1/input-1.mov.json` (per-run telemetry sidecar)
  - `docs/PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md` (encoder-only benchmark)
  - `docs/GPU_STAGE3F_E2E_BENCHMARK.md` (240-frame E2E, stage timings)
- The FFmpeg source referenced is the vendored build tree:
  `.deps/vcpkg/buildtrees/ffmpeg/src/n8.1.2-114db953a4.clean/` (FFmpeg 8.1.2 fork).
  Referred to below as `$FFSRC`.

## 1. TL;DR

There is no single slow stage. Two structural problems make stage times **add up** instead of overlapping:

1. **The Vulkan ProRes encoder is fully synchronous.** Its `async_depth` option is silently
   forced to 1 inside FFmpeg, so every `avcodec_send_frame` submits the encode, blocks until
   the GPU finishes it, and downloads the packet — 26.9 ms per frame, 91% of total wall time.
2. **All GPU work shares one `VkQueue`** (same family, same index 0): the 6 processing passes
   AND the ProRes encode passes execute serially on one hardware queue. Processing frame N+1
   cannot overlap encoding frame N.

Per-frame budget at concurrency 1 (measured): ~10.4 ms processing + ~13–14 ms encode +
~5 ms sync bubbles ≈ **29.6 ms/frame → 33 fps**. The "fast" per-stage numbers (8.6–10.4 ms
processing, 45–56 fps encode) were each measured with the GPU to themselves.

Expected after fixes 1+2+3 below: **~50 fps** (encoder-bound), zero quality change.

## 2. Measured evidence

### 2.1 Full pipeline, single process (sidecar `c1-official-1/input-1.mov.json`)

826 frames, wall 24,431 ms = 29.58 ms/frame = 33.8 fps. GPU util mean 89%.

| Metric | Value | Sidecar JSON path |
|---|---:|---|
| `encoder_send` mean / total | 26.88 ms / 22,199 ms (**91% of wall**) | `pipeline.scheduling.encoder_send` |
| `frame_pack` mean (CPU: memcpy+record+submit) | 2.35 ms | `pipeline.scheduling.frame_pack` |
| `encoder_receive` mean | 0.004 ms | `pipeline.scheduling.encoder_receive` |
| GPU raw_calibration mean | 2.054 ms | `pipeline.gpu.stages.raw_calibration` |
| GPU rcd_demosaic mean | 5.952 ms | `pipeline.gpu.stages.rcd_demosaic` |
| GPU camera_to_dwg mean | 0.694 ms | `pipeline.gpu.stages.camera_to_dwg` |
| GPU capture_sharpening mean | 0.635 ms | `pipeline.gpu.stages.capture_sharpening` |
| GPU davinci_intermediate mean | 0.474 ms | `pipeline.gpu.stages.davinci_intermediate` |
| GPU rgb_to_yuv_422 mean | 0.621 ms | `pipeline.gpu.stages.rgb_to_yuv_422` |
| **GPU processing sum** | **10.43 ms/frame** | (sum of above) |
| Job queue backpressure | 796 of 826 frames waited, 17,740 ms total | `pipeline.queues.job_backpressure_*` |
| Packet queue max depth | 1 of 16 (never backs up) | `pipeline.queues.packet_max_depth` |
| Slot backpressure | 3 waits, 11.5 ms total (negligible) | `pipeline.queues.slot_backpressure_*` |
| U16 RAW upload | 25.2 MB/frame, no RGB upload, no readback | `pipeline.transfers` |

Interpretation:
- Producers (RAW decode, 20.9 ms/frame overlapped across 8 tasks) are **always ahead** — the
  job queue is full 96% of the time. CPU decode is NOT the bottleneck (consistent with
  `docs/GPU_STAGE3F_E2E_BENCHMARK.md` Batch E note and commit `a97afbf`).
- Downstream mux never backs up.
- The single pacing element is the encoder-send thread.

### 2.2 Encoder-only benchmark (`docs/PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md`)

| Parallel processes | Throughput | GPU util mean |
|---:|---:|---:|
| 1 | 45.16 fps (22.14 ms/frame) | 53.7% |
| 8 | 56.61 fps (17.66 ms/frame) | 75.8% |

Derived: encode GPU-busy ≈ 0.758 × 17.66 ≈ **13.4 ms/frame**; the remaining ~9 ms at 1 stream
is synchronous submit→wait→download latency. Note this benchmark ran `async_depth=1` —
which, per finding 3.1, is the only value the encoder can actually run at.

### 2.3 Full pipeline does not scale with processes (`benchmark-report.json`)

| Concurrency | Aggregate fps | GPU util | VRAM peak |
|---:|---:|---:|---:|
| 1 | 32.75 | 88.9% | 3.3 GB |
| 2 | 30.02 | 93.8% | 6.0 GB |
| 4 | 28.63 | 95.6% | 11.2 GB |

Unlike encoder-only (45→56 fps), the full pipeline **loses** throughput with more processes:
the GPU is already saturated at concurrency 1, so extra processes only add WDDM context
switching and VRAM pressure (11.2 GB is near the 3060's 12 GB limit).
**Conclusion: multi-process is the wrong axis. Do not pursue it.**

### 2.4 Arithmetic identity

10.43 ms (processing, measured GPU timestamps)
+ ~13.4 ms (encode GPU-busy, derived from encoder-only bench)
+ ~5 ms (sync bubbles: fence wait wakeup, packet download submit+wait, header CPU work)
≈ 29.6 ms measured wall per frame. The E2E number is not mysterious — it is the **sum** of
the stages plus synchronization overhead, because nothing overlaps.

## 3. Root causes (code-level)

### 3.1 Encoder `async_depth` is silently forced to 1 (FFmpeg fork)

File: `$FFSRC/libavcodec/proresenc_kostya_vulkan.c`

- Line ~871: `ff_vk_exec_pool_init(vkctx, pv->qf, &pv->e, 1, 0, 0, 0, NULL)` — the compute
  exec pool is **hardcoded to 1 context** (compare upstream `ffv1enc_vulkan`, which passes
  the user's `async_depth`).
- Line ~893: `pv->async_depth = pv->e.pool_size;` — the user option (set to 8 by
  `src/output/vulkan_prores_encoder.cpp:81-83`) is **overwritten to 1**.
- Line ~818 in `vulkan_encode_prores_receive_packet`: after submitting frame N,
  `if (pv->in_flight < pv->async_depth) return AVERROR(EAGAIN);` — with `async_depth==1`
  this is false, so the loop continues, `ff_vk_exec_get` returns the same (only) exec
  context, `exec->had_submission` is true, and `get_packet` runs:
  - `ff_vk_exec_wait` (line ~676): **CPU blocks until frame N's encode finishes on GPU**.
  - Then the packet download: a transfer submit + a second `ff_vk_exec_wait` (line ~755),
    or a host memcpy on the fallback path.
- `$FFSRC/libavcodec/encode.c` line ~513: `avcodec_send_frame` **eagerly drives
  `encode_receive_packet_internal`** to fill the internal packet buffer. This is why the
  entire synchronous cycle shows up in the app's `encoder_send` timing (26.9 ms), while
  `encoder_receive` measures ~0 (it just pops the already-buffered packet).

Net effect: encode of frame N+1 can never start before frame N's encode is complete and
downloaded. The encoder pipelines nothing, regardless of configuration.

### 3.2 Processing chain and encoder share one hardware VkQueue

- App frame writer: `src/vulkan/vulkan_rgb_to_yuv_frame.cpp:247-248` —
  `queue_family = runtime.compute_queue_family(); vkGetDeviceQueue(device, queue_family, 0, &queue);`
  → **queue index 0**.
- App family selection: `src/vulkan/vulkan_runtime.cpp:294-301` — picks the **first** entry
  in `AVVulkanDeviceContext.qf[]` with `VK_QUEUE_COMPUTE_BIT`.
- FFmpeg encoder family selection: `$FFSRC/libavutil/vulkan.c` `ff_vk_qf_find` (line ~286)
  — also picks the **first** family with `VK_QUEUE_COMPUTE_BIT` from the same `qf[]` array.
- FFmpeg exec queue index: `$FFSRC/libavutil/vulkan.c:508` — `e->qi = i % qf->num`; with
  pool size 1, `qi = 0`.

Same array, same predicate, same index → **identical `VkQueue`**. Both command streams are
barrier-heavy (the app records calibration + 8 barrier-separated RCD passes + color +
sharpen + DI + YUV into one command buffer per frame,
`src/vulkan/vulkan_rgb_to_yuv_frame.cpp:814-904` and `1250-1478`; the encoder has
DCT→estimate→trellis→encode_slice barriers). On a single in-order queue those barriers gate
everything behind them, so processing N+1 executes essentially serially with encode N.
The 89% GPU util is the queue being busy *serially*, not parallelism.
Note: `VulkanRuntime::compute_queue_count()` (`src/vulkan/vulkan_runtime.cpp:336`) is
recorded but never used anywhere.

### 3.3 Calibration pass reads RAW over PCIe inside the shader

- `src/vulkan/vulkan_rgb_to_yuv_frame.cpp:334` — `slot.raw_upload` is created with default
  properties `HOST_VISIBLE | HOST_COHERENT` (see `create_buffer` defaults at line ~282).
- `upload_raw` (line ~799) memcpys 25.2 MB into it; the calibrate shader then reads that
  host memory directly across PCIe.
- Result: `raw_calibration` costs **2.05 ms** of compute-queue-serialized GPU time for a
  trivial normalize (a device-local read would be ~0.3 ms; 25 MB at ~13 GB/s PCIe ≈ 1.9 ms).
- A staging copy on a transfer queue would overlap with compute and hide this entirely.

### 3.4 Secondary (not currently limiting, will be after fixes 1–2)

- Writer resident slots hard-capped at 2: `src/output/ffmpeg_writer.cpp:142`
  (`slot_count = std::min<std::size_t>(encoder_depth, 2U)`), prepared-frame queue capacity
  also 2 (`vulkan_frame_capacity = slot_count`, line 147). Today slot backpressure is ~0
  because the encoder is the choke point; a deeper async encoder will need deeper slots.
- Thread topology (`src/output/ffmpeg_writer.cpp:975-1109`): producers → job queue (10) →
  `vulkan_prepare_thread` (pack, submits all compute) → prepared queue (2) →
  `vulkan_gpu_thread` (misnamed: it is the **encoder-send** thread; `process_vulkan_frame`
  at line 901) → packet queue (16) → `vulkan_packet_thread` (mux). Sound design; the send
  thread is simply starved of async capacity by 3.1.
- RCD demosaic is 57% of processing GPU time (5.95 ms, 8 full-image passes with full
  barriers between passes). Optimization is real but a separate project.

## 4. Fix plan (ranked; each step is independently verifiable)

> Rule: after each step, rerun the same benchmark
> (`test-output/full-file-parallel-benchmark/benchmark.ps1`, or
> `scripts/benchmark-gpu-stage0.ps1 -ValidateStage2Raw -Config config/vulkan-fast.json`)
> and compare sidecar telemetry, not just fps. Bit-exactness gates from
> `GPU_STAGE3F_E2E_BENCHMARK.md` still apply (no quality change is expected from any step).

### Fix 1 — Make the encoder actually asynchronous (largest win, do first)

Two alternative implementations; pick ONE:

- **1a (FFmpeg fork patch, preferred):** in `proresenc_kostya_vulkan.c`, pass the user
  `async_depth` to `ff_vk_exec_pool_init` for the compute pool (mirror upstream
  `ffv1enc_vulkan`), and delete/adjust the `pv->async_depth = pv->e.pool_size` overwrite so
  the option round-trips. The vendored port lives at `cmake/vcpkg-ports/ffmpeg/` (currently
  a passthrough; a patch file would need to be added there so the fix survives rebuilds).
  Risk: touching the fork; rebuild via vcpkg is slow.
- **1b (app-only, no FFmpeg change):** round-robin 2–3 `VulkanProResEncoder` instances in
  `src/output/ffmpeg_writer.cpp`, exactly mirroring the existing CPU-path design
  (`ffmpeg_writer.cpp:175-181` already does multi-context `prores_ks` with in-order muxing;
  ProRes is intra-only so this is safe). Each instance stays synchronous internally, but
  their submit/wait cycles interleave so the GPU queue stays fed.

Acceptance: `pipeline.scheduling.encoder_send.mean_ms` drops from ~27 ms to
(1a) low single digits or (1b) ~27/N ms effective; E2E ≥ ~40 fps; `direct_frames == frame
count`, `readback_frames == 0` unchanged.

### Fix 2 — Separate hardware queues for processing vs encode

In `src/vulkan/vulkan_rgb_to_yuv_frame.cpp:248`, take a queue index other than 0 (the
family's `compute_queue_count()` is already exposed), or a second compute-capable family
from `qf[]`. Keep the FFmpeg queue lock pair matched to the new (family, index)
(`lock_ffmpeg_queue` calls at lines ~1499/770 currently hardcode index 0 semantics).
Cross-queue ordering per frame is already handled by the existing AVVkFrame timeline
semaphores (wait `sem_value`, signal `sem_value+1`, lines ~1480-1494).

Acceptance: with Fix 1 in place, E2E approaches the encoder-only ceiling: **~50–56 fps**;
GPU util stays ≥ 90% while `encoder_send` no longer tracks (processing + encode) sum.
Without Fix 1, this step alone yields little — do not evaluate it in isolation.

### Fix 3 — Stage RAW upload to device-local memory via transfer queue

Replace direct shader reads of the HOST_VISIBLE `raw_upload` buffer with: host memcpy →
staging ring → `vkCmdCopyBuffer` on a transfer queue → DEVICE_LOCAL raw buffer, overlapped
with the previous frame's compute (semaphore into the compute submit).

Acceptance: `pipeline.gpu.stages.raw_calibration.mean_ms` drops from ~2.05 to ≤ 0.5 ms;
`u16_raw_upload_bytes` unchanged; output bit-exact.

### Fix 4 — Raise writer slot/queue depths (only after Fix 1)

Revisit the `min(encoder_depth, 2)` cap and `vulkan_frame_capacity` in
`src/output/ffmpeg_writer.cpp:137-149`. Size so that `slot_backpressure_waits` stays near 0
at the new throughput. Watch VRAM: each slot is ~475 MB at 4096x3072
(FP32 input/scratch buffer sets, `vulkan_rgb_to_yuv_frame.cpp:315-346`).

### Fix 5 (later, separate project) — RCD demosaic pass fusion

5.95 ms across 8 barrier-separated full-image passes; shared-memory tiling / pass fusion
could reclaim 2–4 ms. Only worth doing after fixes 1–3, when processing time is again a
visible fraction of the frame budget.

### Explicit non-goals

- **Do not** add more parallel *processes* per GPU (Section 2.3: it reduces throughput).
- **Do not** start GPU MCRAW decompression for throughput reasons — decode is overlapped
  and not limiting (Section 2.1; also `docs/GPU_STAGE3F_E2E_BENCHMARK.md` Batch E).
- No precision/quality trades are needed for any fix above.

## 5. Expected outcomes

| State | Per-frame wall | fps |
|---|---:|---:|
| Today | 29.6 ms | 33 |
| + Fix 1 (async encode) | ~24–25 ms (GPU-busy-bound) | ~40–42 |
| + Fix 2 (queue separation) | ~17–20 ms (encoder-bound) | ~50–56 |
| + Fix 3 (staged upload) | frees 1.7 ms of queue time | helps reach the above |
| Hard ceiling (this GPU/driver) | encoder shaders themselves | ~56 |

## Appendix A — Re-verification commands (PowerShell, repo root)

```powershell
# Encoder pool hardcoded to 1 / async_depth overwrite / blocking waits
Select-String -Path .deps/vcpkg/buildtrees/ffmpeg/src/n8.1.2-114db953a4.clean/libavcodec/proresenc_kostya_vulkan.c `
  -Pattern 'ff_vk_exec_pool_init|async_depth = |in_flight < |ff_vk_exec_wait'

# send_frame drives receive internally
Select-String -Path .deps/vcpkg/buildtrees/ffmpeg/src/n8.1.2-114db953a4.clean/libavcodec/encode.c `
  -Pattern 'encode_receive_packet_internal'

# Queue index assignment in FFmpeg exec pool
Select-String -Path .deps/vcpkg/buildtrees/ffmpeg/src/n8.1.2-114db953a4.clean/libavutil/vulkan.c `
  -Pattern 'e->qi = |ff_vk_qf_find'

# App-side: queue 0 of first compute family; host-visible raw upload; slot cap
Select-String -Path src/vulkan/vulkan_rgb_to_yuv_frame.cpp -Pattern 'vkGetDeviceQueue|raw_upload = create_buffer'
Select-String -Path src/vulkan/vulkan_runtime.cpp -Pattern 'VK_QUEUE_COMPUTE_BIT'
Select-String -Path src/output/ffmpeg_writer.cpp -Pattern 'slot_count = |vulkan_frame_capacity'

# Telemetry ground truth
Get-Content test-output/full-file-parallel-benchmark/c1-official-1/input-1.mov.json |
  ConvertFrom-Json | Select-Object -ExpandProperty pipeline |
  Select-Object -ExpandProperty scheduling
```
