# Windows Vulkan ProRes GPU Pipeline Implementation Guide

> Engineering implementation guide for Codex / development agents  
> Goal: build a verifiable, fallback-capable, and as GPU-resident-as-possible Vulkan ProRes pipeline beside the existing stable CPU transcoding pipeline.  
> Platforms: Windows 10/11, NVIDIA/AMD/Intel Vulkan drivers, and FFmpeg/libav*.  
> Primary encoder: `prores_ks_vulkan`.  
> Principle: **Do not break the existing CPU pipeline; land the GPU pipeline incrementally as an independent backend.**

---

## 0. Highest-priority instructions for Codex

1. Read and understand the existing CPU pipeline before writing code.
2. Do not rewrite the stable CPU path in place.
3. Add an abstraction layer so CPU and GPU backends can coexist:
   - The CPU backend remains the reference implementation and automatic fallback.
   - The GPU backend is enabled through a feature flag / runtime capability probe.
4. The final GPU-pipeline goal is not “upload CPU frames and call a GPU encoder”; it is:

   ```text
   RAW/MCRAW decode
       -> Vulkan unpack / normalization
       -> Vulkan debayer
       -> Vulkan image processing
       -> Vulkan color transform / Log encoding
       -> Vulkan RGB-to-YUV 4:2:2 10-bit
       -> AV_PIX_FMT_VULKAN frame
       -> prores_ks_vulkan
       -> CPU-visible compressed packet
       -> MOV muxer / file writer
   ```

5. Every stage must be independently verifiable:
   - numerical correctness
   - Vulkan synchronization
   - image layout
   - color metadata
   - memory lifetime
   - performance
6. Do not make the GPU pipeline the default until bitstream, image-quality, and stability tests pass.
7. Every failure must be able to:
   - produce diagnosable logs;
   - safely release Vulkan/FFmpeg resources;
   - automatically or explicitly fall back to the CPU pipeline;
   - leave no corrupted or falsely complete output file.

---

## 1. Current technical prerequisites and version strategy

### 1.1 Do not rely on the FFmpeg version number alone

`prores_ks_vulkan` exists in current FFmpeg mainline source and Doxygen, but different official packages, third-party Windows builds, and branches may not enable it or may not include the encoder yet.

At startup, run a runtime capability check that verifies at least:

```bash
ffmpeg -hide_banner -encoders | findstr /I prores
ffmpeg -hide_banner -h encoder=prores_ks_vulkan
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -init_hw_device list
```

The code must verify:

```cpp
const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks_vulkan");
if (!codec) {
    // GPU ProRes backend unavailable
}
```

Check build configuration and Vulkan support together; do not assume that every program named `ffmpeg.exe` on the system meets the requirements.

### 1.2 Recommended dependency strategy

Short-term development:

- pin a verified FFmpeg Git commit;
- save the commit hash, configure parameters, and compiler version;
- version Windows build artifacts reproducibly with the project;
- do not track a floating `master` as a production dependency.

Formal release:

- use an explicit version or vendored FFmpeg build;
- print at startup:
  - `av_version_info()`
  - libavcodec/libavutil/libavformat versions
  - Vulkan API version
  - GPU name, vendor ID, device ID, and driver version
  - encoder name and profile
  - whether the GPU-resident or upload-bridge path is enabled

### 1.3 Encoder characteristics

`prores_ks_vulkan` is Vulkan Compute encoding, not NVENC and not fixed-function Vulkan Video ProRes encoding.

Therefore:

- it uses general-purpose GPU compute resources;
- it competes for shader resources and bandwidth with Vulkan debayer, denoising, scaling, and color conversion;
- its load cannot be inferred from NVENC utilization;
- it is not limited by the number of NVENC sessions;
- analyze it with Vulkan timestamp queries, GPUView, Nsight Graphics/Systems, RenderDoc, and similar tools.

---

## 2. Success criteria

The GPU pipeline is complete only when all of the following conditions are met.

### 2.1 Functional correctness

- produce a valid MOV/ProRes file;
- be readable by at least three of Resolve, Premiere, FFmpeg, and ffprobe;
- have correct profile, resolution, frame rate, time base, and color metadata;
- have correct frame count, PTS/DTS, and duration;
- lose neither the first nor the last frame;
- flush correctly;
- report an explicit error for odd or unsupported dimensions;
- follow the defined file-handling policy after cancellation.

### 2.2 Image-quality correctness

Compare with the CPU reference pipeline:

- black/white levels match before and after debayer;
- white balance, CFA order, and matrices match;
- RGB and YUV range and transfer function match;
- chroma siting is explicit;
- the 4:2:2 downsampling filter is explicit;
- 10-bit quantization, rounding, clamping, and dithering are explicit;
- no systematic channel swaps, color casts, or legal/full-range mismatch;
- HDR/Log data is not clipped accidentally.

### 2.3 Performance correctness

Report separately:

```text
decode fps
GPU processing fps
RGB->YUV fps
encode fps
mux/write fps
end-to-end fps
GPU utilization
VRAM usage
CPU utilization
PCIe upload/download throughput
disk write throughput
pipeline latency
```

Ideal path:

- uncompressed images do not return to system memory;
- the CPU receives only compressed `AVPacket` objects;
- the GPU queue and encoder have multiple frames in flight;
- the encode stage does not wait immediately after submitting each frame.

### 2.4 Stability correctness

Complete at least:

- loop a 10-second short clip 100 times;
- 10 minutes of media;
- 1 hour of media;
- multi-file batch processing;
- cancellation and restart;
- simulated or real GPU device-loss recovery;
- different profiles;
- multiple resolutions;
- at least two generations of NVIDIA drivers;
- Debug validation layer 0 error；
- no resource growth in Release mode.

---

## 3. Overall architecture

Do not make the encoder depend directly on the MCRAW reader, GUI, or a specific debayer implementation.

Recommended module boundaries:

```text
InputReader
  -> RawFrame / RawFrameView

IDecodeBackend
  -> CpuDecodeBackend
  -> VulkanDecodeBackend

IImagePipeline
  -> CpuImagePipeline
  -> VulkanImagePipeline

IFrameConverter
  -> CpuRgbToYuvConverter
  -> VulkanRgbToYuvConverter

IVideoEncoder
  -> CpuProResEncoder
  -> VulkanProResEncoder

IMuxer
  -> MovMuxer

PipelineScheduler
  -> queues
  -> backpressure
  -> cancellation
  -> error propagation
  -> telemetry
```

### 3.1 Unified frame description

Do not use one “universal Frame” with many nullable fields.

Use a tagged variant:

```cpp
enum class FrameStorage {
    Cpu,
    Vulkan
};

struct FrameMetadata {
    int width;
    int height;
    int64_t pts;
    AVRational timeBase;

    AVColorPrimaries primaries;
    AVColorTransferCharacteristic transfer;
    AVColorSpace matrix;
    AVColorRange range;
    AVChromaLocation chromaLocation;
};

struct CpuVideoFrame {
    FrameMetadata meta;
    AVPixelFormat format;
    AVFramePtr frame;
};

struct VulkanVideoFrame {
    FrameMetadata meta;
    AVPixelFormat format;        // AV_PIX_FMT_VULKAN
    AVFramePtr frame;            // owns/reference-counts AVVkFrame
    AVPixelFormat swFormat;      // logical software representation
};

using VideoFrame = std::variant<CpuVideoFrame, VulkanVideoFrame>;
```

Ownership must be explicit:

- use RAII for `AVFrame`;
- do not give a bare `VkImage` a lifetime independent of its owning `AVBufferRef/AVFrame`;
- while the encoder holds a frame asynchronously, the producer must not reuse or destroy its resources;
- the frame pool must account for `async_depth` and the number of in-flight preprocessing and postprocessing operations.

---

## 4. Migration route: from the stable CPU pipeline to the GPU pipeline

## Phase 0: Freeze the CPU reference

Before writing GPU code:

1. Establish fixed test media for the current CPU pipeline;
2. save:
   - frame count
    - PTS for every frame
   - ffprobe JSON
    - lossless images or raw buffers for several key frames
   - waveform/statistics
   - hash
3. record current CPU speed and memory usage;
4. define any remaining unspecified color behavior in the CPU pipeline;
5. do not use the GPU to “fix” CPU output incidentally unless a new versioned behavior is established.

The CPU output is the migration reference. It is not necessarily absolute truth, but it must be reproducible.

## Phase 1: Establish only the Vulkan runtime

Do not encode yet.

Complete:

- Vulkan instance；
- physical device selection；
- logical device；
- compute queue selection；
- command pools；
- descriptor pools；
- pipeline cache；
- synchronization primitives；
- debug messenger；
- device telemetry；
- deterministic teardown。

Device selection must support:

- user-specified GPU index, PCI ID, or name;
- default selection of a suitable discrete GPU;
- recording the actual selection;
- never silently selecting Microsoft Basic Render Driver;
- no assumption that enumeration order is permanently stable on multi-GPU systems.

Acceptance:

- initialize/destroy an empty pipeline 1,000 consecutive times;
- no validation-layer errors;
- correct multi-GPU switching;
- device-creation failure can fall back to CPU.

## Phase 2: Establish the FFmpeg Vulkan device/context

Choose one Vulkan device owner first.

### Recommended approach

Either let the application own the Vulkan device and connect it safely to FFmpeg, or let FFmpeg create the device and let the application use that same device through `AVVulkanDeviceContext`. Choose one; do not maintain two unaware Vulkan devices long term and attempt ad-hoc copying between them.

Selection criteria:

- if the existing GPU pipeline already has a mature Vulkan runtime, the application should be the owner;
- if the project has no Vulkan code and only needs to validate the encoder first, FFmpeg may create the device initially;
- the final goal is for processing and encoding to share one logical device and compatible queue families.

Understand:

```cpp
AVBufferRef* hw_device_ctx;   // AVHWDeviceContext
AVBufferRef* hw_frames_ctx;   // AVHWFramesContext
```

Typical conceptual flow:

```cpp
av_hwdevice_ctx_create(
    &hw_device_ctx,
    AV_HWDEVICE_TYPE_VULKAN,
    device_selector,
    options,
    0
);

AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);

frames->format = AV_PIX_FMT_VULKAN;
frames->sw_format = chosen_software_format;
frames->width = width;
frames->height = height;
frames->initial_pool_size = pool_size;

av_hwframe_ctx_init(frames_ref);
```

Notes:

- query and validate supported `sw_format` values at runtime;
- do not mechanically copy NV12 from another hardware-encoder example;
- the logical format required by ProRes 422 10-bit must match the Vulkan frame format actually accepted by the encoder;
- output the complete FFmpeg error string for every error.

Acceptance:

- an `AV_PIX_FMT_VULKAN` frame can be allocated;
- `AVVkFrame` can be queried;
- one software-frame -> hardware-frame transfer can be performed;
- resources can be released safely;
- even if slow, this phase is for bridge validation only.

## Phase 3: Minimal usable GPU encoder, using the upload bridge first

This phase is not the final performance solution; it isolates encoder-integration problems.

Temporary path:

```text
existing CPU pipeline
  -> CPU 10-bit 4:2:2 frame
  -> av_hwframe_transfer_data / explicit upload
  -> AV_PIX_FMT_VULKAN
  -> prores_ks_vulkan
  -> packet
  -> MOV
```

Purpose:

- validate encoder opening;
- validate profile/options;
- validate the hardware-frame format;
- validate the send/receive loop;
- validate muxing;
- validate output compatibility;
- establish GPU-versus-CPU ProRes comparison samples.

Do not treat this phase's speed as the final conclusion.

### Encoder initialization

Conceptual code:

```cpp
const AVCodec* codec =
    avcodec_find_encoder_by_name("prores_ks_vulkan");

AVCodecContext* enc = avcodec_alloc_context3(codec);

enc->width = width;
enc->height = height;
enc->time_base = time_base;
enc->framerate = frame_rate;
enc->pix_fmt = AV_PIX_FMT_VULKAN;
enc->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);

enc->color_primaries = primaries;
enc->color_trc = transfer;
enc->colorspace = matrix;
enc->color_range = range;
enc->chroma_sample_location = chroma_location;

// Set profile and Vulkan encoder private options via av_opt_set*.
// Every option must be checked against the exact vendored FFmpeg build.

int err = avcodec_open2(enc, codec, &options);
```

Do not hard-code private option names that have not been runtime-verified. At initialization, print the equivalent capability information from:

```bash
ffmpeg -h encoder=prores_ks_vulkan
```

or freeze the option mapping for the current FFmpeg commit at build time.

### Correct send/receive loop

```cpp
int send_frame(AVFrame* frame) {
    int ret = avcodec_send_frame(enc, frame);

    if (ret == AVERROR(EAGAIN)) {
        drain_packets();
        ret = avcodec_send_frame(enc, frame);
    }

    if (ret < 0) {
        return ret;
    }

    return drain_packets();
}

int drain_packets() {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(enc, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return 0;
        }

        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }

        // rescale timestamps if required
        // write through muxer
        av_packet_free(&pkt);
    }
}
```

flush：

```cpp
avcodec_send_frame(enc, nullptr);
while (avcodec_receive_packet(enc, pkt) >= 0) {
    write_packet(pkt);
}
```

Do not assume that one input frame immediately produces one packet.

Acceptance:

- complete encoding of a 10-second test clip;
- correct frame count and PTS;
- stable operation of the CPU-upload GPU encoder;
- output readable by multiple software packages;
- no validation-layer errors.

## Phase 4: Move RGB->YUV 4:2:2 10-bit to Vulkan

This is the most important color-correctness phase.

Do not make the ProRes encoder implicitly perform undefined color conversion. The GPU image pipeline must explicitly produce the logical format required by the encoder.

### Define the following mathematical semantics explicitly

1. input RGB working space;
2. transfer function；
3. RGB primaries；
4. YCbCr matrix coefficients；
5. full/legal range；
6. 10-bit code value mapping；
7. chroma downsample kernel；
8. chroma sample location；
9. rounding；
10. dithering。

### Recommended shader layering

```text
Shader A: RAW normalization / black-white level
Shader B: debayer
Shader C: WB / camera matrix / gamut transform
Shader D: optional denoise / lens correction
Shader E: transfer/log encoding
Shader F: RGB -> YCbCr 4:4:4 intermediate
Shader G: chroma low-pass + 4:2:2 downsample + 10-bit pack
```

Some passes may be fused after performance is stable, but an independent reference must exist first.

### 4:2:2 downsampling

Do not simply discard every other chroma sample.

Implement at least a defined low-pass filter and create test patterns:

- colored one-pixel vertical lines;
- red/blue checkerboard;
- zone plate；
- saturated edges;
- grayscale;
- Log gradient。

Ensure:

- chroma phase matches metadata;
- left and right boundary handling is explicit;
- reject odd widths if unsupported;
- no out-of-bounds reads or writes.

### Numeric formats

Keep both modes:

```text
GPU precise mode: FP32
GPU fast mode: FP16 where validated
```

Acceptance:

- GPU-generated YUV frames can be downloaded to CPU;
- compare pixel by pixel and perceptually with the CPU reference;
- define error thresholds;
- validate raw YUV before connecting the encoder.

## Phase 5: Implement a truly GPU-resident frame handoff

The final path must not upload every frame with `av_hwframe_transfer_data()`.

Goal:

```text
application Vulkan compute writes VkImage
  -> ownership/layout transition
  -> AVFrame / AVVkFrame references same image
  -> encoder consumes it
```

### Key requirements

1. one Vulkan logical device;
2. image format compatible with the FFmpeg frame context;
3. usage flags satisfying processing and encoding requirements;
4. correct image layout;
5. correct queue-family ownership;
6. correct synchronization;
7. frame lifetime covering asynchronous encoder use;
8. correct memory allocation and alignment;
9. do not reuse a frame before the encoder finishes;
10. do not map or copy uncompressed images on the CPU.

### Preferred implementation

Prefer allocating frames from the FFmpeg `AVHWFramesContext` pool and having application shaders write to their `AVVkFrame::img[]` images.

Flow:

```text
av_hwframe_get_buffer()
  -> AVFrame(format=AV_PIX_FMT_VULKAN)
  -> obtain AVVkFrame
  -> record application compute commands writing to its VkImage
  -> transition and synchronize
  -> avcodec_send_frame()
```

Consider importing application-owned images into an FFmpeg user pool only after this approach has been shown unable to meet performance or format requirements.

### Synchronization strategy

Initial recommendation:

- one compute queue;
- timeline semaphore；
- one unique incrementing timeline value per frame;
- a frame-slot state machine;
- ensure writes are complete before encoder handoff;
- do not wait with `vkQueueWaitIdle()` on every frame.

```cpp
enum class SlotState {
    Free,
    Processing,
    ReadyForEncode,
    Encoding,
    Recyclable,
    Failed
};
```

The slot may be reused if and only if the encoder no longer holds a frame reference.

Acceptance:

- GPU output is not downloaded;
- encoder input is `AV_PIX_FMT_VULKAN`;
- PCIe upload throughput is near zero;
- no corrupted frames, stale-frame reappearance, or random corruption during long runs;
- the validation layer reports no ownership/layout errors.

## Phase 6: Asynchronous scheduling and backpressure

Use at least four logical stages:

```text
Read/Decode
GPU Process
GPU Encode
Mux/Write
```

Queues must be bounded:

```text
raw_queue:       bounded
gpu_ready_queue: bounded
packet_queue:    bounded
```

### Pool size

```text
pool_size =
    decode_in_flight
  + processing_in_flight
  + encoder_async_depth
  + safety_margin
```

Start with 8–16 frames and adjust using telemetry.

The normal per-frame path must not contain:

```cpp
vkDeviceWaitIdle();
vkQueueWaitIdle();
avcodec_flush_buffers();
```

### Muxer backpressure

Required:

- a dedicated packet-writer thread or asynchronous I/O;
- monitoring packet-queue depth;
- monitoring actual write MB/s;
- an explicit block or failure when disk space is insufficient;
- never drop frames and continue generating a file that appears valid.

---

## 5. Encoding profile and output strategy

The first phase supports only:

```text
ProRes 422 Standard
ProRes 422 HQ
10-bit 4:2:2
progressive
no alpha
MOV
```

Add Proxy, LT, 4444, 4444 XQ, alpha, and interlaced later.

Do not assume profile numeric values are permanently stable or match another encoder. Use the option API and definitions from the current FFmpeg build.

Configuration example:

```json
{
  "encoder": "prores_ks_vulkan",
  "profile": "hq",
  "async_depth": 8,
  "gpu_index": 0,
  "fallback": "prores_ks",
  "gpu_pipeline": true,
  "precision": "fp32"
}
```

At minimum, write these correctly:

- width/height；
- frame rate；
- time base；
- color primaries；
- transfer characteristic；
- matrix coefficients；
- color range；
- chroma location；
- SAR/DAR；
- rotation/orientation；
- audio sync。

---

## 6. Error handling and fallback

### 6.1 At startup

If any of the following conditions fails, mark the GPU backend unavailable:

- encoder not found；
- Vulkan device creation failed；
- required format unsupported；
- hw frame context init failed；
- shader pipeline creation failed；
- encoder open failed；
- smoke test failed。

```text
Auto mode:
    log reason
    fallback CPU

Force GPU mode:
    fail explicitly
```

### 6.2 At runtime

For device loss:

1. stop accepting new frames;
2. cancel/end the queues;
3. close and mark the current output incomplete;
4. release FFmpeg/Vulkan resources;
5. do not switch seamlessly to CPU within the same output file;
6. allow resuming from a checkpoint or restarting the entire file.

### 6.3 Temporary files

Write to:

```text
filename.mov.partial
```

Atomically rename to the final file only after encoder flush, muxer trailer, file close, and basic validation all succeed.

---

## 7. Test design

### 7.1 Unit tests

Cover:

- range conversion；
- transfer functions；
- RGB->YCbCr；
- 10-bit quantization；
- rounding；
- chroma filter；
- edge handling；
- PTS rescale；
- profile mapping；
- config validation；
- resource state machine。

### 7.2 GPU shader golden tests

For each shader:

1. small deterministic input;
2. GPU dispatch；
3. readback；
4. compare with the double/FP64 CPU reference;
5. report max absolute error, mean absolute error, RMSE, percentile, and outlier coordinates.

Do not compare only the final compressed file.

### 7.3 Overall test media

- flat fields；
- near-black Log gradients；
- near-white gradients；
- saturated primaries；
- skin-tone-like patches；
- color checker；
- high-frequency chroma；
- random noise；
- real MCRAW clips；
- extreme white balance；
- clipped highlights；
- bad pixels。

CPU ProRes and GPU ProRes bitstreams need not be bit-identical; compare decoded frames, metadata, profile, bitrate, and artifacts.

### 7.4 Compatibility matrix

| Reader/NLE | Decode | Seek | Color | Duration | Audio sync |
|---|---:|---:|---:|---:|---:|
| ffmpeg/ffprobe | | | | | |
| DaVinci Resolve | | | | | |
| Adobe Premiere Pro | | | | | |
| VLC/mpv | | | | | |

---

## 8. Benchmark method

Measure separately:

```text
A. CPU baseline full pipeline
B. CPU image pipeline + GPU upload + Vulkan ProRes
C. GPU image pipeline + readback + CPU ProRes
D. GPU image pipeline + Vulkan ProRes
E. D + mux/write
```

For each:

- warm up for 100 frames;
- run the formal test for at least 1,000 frames or 30 seconds;
- repeat 3–5 times;
- report median and p95;
- fix the media and power policy;
- write output to both a RAM disk and a real SSD.

Metrics:

```text
fps
ms/frame
GPU process ms
GPU encode ms
CPU submit ms
queue wait ms
disk write ms
VRAM peak
system RAM peak
PCIe H2D MB/s
PCIe D2H MB/s
CPU utilization
GPU compute utilization
```

Suggested integration gates for the GPU pipeline:

- image quality passes;
- end-to-end throughput improves by at least 1.5×;
- CPU utilization decreases materially;
- no uncompressed-frame readback;
- no per-frame queue/device idle;
- sustained real-time-or-better 4K30;
- no resource growth during long runs.

---

## 9. Windows-specific requirements

- pin FFmpeg headers/libs and build configuration;
- save `ffmpeg -buildconf`;
- state LGPL/GPL configuration and license notices clearly;
- check `vulkan-1.dll` and the ICD;
- ensure Release does not depend on the Vulkan SDK or validation layer;
- set debug names for Vulkan objects;
- support RenderDoc, Nsight, GPUView/ETW markers;
- avoid a single excessively long shader dispatch triggering TDR;
- do not require ordinary users to modify the TDR registry.

---

## 10. Suggested code structure

```text
src/
  pipeline/
    pipeline_scheduler.*
    frame_metadata.*
    frame_queue.*
    cancellation.*

  cpu/
    cpu_decode_backend.*
    cpu_image_pipeline.*
    cpu_prores_encoder.*

  vulkan/
    vulkan_instance.*
    vulkan_device.*
    vulkan_queue.*
    vulkan_frame_pool.*
    vulkan_sync.*
    vulkan_shader_cache.*
    vulkan_telemetry.*

    shaders/
      raw_normalize.comp
      debayer.comp
      color_transform.comp
      log_encode.comp
      rgb_to_ycbcr.comp
      chroma_422.comp

  ffmpeg/
    ffmpeg_raii.*
    ffmpeg_error.*
    ffmpeg_vulkan_context.*
    prores_vulkan_encoder.*
    mov_muxer.*

  tests/
    color_math_tests.*
    chroma_tests.*
    shader_golden_tests.*
    encoder_smoke_tests.*
    end_to_end_tests.*
```

---

## 11. Telemetry

Emit structured logs for every transcode:

```json
{
  "pipeline": "vulkan_prores",
  "ffmpeg_version": "...",
  "ffmpeg_commit": "...",
  "encoder": "prores_ks_vulkan",
  "gpu": "...",
  "driver": "...",
  "resolution": "3840x2160",
  "profile": "hq",
  "async_depth": 8,
  "frame_pool": 12,
  "gpu_resident": true,
  "upload_frames": 0,
  "readback_frames": 0,
  "fps": 0.0,
  "vram_peak_mb": 0,
  "fallback": false
}
```

Key invariant:

```text
gpu_resident == true
=> upload_frames == 0
=> readback_frames == 0
```

Returning a compressed packet to the CPU does not count as frame readback.

---

## 12. Staged Codex tasks

### Task 1: Audit the existing CPU pipeline

Output:

- pipeline diagram;
- frame formats；
- thread model；
- color transforms；
- ownership；
- bottlenecks；
- GPU insertion points.

### Task 2: Establish the encoder backend interface

Complete `IVideoEncoder`, the CPU adapter, GPU stub, configuration, and capability reporting; all existing tests must pass.

### Task 3: Introduce a reproducible FFmpeg Vulkan build

Complete the pinned commit, build scripts, CI capability check, license handling, and `prores_ks_vulkan` probe.

### Task 4: Vulkan runtime and FFmpeg hardware context

Complete device selection, the shared device, frames context, frame allocation, and the smoke test.

### Task 5：CPU upload bridge encoder

Complete CPU YUV -> Vulkan upload -> encode -> mux -> flush. This task proves integration only and does not claim final acceleration.

### Task 6：GPU RGB->YUV 422

Complete shaders, golden tests, range/log/chroma tests, and precision modes.

### Task 7：GPU-resident handoff

Complete the frame pool, direct writes to encoder-compatible images, synchronization, no-upload/no-readback operation, and long-duration tests.

### Task 8: Asynchronous pipeline

Complete bounded queues, async depth, backpressure, cancellation, and telemetry.

### Task 9: Performance optimization

Optimize only according to profiler results:

- command submission；
- descriptor churn；
- image transitions；
- queue contention；
- shader occupancy；
- memory bandwidth；
- frame pool stalls；
- disk stalls；
- unnecessary format conversions.

### Task 10: Productionization

Complete fallback, partial-file handling, error taxonomy, device loss, the compatibility matrix, user settings, and the benchmark report.

---

## 13. Prohibited actions

Codex must not:

- delete the CPU pipeline;
- treat the upload bridge as the final GPU pipeline;
- call `vkQueueWaitIdle()` on every frame;
- create/destroy a Vulkan pipeline, descriptor pool, or device on every frame;
- use an unbounded frame queue;
- hard-code an unverified FFmpeg private option;
- assume `AV_PIX_FMT_VULKAN` has a fixed memory layout;
- ignore color range or chroma location;
- inspect image quality only by eye;
- add global queue idle to hide a synchronization bug;
- fuse all shaders without benchmark evidence;
- make the GUI thread perform encoding or mux I/O;
- scatter raw FFmpeg/Vulkan pointers through the business layer;
- disguise a hidden CPU copy as a GPU-resident pipeline.

---

## 14. Definition of Done

- [ ] CPU backend has no regression;
- [ ] `prores_ks_vulkan` runtime capability probe；
- [ ] pinned FFmpeg build；
- [ ] Windows NVIDIA tests pass;
- [ ] capability/failure tests for at least one other vendor;
- [ ] Vulkan validation clean；
- [ ] GPU shader golden tests pass;
- [ ] 4:2:2 chroma tests pass;
- [ ] Log/range/metadata tests pass;
- [ ] complete send/receive/flush;
- [ ] MOV trailer and partial-file strategy;
- [ ] Resolve/Premiere/FFmpeg compatibility；
- [ ] no uncompressed-frame upload;
- [ ] no uncompressed-frame readback;
- [ ] no per-frame global wait;
- [ ] bounded queues；
- [ ] cancellation；
- [ ] device lost handling；
- [ ] one-hour stability;
- [ ] batch stability；
- [ ] benchmark reaches project gates;
- [ ] CPU fallback works;
- [ ] documentation and licensing complete.

---

## 15. Recommended first implementation route

```text
CPU reference frozen
  -> encoder abstraction
  -> pinned FFmpeg build
  -> Vulkan device + AVHWFramesContext
  -> CPU YUV upload bridge
  -> prores_ks_vulkan encode/mux
  -> verify compatibility
  -> Vulkan RGB->YUV 422
  -> write into FFmpeg-owned Vulkan frames
  -> remove upload
  -> asynchronous frame pool
  -> optimize with profiler
  -> production fallback and stability
```

Do not jump from a custom RAW Vulkan debayer directly to complete ProRes output. First prove the encoder bridge, then color conversion, then zero-copy, and finally parallelism and performance optimization.

---

## 16. References

- FFmpeg `prores_ks_vulkan` source / Doxygen:  
  https://ffmpeg.org/doxygen/trunk/proresenc__kostya__vulkan_8c_source.html

- FFmpeg `AVHWFramesContext`:  
  https://ffmpeg.org/doxygen/trunk/structAVHWFramesContext.html

- FFmpeg Vulkan hardware context header:  
  https://ffmpeg.org/doxygen/trunk/hwcontext__vulkan_8h_source.html

- FFmpeg `AVVkFrame`:  
  https://ffmpeg.org/doxygen/trunk/structAVVkFrame.html

- FFmpeg hardware encode example:  
  https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/vaapi_encode.c

- Khronos: Vulkan Compute codecs in FFmpeg:  
  https://www.khronos.org/blog/video-encoding-and-decoding-with-vulkan-compute-shaders-in-ffmpeg

- FFmpeg codecs documentation:  
  https://ffmpeg.org/ffmpeg-codecs.html

---

## 17. Final execution guidance for Codex

Before coding begins, submit a `GPU_PIPELINE_AUDIT.md` containing:

1. the actual data flow of the current CPU pipeline;
2. input and output formats for every stage;
3. all color transforms;
4. frame ownership；
5. thread/queue model；
6. FFmpeg integration points;
7. the Vulkan device-owner design;
8. the zero-copy handoff design;
9. identified risks;
10. the Phase 1–3 file-level modification plan.

After the audit is approved, begin with the encoder abstraction and upload bridge. If any phase finds that the current FFmpeg build, Vulkan image format, or synchronization interface cannot support zero-copy, record the facts and a minimal reproduction; do not disguise the limitation with a hidden CPU copy and call it a GPU-resident pipeline.
