# Vulkan ProRes GPU Pipeline Audit

Status: awaiting review; contains no GPU implementation code  
Audit date: 2026-07-14  
Scope: current CPU reference, FFmpeg/MOV output, threading and ownership, Vulkan integration boundary, and the Phase 1–3 file-level plan

## 1. Conclusion

The current CPU pipeline has the conditions required to serve as the GPU migration reference: data-type boundaries are clear, the color math has an analytic reference, the fused and reference paths differ by at most 1 LSB in 10-bit output, the real MCRAW sample is reproducible, and CPU multi-frame execution and ProRes muxing are bounded.

The GPU backend should not rewrite `CpuPipeline` or directly replace `FfmpegWriter`. First extract the existing FFmpeg encode/mux behavior into a CPU adapter while keeping the public interface unchanged, then add an independent Vulkan backend. Keep exactly one Vulkan logical device: **let FFmpeg create and own the device, and let the application borrow the same instance/device/queue through `AVVulkanDeviceContext`**. This matches the project's current fact that it has no existing Vulkan runtime and avoids cross-device copies later.

There is currently one hard blocker: although the FFmpeg 8.1.2 source linked by the repository contains `prores_ks_vulkan`, the vcpkg manifest does not enable the `vulkan` feature. Therefore, the current project libraries report:

```text
CONFIG_VULKAN=0
CONFIG_PRORES_KS_VULKAN_ENCODER=0
```

The system FFmpeg 8.1.2, RTX 3060, and Vulkan SDK passed a two-frame upload-bridge smoke test, proving that `yuv422p10le -> AV_PIX_FMT_VULKAN -> prores_ks_vulkan -> MOV` works on this machine.

## 2. Verified baseline

### 2.1 Development environment

| Item | Current state |
|---|---|
| OS | Windows 10/11 development environment |
| GPU | NVIDIA GeForce RTX 3060, 12 GiB |
| NVIDIA driver | 576.02 |
| GPU Vulkan API | 1.4.303 |
| Vulkan SDK | 1.4.350.0 |
| Validation layer | `VK_LAYER_KHRONOS_validation` available |
| Shader compiler | `glslc` / shaderc 2026.2 available |
| System FFmpeg | 8.1.2 full build with `prores_ks_vulkan` available |
| Project FFmpeg source | vcpkg pinned to 8.1.2 |
| Project FFmpeg binary | Vulkan is not currently enabled; rebuild required |
| Compiler | Visual Studio Build Tools 2022, MSVC x64 preset |

The system FFmpeg smoke test used 128×64, 2 frames, ProRes HQ, and `async_depth=2`. ffprobe read the output successfully: `apch`, `yuv422p10le`, video range, and 2 frames.

### 2.2 CPU reference

Current verification results:

- 18/18 unit tests passed;
- Fixed sample: `mcraw_sample/260710_142121_VIDEO_49mm.mcraw`;
- Sample dimensions: 4096×3072, 240 frames;
- First-frame RAW FNV-1a 64: `4534363536704555902`;
- First-frame compressed payload: 11,222,720 bytes;
- First-frame fused/reference comparison: 25,165,824 10-bit samples, 600 differences, maximum difference 1 LSB;
- Complete CPU output recorded: ProRes 422 HQ, 240 frames, 48 kHz stereo PCM, and full-stream FFmpeg decode passed;
- The repository's `test-output/` does not retain large MOV files. Before implementation formally begins, generate a small, committable reference manifest containing the command, Git commit, configuration, RAW hash, key-frame plane hashes, ffprobe JSON, and output hash. Large MOV files may remain as local test assets and need not be committed to Git.

## 3. Actual data flow of the current CPU pipeline

```text
MCRAW file
  |
  +-- independent frame index validation
  +-- MotionCam container/frame metadata
  +-- compressed type 6/7 payload
  v
motioncam::Decoder (one leased instance per concurrent frame)
  v
RawMosaicU16                         uint16 CFA mosaic
  v
black/white calibration             FP64 setup, FP32 output, preserve <0 and >1
  v
RawDemosaicF32                      planar-less CFA buffer in 0..65535 working scale
  v
librtprocess demosaic               RCD default; AMaZE/IGV/DCB/LMMSE optional
  v
CameraRgbF32                        3 planar FP32 channels, still scaled by 65535
  v
per-frame camera color solution     FP64 CameraNeutral/matrix setup
  v
fused production pack
  +-- camera RGB -> linear DWG
  +-- exposure offset
  +-- optional neutral capture sharpening
  +-- DaVinci Intermediate OETF LUT
  +-- encoded RGB -> BT.2020 NCL Y'CbCr
  +-- 5-tap left-sited 4:2:2 downsample
  +-- deterministic dither
  +-- 10-bit video-range clamp/quantize
  v
Yuv422P10                           3 CPU uint16 vectors
  v
FfmpegWriter
  +-- AVFrame / AV_PIX_FMT_YUV422P10LE
  +-- N independent prores_ks contexts
  +-- sequence-ordered AVPacket collection
  +-- PCM S16LE audio
  +-- av_interleaved_write_frame
  v
output.partial.mov
  v
flush -> trailer -> close -> atomic rename
  v
output.mov + sidecar JSON
```

## 4. Per-stage formats, ownership, and GPU insertion points

| Stage | Input | Output | Current ownership | Concurrency | GPU migration point |
|---|---|---|---|---|---|
| MCRAW index/read | file offsets | compressed payload/metadata | `McrawReader::Impl` owns the index; payload is an independent vector | reader-shared | Keep on CPU; do not move disk I/O to the GPU |
| Official decode | MCRAW compression 6/7 | `RawMosaicU16` | decoder lease + moved vector | independent decoder per in-flight frame | Keep on CPU initially; future Vulkan decode must match pixel for pixel |
| Calibration | U16 CFA | `RawDemosaicF32` | new FP32 vector | OpenMP rows | Shader A; suitable for later merge with unpack, but validate independently first |
| Demosaic | FP32 CFA, 0..65535 | `CameraRgbF32` | three new FP32 planes | librtprocess/OpenMP | Shader B; RCD is the first GPU parity target |
| Color solution | per-frame metadata | FP64 matrices/white point | small value object | CPU per frame | Keep CPU setup; upload the final matrix as a uniform |
| Fused color/pack | Camera RGB FP32 | `Yuv422P10` | new Y/Cb/Cr vectors | OpenMP row partitions | Split into verifiable shaders first; profiler evidence is required before final fusion |
| CPU ProRes | CPU planar 10-bit | packets | AVBufferRef owns moved vectors | multiple codec contexts | retain as reference/fallback |
| Vulkan ProRes | Vulkan 10-bit 4:2:2 | packets | AVFrame/AVBufferRef owns AVVkFrame | `async_depth` | Phase 3 uses upload first; Phase 5 writes directly from the shader |
| MOV/audio | packets + PCM | MOV | muxer/context RAII | mux writes are serial | Keep on CPU; returning compressed packets to the CPU is valid |

Main CPU buffer sizes for one 4096×3072 frame:

| Buffer | Approx. bytes |
|---|---:|
| U16 CFA | 25,165,824 |
| FP32 calibrated CFA | 50,331,648 |
| 3-plane FP32 RGB | 150,994,944 |
| planar 10-bit YUV stored as U16 | 50,331,648 |

The current automatic mode permits eight concurrent frame computations, so the upload bridge is for integration validation only; if retained long term, it increases system RAM, VRAM, and PCIe pressure simultaneously.

## 5. Frozen color-transform semantics

GPU output must match the following CPU behavior; do not “fix” it incidentally in the GPU backend. Any change must be an independent, versioned product decision.

### 5.1 RAW calibration

Use independently for each CFA position:

```text
normalized = (sample - black[cfa_position]) /
             (white[cfa_position] - black[cfa_position])
```

Do not clamp at this stage; preserve negative values and super-white. The production path then multiplies by 65535 to enter the librtprocess working domain.

### 5.2 Demosaic

- CFA: RGGB/BGGR/GRBG/GBRG;
- RCD by default;
- librtprocess input and output use the FP32 0..65535 working domain;
- The current production path skips the full-frame `/65535` after demosaic; that scale is folded into the subsequent matrix.

### 5.3 Camera RGB to DWG

- Iterate `CameraNeutral -> xy` for at most 50 iterations, with a `1e-10` convergence threshold;
- interpolate dual illuminants by reciprocal CCT;
- with ForwardMatrix: use the ForwardMatrix path;
- without ForwardMatrix: convert camera-to-XYZ-at-white, then use Bradford to D50;
- use Bradford from XYZ D50 to D65, then a fixed matrix to DaVinci Wide Gamut;
- use FP64 for setup and matrix operations, and FP32 for pixel output;
- exposure is `exp2(exposure_offset_stops)`.

### 5.4 Capture sharpening

- Operate in the linear DWG domain;
- form neutral detail using BT.2020 luma weights from target-linear RGB;
- use four-neighbor detail from the top, bottom, left, and right;
- after thresholding, add the same delta to R/G/B to avoid actively introducing chroma;
- default amount 0.4, threshold 0.002.

### 5.5 DaVinci Intermediate

- The analytic OETF is authoritative;
- the production LUT has 65,536 entries per segment;
- linear cut `0.00262409`；
- the default negative policy is `preserve_by_curve`, so negative values pass through the linear toe;
- non-finite values must raise an error; do not continue silently.

### 5.6 RGB to Y'CbCr and quantization

- matrix coefficients: BT.2020 non-constant luminance;
- `Kr=0.2627`, `Kb=0.0593`, `Kg=0.6780`；
- luma code: `64 + 876 * Y'`, clamped to 64..940;
- chroma code: `512 + 896 * Cb/Cr`, clamped to 64..960;
- quality chroma filter: `[-1, 4, 10, 4, -1] / 16`;
- sample at even x / left position, with edge clamping;
- deterministic noise range `[-0.5, 0.5)`, based on a frame/plane/sample hash;
- round positive code values by adding 0.5 and truncating;
- explicitly reject odd widths.

### 5.7 Current metadata facts

The current CPU writer writes:

```text
range       = MPEG/video
matrix      = BT.2020 NCL
primaries   = unspecified
transfer    = unspecified
profile     = ProRes 422 HQ
pixel format= yuv422p10le
```

The algorithm uses left-sited chroma, but `AVCodecContext/AVFrame` currently does not explicitly set `chroma_sample_location`. The sidecar requires the NLE to specify DaVinci Wide Gamut / DaVinci Intermediate manually, and the ADR explicitly marks the BT.2020 packing/chroma-siting behavior as provisional pending Resolve chart validation.

The GPU parity phase must first reproduce this metadata behavior. If `AVCHROMA_LOC_LEFT` or primaries/TRC markers are added, evaluate and version both CPU and GPU backends together; do not change only the GPU backend.

## 6. Current frame ownership

1. `McrawReader` owns the immutable index, container metadata, and decoder pool.
2. Each asynchronous frame task leases one `motioncam::Decoder`; the lease returns it to the pool on destruction.
3. The official decoder's vector is moved into `RawMosaicU16`.
4. Calibration, demosaic, and packing each allocate and return owning vectors; upstream buffers are destroyed automatically at the end of each stage.
5. `ProcessedFrame` owns the timestamp, metadata, color solution, and final YUV planes.
6. `write_video` moves the three YUV vectors to the heap and wraps them in three `AVBufferRef` objects using custom free callbacks.
7. Queued `AVFrame*` is released after `avcodec_send_frame`; if FFmpeg uses it asynchronously, FFmpeg must hold its own reference.
8. A worker receives an `AVPacket` and stores it by sequence; the packet is released after muxing.
9. `FfmpegWriter` uses RAII in its destructor to join workers and release queued frames, packets, codecs, and the muxer.

The GPU path must continue the “owning frame + RAII” rule. A bare `VkImage` cannot outlive its `AVFrame`; a slot can be reclaimed only after the encoder no longer holds a frame reference.

## 7. Current threads, queues, and backpressure

### 7.1 Frame compute

- The CLI starts frame tasks with `std::async(std::launch::async)`;
- the `pending` future deque is limited by `parallel_frames`, with a default maximum of 8;
- each frame uses OpenMP internally; with the default total CPU budget of 16, this is 2 threads/frame;
- consuming from the front of the future deque preserves submission order but introduces head-of-line waits;
- the `McrawReader` decoder pool supports concurrent frame decoding.

### 7.2 CPU encoding

- By default, use at most 16 independent `prores_ks` contexts, with one slice thread per context;
- limit in-flight jobs in the encode queue to `contexts + 2`;
- packets may complete out of order, but the `completed` map muxes only by submission sequence;
- audio encode/mux runs synchronously on the calling thread;
- a mutex serializes all mux writes.

### 7.3 Implications for the GPU scheduler

The current CPU encoder relies on the assumption that an intra-only encoder immediately returns at least one packet from every send. `prores_ks_vulkan` exposes `AV_CODEC_CAP_DELAY` and `async_depth`; do not reuse that assumption or drain/wait on every frame merely to satisfy it.

The GPU scheduler needs an independent bounded state machine:

```text
CPU read/decode queue
  -> GPU processing slots
  -> Vulkan encoder submission
  -> packet queue
  -> MOV mux
```

The Phase 3 upload bridge may initially reuse CPU frame computation, but it must use the correct send/drain/flush loop. Phase 6 can then extend queues/backpressure into the complete asynchronous pipeline.

## 8. FFmpeg integration points

All current FFmpeg integration is concentrated in `src/output/ffmpeg_writer.cpp`, which handles:

- ProRes codec discovery/configuration;
- video `AVFrame` wrapping;
- multi-context CPU encode scheduling;
- packet ordering；
- PCM encode；
- MOV context/header/interleave/trailer;
- FFmpeg error translation;
- cleanup.

This seam must be established before GPU integration. Keep `FfmpegWriter` as a CPU-compatible facade so existing CLI calls and CPU tests remain unchanged, while gradually extracting:

```text
Ffmpeg RAII + errors
MovMuxer
CpuProResEncoder
VulkanProResEncoder
```

Keep the CPU encoder's multi-context ordering logic in the CPU adapter. Start the Vulkan encoder with one logical device and one encoder context, using `async_depth` for internal parallelism.

## 9. Vulkan device-owner design

### 9.1 Decision

Choose an **FFmpeg-owned Vulkan device**:

1. Create the sole device context with `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VULKAN, selector, options)`;
2. obtain `AVVulkanDeviceContext` from `AVHWDeviceContext.hwctx`;
3. use its `inst`, `phys_dev`, `act_dev`, and queue-family information for application compute pipelines;
4. obtain all encoder input frames from that device's `AVHWFramesContext`;
5. manage and finally release the Vulkan/FFmpeg device context through one RAII owner.

Reasons:

- the project currently has no application-owned Vulkan runtime to preserve;
- the system smoke test proved that FFmpeg device creation works;
- this reduces the risk of omitting an extension, queue, or loader callback when the application creates a device and populates `AVVulkanDeviceContext`;
- application shaders and the encoder remain on the same logical device, with no cross-device copy.

### 9.2 Device selection

The system must record and allow selection by:

- application-visible stable GPU id；
- Vulkan device UUID；
- vendor ID / device ID；
- device name；
- driver name/version；
- actual compute queue family.

Configuration may accept an index for convenience, but enumeration index cannot be treated as a permanent identity. Prefer a discrete GPU by default and reject software/basic renderers; forced-GPU mode must error on failure, while auto mode records the reason and falls back to CPU.

## 10. Zero-copy handoff design

### 10.1 Phase 3 bridge

```text
Yuv422P10 CPU vectors
  -> software AVFrame (AV_PIX_FMT_YUV422P10LE)
  -> av_hwframe_get_buffer(Vulkan frame)
  -> av_hwframe_transfer_data
  -> AV_PIX_FMT_VULKAN
  -> prores_ks_vulkan
```

This validates only the encoder, format, timestamps, packets, muxing, and cleanup; it is not a performance conclusion.

### 10.2 Final GPU-resident handoff

```text
av_hwframe_get_buffer()
  -> AVFrame(format=AV_PIX_FMT_VULKAN,
             hw_frames_ctx.sw_format=AV_PIX_FMT_YUV422P10LE)
  -> AVVkFrame::img[]
  -> application compute writes Y/Cb/Cr images
  -> update layout/access/queue ownership
  -> wait+signal AVVkFrame timeline semaphores
  -> avcodec_send_frame()
```

FFmpeg 8.1.2's `AVVkFrame` already provides for each image:

- `img[]` / `mem[]`；
- `layout[]` / `access[]`；
- `queue_family[]`；
- timeline `sem[]` / `sem_value[]`。

Its contract requires each submission to wait for the current `sem_value` and signal an incremented value. The application must not bypass these fields or hide errors with per-frame `vkQueueWaitIdle()`.

Prefer allocation from the FFmpeg frames pool; do not implement application-owned image import in the first version. At runtime, query storage/transfer support on the RTX 3060 for the optimal-tiling image and fallback-plane formats corresponding to `AV_PIX_FMT_YUV422P10LE`; do not hard-code the number of VkFormats or the plane layout.

## 11. Measured bottleneck and GPU priority

Current compute-only snapshot with 8 frames, 16 CPU threads, 8 frames in flight, and 2 threads/frame:

| Stage | mean ms/frame |
|---|---:|
| official RAW decode + metadata | 61.05 |
| black/white calibration | 52.91 |
| demosaic | 411.14 |
| color solution | 0.04 |
| fused DWG/DI/sharpen/YUV | 565.17 |
| wall throughput | 6.64 fps |

This short test is for localization only and does not replace a formal benchmark. It indicates that GPU work should be prioritized as follows:

1. Fuse color/DI/sharpening/RGB-to-YUV;
2. demosaic;
3. calibration/RAW normalization;
4. RAW compression 6/7 decode;
5. Continue fusion or adjust queues only on the basis of profiler results.

The historical full CPU output was about 2.52 fps, below compute-only throughput, showing that CPU `prores_ks` and mux/write are also important end-to-end bottlenecks. Phase 3 must report upload, GPU encode, and write times separately.

## 12. Identified risks

### High

1. **Project FFmpeg does not enable Vulkan.** The version is correct but the build feature is wrong; use `avcodec_find_encoder_by_name` against the project-linked libraries as the authority.
2. **The CPU writer's packet-immediate assumption does not apply to the Vulkan encoder.** The GPU backend needs an independent send/drain/flush state machine.
3. **Color metadata is not fully productized.** Primaries/TRC are unspecified, chroma location is not written, and the ADR marks it provisional; GPU parity must be strict first, and any correction must be versioned for CPU and GPU together.
4. **Synchronization and lifetime.** Any error in the `AVVkFrame` timeline semaphore, layout, access, or queue family can produce random stale frames or corrupted frames; do not hide it with global idle.

### Medium

5. **The current FFmpeg writer handles both encoding and muxing.** A narrow abstraction is needed, protected by CPU bitstream/decoded-frame/metadata regression tests during refactoring.
6. **Current telemetry does not measure true encode time.** `prores_submit_wait` includes only submission and backpressure; add encode/mux/queue/VRAM/PCIe metrics before GPU work.
7. **There is currently no cancellation/device-lost state machine.** A CLI exception can leave `.partial.mov`; GPU runtime failure needs explicit queue shutdown and reason reporting.
8. **The large reference file is not retained in the repository.** Keep a small manifest and key-frame artifacts so the project does not depend only on historical numbers in documentation.
9. **Multi-frame 4K memory pressure.** The upload bridge temporarily retains CPU YUV and Vulkan frames simultaneously; pools and queues must be bounded.

### Low / operational

10. The Vulkan loader sees implicit layers from OBS, Bandicam, Steam, and others; validation/performance runs should disable overlays and record active layers.
11. The current `CMake` command comes from the Nuitka cache. Although its version meets requirements, a formal reproducible script should resolve a pinned toolchain or VS/CMake installation instead of relying on an accidental PATH.

## 13. Phase 1–3 file-level modification plan

### 13.0 Prerequisite: backend seam and reproducible dependencies

Goal: preserve externally visible CPU behavior and establish a shared CPU/GPU entry point.

Changes:

- `vcpkg.json`
  - Add `vulkan` to the FFmpeg features;
  - explicitly add the Vulkan headers/loader dependency used directly by the project;
  - retain the current builtin baseline; do not track a floating master.
- `CMakeLists.txt`
  - add `MCRAW_ENABLE_VULKAN`, enabled by default in development builds and explicitly disableable when capabilities are missing;
  - separate FFmpeg common, CPU encoder, and Vulkan targets;
  - enable validation/debug utilities only in Debug; Release must not depend on SDK layers.
- `include/mcraw/core/config.hpp`, `src/core/config.cpp`, `config/schema-v1.json`, `config/default.json`
  - add `backend = auto|cpu|vulkan`;
  - add GPU selector, `async_depth`, fallback, and precision;
  - preserve the CPU default and existing configuration semantics.
- `include/mcraw/output/video_encoder.hpp` (new)
  - the `IVideoEncoder` send/drain/flush/capability contract;
  - do not assume that one frame produces an immediate packet.
- `include/mcraw/output/video_frame.hpp` (new)
  - an explicit tagged variant for CPU YUV frames and Vulkan frames;
  - shared metadata containing width/height/PTS/time base/range/matrix/primaries/TRC/chroma location.
- `src/output/ffmpeg_raii.*` (new)
  - RAII and a unified error string for AVFrame/AVPacket/AVBufferRef/codec/context.
- `src/output/cpu_prores_encoder.*` (new)
  - extract the CPU encoder adapter from the existing writer;
  - preserve multi-context/sequence behavior.
- `src/output/mov_muxer.*` (new)
  - packet/timebase/interleave/trailer/PCM;
  - accept only compressed packets and do not depend on frame storage.
- `src/output/ffmpeg_writer.cpp`
  - preserve the facade and existing construction/call semantics; compose a CPU encoder plus muxer;
  - validate every step with the existing CPU E2E output regression.
- `src/cli/main.cpp`
  - runtime capability reporting and backend selection;
  - auto-fallback and forced-GPU error semantics;
  - `list-capabilities` prints reasons for unavailable capabilities.
- `tests/test_backend_selection.cpp`, `tests/test_ffmpeg_cpu_regression.cpp` (new)
  - auto/force/fallback;
  - no regression in CPU frame count, PTS, profile, metadata, or decoded checksum.

Acceptance: all current 18 tests plus the new tests pass; CPU sample output behavior is unchanged; the CPU backend still builds and runs independently when Vulkan is disabled.

### 13.1 Phase 1: Vulkan runtime/capability

Add:

- `include/mcraw/vulkan/vulkan_capabilities.hpp`
- `include/mcraw/vulkan/vulkan_runtime.hpp`
- `src/vulkan/vulkan_capabilities.cpp`
- `src/vulkan/vulkan_runtime.cpp`
- `src/vulkan/vulkan_debug.cpp`
- `src/vulkan/vulkan_telemetry.cpp`
- `tests/test_vulkan_runtime.cpp`

Responsibilities:

- enumerate physical devices, UUIDs, PCI/vendor/device IDs, and drivers;
- stable device selection;
- create/destroy the FFmpeg-owned Vulkan device context;
- validate the compute queue;
- install the debug messenger/object names;
- record active layers/extensions structurally;
- repeated init/destroy, fault injection, and no-device fallback.

Acceptance:

- RTX 3060 is selected consistently;
- 1,000 init/destroy cycles produce no validation errors or resource growth;
- selecting a nonexistent GPU falls back under auto and fails explicitly in forced mode;
- CPU tests are unaffected.

### 13.2 Phase 2: FFmpeg Vulkan device/frames context

Add:

- `include/mcraw/output/ffmpeg_vulkan_context.hpp`
- `src/output/ffmpeg_vulkan_context.cpp`
- `tests/test_ffmpeg_vulkan_context.cpp`
- `tests/test_ffmpeg_vulkan_capability.cpp`

Responsibilities:

- check `prores_ks_vulkan` using the project-linked libraries;
- print/freeze the current private-options mapping;
- query `av_hwdevice_get_hwframe_constraints`;
- create `AVHWFramesContext`:
  - `format = AV_PIX_FMT_VULKAN`；
  - prefer `sw_format = AV_PIX_FMT_YUV422P10LE`;
  - use the sample and small-test dimensions for width/height;
  - bound the pool size explicitly;
- allocate AVVkFrame and record actual VkFormat/image count/usage/layout;
- complete a single-frame software -> Vulkan -> software round trip for bridge validation only.

Acceptance:

- the project-library capability probe passes;
- the RTX 3060 supports the required sw format and storage/transfer usage;
- allocation/transfer/free validation is clean;
- an unsupported format returns a diagnosable reason and falls back to CPU.

### 13.3 Phase 3: CPU upload bridge + Vulkan ProRes

Add:

- `include/mcraw/output/vulkan_prores_encoder.hpp`
- `src/output/vulkan_prores_encoder.cpp`
- `src/output/vulkan_output_backend.cpp`
- `tests/test_vulkan_prores_smoke.cpp`
- `tests/test_vulkan_prores_e2e.cpp`

Modify:

- `src/cli/main.cpp`
  - integrate GPU backend selection;
  - output backend, GPU, driver, FFmpeg/libav versions, async depth, and fallback reason.
- `src/output/sidecar.cpp`
  - add pipeline/backend/FFmpeg/Vulkan/device/transfer counters;
- `scripts/validate-samples.ps1`
  - add ffprobe, frame-count, PTS, decode, and failure-cleanup checks for CPU and upload-bridge GPU output.

Encoder rules:

- start with one `prores_ks_vulkan` context;
- profile HQ，alpha disabled；
- validate `async_depth` at 1, then test 2/4/8;
- correctly handle send `EAGAIN`, the receive loop, delayed packets, and null-frame flush;
- mux receives packets only;
- rename a partial file only after flush, trailer, close, and basic ffprobe validation succeed;
- a device failure must not switch to CPU inside the same MOV.

Acceptance:

- complete output for a 10-second sample;
- frame count, PTS/duration, profile, audio sync, and metadata match the CPU reference;
- readable by FFmpeg/ffprobe/Resolve;
- validation 0 error；
- 100 iterations with no resource growth;
- telemetry explicitly marks `gpu_resident=false`, `upload_frames=N`, `readback_frames=0`;
- do not treat this phase's speed as the final GPU pipeline conclusion.

## 14. Boundaries for later phases

Only Phase 4 begins moving color/DI/RGB-to-YUV to Vulkan; only Phase 5 may report `gpu_resident=true`. This invariant must be encoded in the implementation and tests:

```text
gpu_resident == true
=> upload_frames == 0
=> readback_frames == 0
```

Phase 4 shader golden tests should compare each pass against the CPU FP64/analytic reference before deciding whether to fuse passes. The first version uses FP32 precise mode; FP16 must be a separate fast mode and may be enabled only after passing maximum-error, RMSE, percentile, outlier-coordinate, and final 10-bit LSB thresholds.

## 15. Audit approval gates

Confirm these design decisions before implementation begins:

1. Keep the CPU pipeline as the default and fallback, with public behavior frozen;
2. let FFmpeg own the sole Vulkan device, borrowed by the application;
3. define Phase 3 explicitly as an upload bridge, without claiming final acceleration;
4. preserve the current unspecified primaries/TRC and existing chroma metadata behavior for initial GPU parity;
5. choose fused color/DI/YUV as the first GPU-processing target based on measured hotspots, then migrate RCD demosaic;
6. treat any output-metadata improvement as a versioned change to shared CPU/GPU behavior.

After approval, begin with the backend seam and vcpkg Vulkan build in 13.0; do not jump directly to shaders or zero-copy.
