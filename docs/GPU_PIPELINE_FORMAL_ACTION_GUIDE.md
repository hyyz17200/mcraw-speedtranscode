# GPU Pipeline Official Action Guide for the Next Phase

Date: 2026-07-14; Latest revision: 2026-07-15
Nature: General outline of implementation and acceptance
Scope: Stage 0–3 / Batches A–D are complete. The GPU pipeline has been moved to the U16 RAW mosaic entry point. This guide covers cleanup of confirmed user-visible overhead, validation of Vulkan ProRes async/queue-serialization candidates, and freezing the GPU pipeline boundary at the U16 RAW entry; MCRAW compressed payload remains CPU-decoded.

This article synthesizes the following status documents and converts the existing analysis into executable stages, deliverables, acceptance thresholds, and stopping conditions:

- `GPU_PIPELINE_SUMMARY_AND_NEXT_STEPS.md`
- `GPU_PHASE1_VALIDATION.md` ~ `GPU_PHASE8_PRODUCTION.md`
- `GPU_PIPELINE_AUDIT.md`
- `VULKAN_PRORES_GPU_PIPELINE_GUIDE.md`
- `GPU_STAGE3F_E2E_BENCHMARK.md`
- `PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md`
- `GPU_PIPELINE_SERIALIZATION_ANALYSIS_2026-07-15.md`
- `test-output/full-file-parallel-benchmark/benchmark-report.json`
- `implementation-status.md`

This document does not change the existing CPU reference, Vulkan opt-in policy, or release thresholds. If subsequent implementation conflicts with it, update this document or record an ADR first; do not silently change the goal in code.

---

## 1. Current baseline and formal conclusions

### 1.1 Completed capabilities

The current Vulkan path is complete:

```text
CPU official RAW decode
  -> U16 RAW mosaic upload
  -> Vulkan calibration / RCD demosaic
  -> Vulkan Camera RGB/DWG/sharpening/DI
  -> Vulkan RGB-to-YUV 4:2:2 10-bit
  -> FFmpeg-owned AVVkFrame
  -> prores_ks_vulkan
  -> compressed packet
  -> CPU MOV/audio mux
```

Proven engineering foundations include:

- FFmpeg-owned single Vulkan logical device;
- The application compute and `prores_ks_vulkan` share this device;
- The shader writes directly to the FFmpeg Vulkan frame pool;
- timeline semaphore and correct `AVVkFrame` ownership;
- Bounded frame job queue, GPU slots, packet queue and independent mux worker;
- normal path None `vkQueueWaitIdle()` / `vkDeviceWaitIdle()`;
- backpressure, cancellation, exception propagation, device lost and partial file cleaning;
- `auto` full-size preflight/fallback with forced Vulkan fail semantics;
- 240 frames 4096×3072 ProRes HQ + PCM full output and software full stream decoding.

### 1.2 Current Performance Facts

On RTX 3060 / NVIDIA 610.62, 4096×3072 ProRes 422 HQ baseline on 2026-07-15:

| Project | Results |
|---|---:|
| 240 frames precise full conversion | 34.776 fps |
| 240 frames fast full conversion | 36.857 fps |
| GPU processing-only microbenchmark | 96.052 fps / 10.41 ms/frame |
| GPU processing + Vulkan ProRes microbenchmark | 35.880 fps / 27.87 ms/frame |
| encoder-only, 1 process | 45.16 fps / 22.14 ms/frame |
| encoder-only, 8 processes aggregate | 56.61 fps / 17.66 ms/frame |
| Full application, 1/2/4 parallel processes aggregate | 32.75 / 30.02 / 28.63 fps |

10.41 ms for processing-only combined with 17.66 ms for the best encoder-only aggregate, predicted
35.62 fps; actual combined microbenchmark is 35.88 fps. The input content of the two benchmarks and
The upload paths are not exactly the same, so this equation doesn't prove that there aren't any bubbles inside, but it clearly states: **Existing
The evidence does not show significant additional steady-state pipeline losses. The current main costs are image processing and
ProRes encoding performs efficient work on the same GPU instead of duplicating uncompressed data copying, mux, queue lock
or CPU decode starvation. **

About 26.88 ms of `encoder_send` is the thread blocking position, not pure encoder shader time. it may contain both
Input frame semaphore pending processing, ProRes GPU execution, packet transfer and FFmpeg
Internal packet production. It is forbidden to directly interpret the wall timer as an independent encoder cost, and it is also forbidden to use
The device-level utilization of `nvidia-smi` is multiplied by wall time to derive the so-called GPU-busy shader time.

### 1.3 The exact caliber of zero-copy

What has been implemented currently is:

```text
CPU U16 RAW -> one U16 upload -> Vulkan full image chain
  -> FFmpeg AVVkFrame -> Vulkan ProRes -> compressed packet download
```

The production path does not have FP16/FP32 RGB upload, uncompressed YUV upload or pixel/YUV readback, so telemetry
`gpu_resident=true`, `direct_frames=N`, `upload_frames=0`, `readback_frames=0`
Established under the established definition.

That's not literally zero transfers: there's still a ~25.2 MB U16 RAW host-to-device upload per frame, encoded
The compressed bitstream must still be returned to the CPU mux. These two boundary transfers are a necessary cost of the current architecture; without
Before matching profiler evidence, they are not classified as duplicate copies. The official caliber is:

> Upload the smallest possible source data to the discrete GPU only once per frame; all large uncompressed intermediate images stay in VRAM;
> Just let the compressed packet go back to the CPU mux after encoding.

---

## 2. Formal goals for the next phase

### 2.1 Main goal

The Vulkan pipeline entry has been moved forward in the following order:

```text
TargetLog FP32 RGB (Historical Entry)
  ->Camera RGB FP32
  -> U16 RAW mosaic (current and final GPU pipeline entry)
```

Projects no longer target the GPU MCRAW decoder. compression 6/7 decoding continues on CPU, Batch E
Responsible for completing official truth, scheduling, memory and necessary CPU fast paths; subsequent profiler results are no longer automatic
Trigger GPU decoder work. If this direction is proposed again in the future, a new outline or ADR must be established.

### 2.2 Performance Goals

The project has identified a minimum goal and an extended goal:

- Minimum product target: 4096×3072 ProRes HQ stably reaches the real-time frame rate of the source material;
- First clear benchmark: ≥24 fps;
- Stretch target: ≥30 fps;
- Do not replace the full sample results with the instantaneous FPS of short frame smoke;
- Each stage reports at least end-to-end, each producer stage, GPU timestamp, and CPU/GPU at the same time
  utilization, PCIe upload, VRAM peak, and disk writes.

24/30 fps is an action goal, not an upfront performance promise for the RTX 3060. If the hardware compute, memory bandwidth or
`prores_ks_vulkan` has become a new bottleneck under the full GPU producer, and the target or scheme should be updated with the profiler.

### 2.3 Invariant constraints

- The CPU pipeline maintains reference, default, and fallback, and prohibits rewriting its stable semantics for GPU optimization;
- Disable running-time switching of CPU encoder from Vulkan in the same MOV;
- Only publish final files after drain, trailer, close and light reopen/metadata validation succeed; complete
  Packet-by-packet scanning remains as an explicit verify/release/test gate and no longer counts towards normal conversion E2E by default;
- Behavioral changes to precision, demosaic algorithms, metadata or chroma sitting must be explicitly versioned;
- Each new GPU stage must be able to independently golden test, performance test and fault injection;
- Vulkan remains opt-in until release gates are completed.

---

## 3. Precision strategy: Precise and Fast splitting

Performance optimization and semantic validation cannot be mixed into an indistinguishable backend. It is recommended to formally establish two modes:

### 3.1 `fp32/precise`

Purpose: GPU production reference, regression to baseline, and stage-by-stage migration.

- shader images and key calculations use FP32;
- Each frame color calculation continues to be completed by CPU FP64, and only the final matrix/parameters are uploaded;
- Keep the existing negative policy, sharpening, chroma filter, rounding, clamp and dither;
- Comparison by stage before final quantification/after quantification;
- The final maximum difference between Y/Cb/Cr and CPU reference remains ≤1 LSB;
- Non-limited numbers and illegal metadata will continue to explicitly report errors.

### 3.2 `mixed/fast` (verification design must be completed before adding)

Purpose: After the precise pipeline is correct, explicit image quality budget is exchanged for throughput and bandwidth.

Suggested candidate strategies:

- RGB/intermediate image uses FP16;
- Matrix parameters, key accumulation, DI shadows and final YUV quantization remain FP32;
- Allow FMA, different order of operations, non-bit-exact dither;
- DI can evaluate texture LUT + interpolation or shader analytic evaluation;
- Fast mode cannot be silently substituted for precise mode.

It is recommended to use the following values as the **initial experiment budget**, and then freeze the formal threshold after fixing the corpus measurement:

| Indicators | Precise | Fast Candidates |
|---|---:|---:|
| Final 10-bit max error | ≤1 LSB | ≤8 LSB |
| Final 10-bit P99 error | ≤1 LSB | ≤2 LSB |
| Final 10-bit RMSE | Record | ≤1.0 LSB |

These thresholds are not a substitute for image inspection. Must additionally cover shadows, super-white, negative toe, saturated colors, thin lines,
Bevel edges, moiré and high frequency chroma. If clipping boundaries lead to a small number of outliers, the coordinates and reasons should be recorded.
Only expanding the max-error threshold masks the problem.

### 3.3 demosaic separate acceptance

demosaic cannot pass final ProRes PSNR acceptance alone. Both must be compared:

- Linear Camera RGB;
- CFA boundaries and four Bayer patterns;
- Black point, white point, bad pixel neighborhood and image edge;
- High frequency slashes, colored moiré and zipper artifacts;
- max/RMSE/P50/P95/P99 and abnormal pixel coordinates;
- Final DWG/DI/YUV and decoding ProRes impact.

If the GPU RCD is an approximate implementation, it must be exposed under a new name/mode and cannot be claimed to be identical to the librtprocess RCD.

---

## 4. Phased implementation route

## Stage 0: Freeze reproducible baseline and profiler contracts

### Target

Before changing the pipeline split point, establish a baseline of facts that will be used by all subsequent stages.

### Work item

1. Fixed current commit, FFmpeg build, driver, configuration and sample hash.
2. Create a fixed corpus containing at least the following:
   - Current 4096×3072, 240 frames real sample;
   - First frame, last frame and fixed middle frame;
   - Small synthetic/golden for four CFA patterns;
   - Shadows, super-white, negative values, saturated colors and high-frequency edge patterns.
3. Save phase-by-phase hash/statistics: RAW, calibrated mosaic, Camera RGB, TargetLinear,
   TargetLog, YUV planes, packet/frame count, PTS and ffprobe JSON.
4. Add GPU timestamp query to Vulkan stages; CPU wall timer cannot pretend to be GPU execution time.
5. Telemetry clearly distinguishes:
   - compressed input read bytes;
   - U16 RAW upload;
   - FP16/FP32 RGB upload;
   - GPU image-to-image bytes are not declared as PCIe bytes;
   - compressed packet download/mux bytes.
6. Record CPU%, GPU compute utilization, VRAM peak, PCIe, disk throughput for complete conversion.

### Acceptance threshold

- frame count, PTS, RAW/YUV golden and telemetry invariant stable for repeated runs of the same build;
- The benchmark has at least one warm-up, three official runs, and reports median and spread;
- Validation layer normal validation without application errors;
- Existing Phase 8 output behavior is not regressed.

### Stop condition

If the baseline repeatedly fluctuates enough to cover up the expected optimization range, correct the benchmark or telemetry first without entering Stage 1.

---

## Stage 1: Full GPU after Camera RGB

### Target

Eliminate the current CPU TargetLog producer of about 511 ms/frame, and move the upload splitting point forward from TargetLog RGB
Camera RGB.

### Target data flow

```text
CPU decode/calibration/RCD
  -> Camera RGB FP32 staging
  -> Vulkan camera matrix + exposure
  -> Vulkan neutral capture sharpening
  -> Vulkan DaVinci Intermediate
  -> Vulkan RGB-to-YUV 4:2:2 10-bit
  -> existing AVVkFrame/prores_ks_vulkan
```

### Implementation principles

- CPU retains FP64 for CameraNeutral, dual illuminant, ForwardMatrix, Bradford and DWG matrix
  setup; only upload the final matrix, exposure and policy parameters as uniform/push data;
- In the first version, each logical stage has an independent golden test; after it is correct, the profiler will decide pass fusion;
- Neighborhood semantics and boundary rules must be maintained when sharpening uses tile/shared memory;
- The first version of DI uses FP32 precise as the baseline and does not introduce FP16 at the same time in the same submission;
- Directly reuse existing FFmpeg frames, timeline semaphore, slot and encoder handoff.

### Deliverables

- Vulkan color/exposure pass;
- Vulkan sharpening pass;
- Vulkan DI pass;
- Camera RGB staging writer;
- stage-level golden tests and full E2E regression;
- New and old paths matched benchmark and GPU timestamp breakdown;
- sidecar adds actual pipeline entry and precision tags.

### Acceptance threshold

- precise mode final YUV ≤1 LSB;
- All existing audio, PTS, MOV cleanup, fallback and device-lost tests continue to pass;
- Does not increase CPU readback;
- The GPU queue should continue to get work more continuously than it currently does;
- There must be repeatable improvements in full sample end-to-end performance. It is recommended that the lower limit of go/no-go is ≥20%, otherwise profile first.
  Do not continue to blindly fuse shaders.

### Stage decision-making

- If the color chain becomes a GPU hotspot: first analyze occupancy, bandwidth, inter-pass layout and fusion;
- If demosaic clearly becomes the absolute main bottleneck: freeze the correct version of Stage 1 and enter Stage 2;
- If PCIe Camera RGB upload becomes a hotspot: Stage 2 priority is further increased.

---

## Stage 2: U16 RAW upload + GPU calibration + GPU demosaic

### Target

Reduces upload size per frame from ~151 MB FP32 RGB to ~25.2 MB U16 CFA and eliminates CPU calibration and
RCD demosaic hotspot.

### Target data flow

```text
CPU official MCRAW decode
  -> U16 RAW mosaic staging upload
  -> Vulkan black/white calibration
  -> Vulkan RCD demosaic
  -> Stage 1 GPU color/sharpen/DI/YUV
  -> Vulkan ProRes
```

### Substage

1. **2A: U16 upload and calibration shader**
   - Four CFA positions independent black/white;
   - Do not clamp negative/super-white in advance;
   - First output the test image that can be read back, and only readback in the test path.
2. **2B：GPU RCD precise prototype**
   - Correct first, then optimize tile/shared-memory;
   - Independent verification of four Bayer patterns and edge processing;
   - Not first launched at the same time as calibration fusion.
3. **2C: GPU-resident concatenation and fusion**
   - production path prohibits intermediate readback;
   - Profiler proves the benefits before integrating calibration/RCD or subsequent color pass;
   - Transfer queue and compute overlap are only used when supported by the actual queue family and profiler.

### Deliverables

- U16 RAW Vulkan input type/ownership;
- calibration compute shader;
- GPU RCD precise implementation;
- stage readback test harness (test only);
- demosaic quality corpus/report;
- U16 upload bytes, GPU stage timestamp, VRAM peak and E2E benchmark.

### Acceptance threshold

- FP32 RGB upload in production telemetry must be 0;
- The production path does not allow calibrated/Camera RGB readback;
- U16 RAW upload byte accounting accurate;
- precise demosaic meets officially approved tolerances and has no structural artifacts;
- Finally YUV, MOV, audio and error handling continue to meet Stage 1 standards;
- Full sample has repeatable performance gains over Stage 1.

### Stop condition

- If the GPU RCD quality cannot reach the precise threshold, retain the CPU RCD path and limit the approximation algorithm to fast
  mode; cannot lower the precise nominal standard;
- If Vulkan ProRes and the full GPU image pipeline start competing for compute, you must re-profile
  Queue overlap, async depth and shader/encoder ratio determine the optimization direction.

---

## Stage 3: mixed precision and fast mode

### Preconditions

- The FP32 precise pipeline of Stage 1/2 is correct, stable and has complete golden;
- The bandwidth, occupation, VRAM and compute distribution of the GPU stage have been measured;
- Be able to demonstrate that FP16/fast optimizations target real bottlenecks, rather than just reducing theoretical bytes.

### Work item

Evaluate in order changing only one variable at a time:

1. FP16 intermediate storage, FP32 accumulation;
2. FP16 color/sharpen part calculation;
3. texture DI LUT / analytic DI;
4. Non-bit-exact GPU dither;
5. Independent selection of precise RCD and fast demosaic.

### Acceptance threshold

- A/B output, numerical reports, visual corpus and performance delta for each variable;
- If the end-to-end revenue of a single item is lower than the noise or introduces unacceptable artifacts, the item will be revoked;
- preset/sidecar must clearly document precision, demosaic implementation, dither and DI mode;
- fast output cannot use an indistinguishable configuration identity from precise output.

---

## Stage 3G / Batch D.1: GPU pipeline structural cleaning and controlled experiments

### Formal diagnosis

2026-07-15 serialization review confirms two code facts:

1. vendored FFmpeg’s `prores_ks_vulkan` compute exec pool is hard-coded to 1, and the user requested
   `async_depth` is overwritten to the actual pool size 1; the current sidecar record is requested depth, not effective
   depth;
2. Both the application processing command and the single context encoder currently use the queue of the first compute family.
   index 0, so the GPU commands of both are executed sequentially on the same Vulkan queue.

These are structural constraints that need fixing or experimentation, but the current benchmark **does not demonstrate** that they result in recoverable 40~56
Huge fps notch. Multiple Vulkan queues still share the SM, cache and memory bandwidth of the same GPU; the second
Queue only provides concurrent scheduling opportunities and is not equal to the second hardware encoder. Any gains must be made by the same real frame, the same process,
Matched A/B proof of the same configuration.

### D.1-A: Remove confirmed meaningless wall-time overhead

1. Normal conversion no longer unconditionally scans the entire MOV just written within the E2E timer with `av_read_frame()`. Existing approx.
   The read-only retest of 1.40 GB / 240 frame output is about 0.67~0.81 seconds, accounting for about 10~12% of the 6.51 second complete conversion;
2. Keep lightweight reopen, stream/co…2716 tokens truncated…tively:

- Memory payload → pure decoder-only for U16;
- `loadFrame` for real file reading, metadata JSON parse, decode and output buffer;
- Complete precise/fast Vulkan pipeline;
- compression 6 and 7;
- frame workers 1/2/4/6/8, and a combination of intra-frame threads subject to a unified budget.

Each group runs at least three long samples after warm-up, reporting P50/P90/P95/P99, MP/s, CPU
core-equivalent, working set, compressed input GB/s, producer wait, queue depth, downstream
backpressure and end-to-end FPS. Capacity and resource costs continue to be reported for 90/120/130 fps, but these are capacity bins,
This is not a product promise that all 8-core machines must meet. 2 ms/frame for 4096×2160 is exploratory only
stretch result, shall not replace the project's official threshold of 4096×3072 corpus.

### E-D: compression 7 fast decoder in the warehouse (conditional implementation)

Only if E-C proves that the updated official decoder is still worth optimizing, new modules will be implemented in the following order; no in-place modifications are allowed
FetchContent source:

1. **M0: benchmark, golden, synthetic encoder and bit-width histogram. ** Stable reproduction first
   official output and real performance;
2. **M1: scratch reuse and direct writing U16. ** Delete `p0..p3 -> row vectors -> memcpy` intermediate traffic,
   Maintain single thread and original unpack semantics;
3. **M2: block offset table and complete input verification. ** After bits metadata is solved, use 64-bit prefix and build absolute
   offset, verify the final range, and then allow block/band to be scheduled independently;
4. **M3: 4-row band intra-frame parallelism. ** Use OpenMP or existing persistent executor, tuned by band grain; production
   Whether the default value is greater than 1 thread is determined by the complete pipeline benchmark under a unified budget;
5. **M4: independent ISA translation units. ** scalar/SSE4.1/AVX2 are compiled separately and detected at runtime; AVX2
   Must not be applied to the entire binary. Increase the kernel one by one according to the real bit-width histogram and test the lane separately.
   interleave, reference add, tail clipping and `vzeroupper`;
6. **M5: opt-in integration, fuzz and matched A/B. ** The new path can be activated first, and the official path is retained;
   Default switching is discussed only after all correctness, stability, and performance gates in this section have been met.

It is recommended that the module boundary be the warehouse's own `raw_decode` API: input immutable payload, clear compression type,
dimensions, writable U16 span, options and external reuse scratch; returns structured error code and metadata/offset/
payload/total timings. Disable exceptions as normal error branching, hot-loop allocation, misaligned/endian assumptions, or let
The entire `mcraw_core` depends on AVX2.

### E-E: compression 6 optimization strategy

compression 6 first only accesses the updated official legacy decoder and bit-exact tests. only reality
Compression 6 corpus only establishes an independent milestone after proving it is a capacity or latency hotspot; its block offset, row-level
Parallel, SIMD and tail schemes must be re-derived from the legacy format. There are no real corpus, official golden and
Does not implement or declare compression 6 fast decoder when matched benchmark.

### E-F: Acceptance, default path and stop conditions

Non-relaxable correctness threshold:

- Every implementation, ISA and thread combination of compression 6/7 is bit-exact pixel-by-pixel with the corresponding official decoder U16;
- The output is independent of the number of workers, the number of threads in the frame and the scheduling order;
- Payload length, header, offset, size, padding, encoded width and output span all boundary verification;
- Corrupted input, cancellation and decoder exceptions cannot produce a disguised complete MOV;
- The metadata, PTS, audio, fallback, cleanup and sidecar behaviors of CPU/GPU backend are not returned.

Performance go/no-go: Single optimization must exceed noise in matched long samples and satisfy at least one: decoder-only
Median improvement ≥15%; reduce at least one frame worker in the same throughput bin; or significantly reduce working set/P95 and
There is no rollback end-to-end. If M1–M4 does not meet this threshold, retain the proven simpler version and do not pursue theoretical SIMD width.
Keep adding complexity.

The new decoder must also pass compression 6/7 full corpus, scalar/SSE/AVX2,
1/2/4/6/8 workers, cancel/corrupt input, long time and full precise/fast Vulkan A/B. Even if the fast path becomes
By default, the official decoder remains as the reference/diagnostic path. After Batch E is completed, the GPU pipeline boundary is officially
Freeze on U16 RAW upload; GPU decoder subsequent steps are no longer retained.

RAW decompression is bounded by integer semantics and source data correctness, and does not belong to the fast mode that allows image quality errors.

---

## 5. Performance measurement and go/no-go rules

The report of each stage uses a unified table:

| Category | Must Report |
|---|---|
| Build | commit, Release/Debug, FFmpeg commit/config, shader hash |
| Hardware | CPU, RAM, GPU, VRAM, driver, Vulkan API |
| Input | File hash, size, frame number, audio, configuration |
| CPU | total/process utilization, each CPU stage mean/P50/P95 |
| GPU | Each pass timestamp, queue busy/idle, encoder time, utilization |
| Transfer | compressed input read bytes, U16/RGB upload bytes and GB/s, readback bytes |
| Memory | system RAM peak, VRAM peak, slot/pool count |
| Queues | capacity, max depth, wait count/time |
| Output | conversion-core/process wall, validation wall, fps, packet bytes, disk MB/s, frame/PTS/audio validation |
| Quality | stage error, final YUV error, decoded comparison, manual conclusion |

Unified rules:

1. Use the same executable/config/input as matched A/B;
2. Warm-up first and run it at least three times;
3. Report median and min/max or standard deviation;
4. Short smoke only verifies correctness and is not used to promise final performance;
5. When the stage timer has parallel overlap, the mean cannot be added as wall time;
6. Prioritize the shader/producer/encoder bottleneck based on GPU timestamp and queue idle;
7. When there is no profiler evidence, fine-tuning of descriptor churn, queue quantity or mux buffer is not given priority;
8. API wall timers such as `encoder_send` only indicate blocking attribution and do not automatically equal the exclusive execution time of the stage;
9. It is forbidden to use device-level sampled GPU utilization × wall time to derive shader GPU-busy time;
10. The input content, upload, number of processes and timer boundary of isolated benchmark and combined benchmark must be
    Disclose differences consistently or clearly, and do not treat cross-workload derived values as acceptance facts;
11. Report startup/preflight, conversion core, output validation and process wall respectively;
12. Set a rollback point in each stage, and retain the last stable split point when the performance has not improved and the complexity has increased significantly.

---

## 6. Release gates: parallel to performance development but cannot be skipped

The following issues do not prevent experimental Stage 1/2 development, but do prevent the Vulkan backend from becoming the default production path:

1. [x] pinned FFmpeg ProRes DCT shader’s GPU-assisted validation race: Already running between the current driver and
   The validation layer reappears and records restricted waiver in `GPU_BATCH_F_VALIDATION_RACE_WAIVER.md`;
2. decode/seek/color/duration/ for DaVinci Resolve, Adobe Premiere and at least one desktop player
   Audio sync actual test; FFmpeg/VLC passed, Resolve failed due to external scripts being disabled, and Premiere was not installed.
   See `GPU_BATCH_F_COMPATIBILITY.md` for details;
3. [x] Frozen and verified chroma sitting and primaries/TRC metadata product decisions, both used by CPU/Vulkan
   Left sitting, DWG/DI has no standard MOV enumeration so primaries/TRC remain unspecified, see for details
   `GPU_BATCH_F_COLOR_METADATA.md`;
4. AMD, Intel Vulkan hardware and second-generation NVIDIA driver coverage; currently RTX 3060 / 610.62 has passed,
   This machine does not have an AMD/Intel physical device and the second generation NVIDIA driver is not installed. The missing lines remain blocked. For details, see
   `GPU_BATCH_F_HARDWARE_MATRIX.md`;
5. One hour of real material conversion; the current longest real sample is only about 27.5 seconds and has gone through two rounds, 3,600 logical seconds of synthesis
   The stress has passed but does not replace the real material, so this item remains blocked. For details, see `GPU_BATCH_F_STABILITY.md`;
6. Multi-file batch, repeated startup, cancellation, device lost and resource growth testing; multi-file/repeated startup/cancellation semantics/
   The resource growth has passed, but the real `VK_ERROR_DEVICE_LOST` has not yet been injected, so this item remains blocked;
7. [x] 24 fps has been confirmed in writing as the minimum product threshold; the final 240 frames precise is 37.189 fps, real batch
   It is 35.78~38.34 fps, which has passed the performance threshold.

The metadata/chroma decision must affect both the CPU and GPU backends. You cannot just modify the Vulkan output to cause reference
Forks.

---

## 7. Recommended execution batches

### Batch A: Establish a factual baseline first

- Complete Stage 0 corpus, GPU timestamp and unified benchmark reports;
- Also record and confirm the product semantics of precise/fast;
- Output: baseline manifest, benchmark report, quality budget decision.

### Batch B: Migrate the largest hot spots

- Implement Stage 1 GPU color/sharpen/DI;
- Pass golden first, then profile and selective fusion;
- Output: precise Vulkan pipeline for Camera RGB entry.

### Batch C: Reduce 6× uploads and eliminate demosaic hotspots

- Implement Stage 2A calibration;
- Implement Stage 2B GPU RCD;
- Concatenating Stage 1, the production path does not have readback;
- Output: precise Vulkan pipeline for U16 RAW entry.

### Batch D: Performance mode based on profiler

- Experimental FP16, DI, dither and fast demosaic one by one;
- Only include items that have measurable end-to-end benefits and pass quality budgets;
- Output: Unambiguously identifiable precise and fast presets.

### Batch D.1: Clean up acknowledgment overhead and verify GPU serialization assumptions

- First move the full MOV packet scan from normal E2E to explicit verify/release gate and eliminate duplicate Vulkan
  full-resolution preflight;
- Revise FFmpeg effective async depth and telemetry again, and verify the benefits with real frame single-process depth matrix;
- Only after the async evidence is established, try explicit queue reservation/separation; only memory-type and transfer
  Experiment with device-local RAW staging only after the timeline evidence is established;
- Output: separated timer boundary matched benchmark, requested/effective depth report, and each
  Go/no-go conclusion for serialization/staging candidate; no pre-commitment to 40-56 fps.

### Batch E: Complete CPU decoder

- Update and fix the official truth source that actually covers compression 6/7, fix RAW buffer over-allocation;
- Replace per-frame `std::async` with a persistent, bounded worker pool to establish a unified CPU thread budget;
- compression 6/7 split track build bit-exact golden, safety testing and matched benchmark;
- Implement compression 7 direct writing, offset, intra-parallelism and independence only if new official baseline proves beneficial
  ISA fast path; compression 6 only performs additional legacy optimizations when the real corpus proves necessary;
- Output: CPU decoder capacity, resource cost, default path decision at 90/120/130 fps bins, and frozen in
  Final GPU pipeline boundaries for U16 RAW upload.

### Batch F: Close production release gates

- Multi-software, multi-hardware, long time, batch and validation race;
- Discuss whether to make `backend=auto` or Vulkan a more aggressive default until clear product performance thresholds are reached.

---

## 8. Fixed requirements for the development agent for each implementation task

Each subsequent task should explicitly provide:

1. **Unique stage boundary**: Which input/output is only migrated this time;
2. **CPU reference**: corresponding functions, data types and frozen semantics;
3. **golden corpus**: Which fixed frames and synthetic cases to use;
4. **Allowable error**: bit-exact, ≤1 LSB or fast budget, "visual almost" is not allowed;
5. **ownership/sync**: buffer/image/AVFrame life cycle and semaphore contract;
6. **telemetry**: counter/timestamp that must be added or maintained this time;
7. **failure semantics**: fallback, forced Vulkan, device lost, partial cleanup;
8. **benchmark**: matched A/B conditions and go/no-go threshold;
9. **Non-target**: List the CPU, metadata, algorithms or configuration behaviors that are not modified this time;
10. **Definition of Done**: It is not complete until testing, documentation, sample verification and performance reporting are all met.

In the GPU stage, it is recommended that each task maintains "a new GPU semantic + a set of golden + a benchmark" and not in the same
New stages, FP16, algorithm approximation and pass fusion are also introduced in the task. The minimum change unit corresponding to Batch E is changed to
"A decoder/truth/scheduling semantics + bit-exact golden + matched benchmark" also prohibits using workers
Pool, format migration, intra-parallelism and the new ISA kernel fall into the same commit for the first time.

---

## 9. Next step is to execute the list immediately

Formal recommendations for action begin with the following sequence:

- [x] Approve this article as the general outline for the next phase of implementation;
- [x] Confirmed minimum performance target is 24 fps, with 30 fps as a stretch target;
- [x] Confirm that precise remains ≤1 LSB and fast uses independent preset/sidecar identity;
- [x] Execution Stage 0: freeze baseline manifest, quality corpus and GPU timestamp;
- [x] Write independent technical design for Stage 1, freeze Camera RGB input format, shader pass, uniform and
      golden boundary;
- [x] Implement and accept Stage 1 without simultaneously enabling FP16; Stage 1G median `13.791 fps`, relative
      Rebuild Stage 0 to `+100.943%`, passing `+20%` gate;
- [x] Use Stage 1 profiler to determine pass fusion and Stage 2 priorities; the root cause of fallback is CPU finite
      Full frame repeated scanning and pack/encoder serialization instead of shader fusion; after correction, the bottleneck returns to the CPU producer;
- [x] Implement Stage 2A/2B, switch production uploads to U16 RAW; Stage 2E median
      `37.747 fps`, relative to Stage 1G `+173.710%`, final YUV `<=1 LSB`;
- [x] precise starts after completion mixed/fast A/B; Batch D final precise/fast
      Median is `34.776/36.857 fps`; combined with FP16 intermediate storage and
      analytic DI, removes balanced presets not exposed as modes, rejects dither-off and
      bilinear demosaic of substandard quality;
- [x] Batch D.1-A: Remove full MOV packet scan from normal E2E, separate startup/conversion/validation/
      process timers, and eliminate duplication full-resolution Vulkan preflight;
- [x] Batch D.1-B: Fix and report FFmpeg requested/effective async depth, use real frame single process
      depth 1/2/4/8 does matched A/B; queue separation will not be implemented before the benefits are proven;
- [x] Batch D.1-C/D: Only after the pre-telemetry proves that idle or PCIe/heap problems can be recovered, experiment separately
      queue reservation and device-local RAW staging; record no-go if the noise is not exceeded;
- [x] Batch E-A: official truth updated to immutable commit that also covers compression 6/7, absorbed
      RAW buffer over-allocation repair, and confirm that the existing compression 7 first/middle/last frame U16 hash remains unchanged;
- [x] Batch E-B/E-C: implement persistent, bounded frame worker pool, compression 7 after completion of update
      decoder-only, loadFrame and full precise/fast Vulkan 1/2/4/6/8 worker matrix;
- [x] Batch E-D/E-E go/no-go: updated official compression 7 decoder has been exceeded
      90/120/130 fps capacity is divided into batches and is not a complete pipeline bottleneck. The self-developed fast decoder is no-go; no real
      compression 6 corpus, do not start legacy fast decoder;
- [x] Batch E-F compression 6 corpus gate: abandon material-level compression 6 as per 2026-07-16 project decision
      Test acceptance, recorded as explicit waiver; continue to try official legacy decoder at runtime, and ask for console
      Warning about unverified status; GPU pipeline boundaries remain frozen at U16 RAW upload;
- [x] Batch F has executed all NLE/hardware/long-term release gates allowed by the current host, software, hardware and real samples
      Action; local passed items, waiters, and blocked items that still require external environment are summarized in
      `GPU_BATCH_F_RELEASE_GATE_STATUS.md`, blocking entries are not pretended to pass.

Stage 0～3 / Batch A～D, Batch D.1-A～D and Batch E-A～E-F have been completed; D.1-C/D
Both queue/staging candidates and E-D/E-E fast decoder log no-go. compression 6 material level acceptance button
2026-07-16 Decision explicit exemption; the format continues to use the official legacy decoder and is marked as unverified with a console warning
status and shall not be described as having completed golden/safety/capacity acceptance for this project.

Current host actions for Batch F are complete: validation race limited waiver, FFmpeg/VLC compatibility, color metadata,
Evidence exists on current NVIDIA hardware, real multi-file duplication batches, 3,600 logical second resource growth, and 24 fps threshold.
Resolve/Premiere, AMD/Intel, 2nd generation NVIDIA driver, one hour of real footage and actual device-lost injection still blocking
Production release; therefore Vulkan continues to be opt-in and the CPU backend continues to be the default with fallback.
