# GPU Pipeline Build Summary and Performance Bottleneck Analysis

Date: 2026-07-14  
Nature: analysis summary; no code changes. This is a current-state snapshot for
future agents.  
Sources: `GPU_PIPELINE_AUDIT.md`, the `GPU_PHASE1~8` validation reports, and
`implementation-status.md`.  
Manual acceptance: the exported files were manually compared by the user and
showed no visible difference from the CPU version, consistent with the documented
≤1 LSB / PSNR ~88 dB result.

## 1. Build overview (Phases 0–8)

| Phase | Work | Status |
|---|---|---|
| Phases 0–2 | Backend seam (`IVideoEncoder` abstraction and CPU encoder/muxer split); vcpkg rebuild with Vulkan-enabled FFmpeg 8.1.2; one FFmpeg-owned Vulkan device borrowed by the application | Complete |
| Phase 3 | CPU YUV upload bridge plus `prores_ks_vulkan`; encoder, muxing, and cleanup validation | Complete |
| Phase 4 | Application-owned compute shader for RGB→BT.2020 NCL YCbCr 4:2:2 10-bit packing; CPU-reference golden test within ≤1 LSB; bit-identical dither; FP16 explicitly disabled | Complete |
| Phases 5–6 | GPU-resident direct path: shader writes `AVVkFrame` directly, synchronized by timeline semaphore; bounded asynchronous pipeline (RGB job queue → GPU slots → encode → packet queue → dedicated mux thread), no `vkQueueWaitIdle` on the normal path, with cancellation, backpressure, and telemetry | Complete |
| Phase 7 | Measured optimization of the CPU-side TargetLog RGB producer: 1357 → 547 ms/frame (-59.7%), end-to-end 2.306 → 3.618 fps | Complete |
| Phase 8 | Productization: `backend=auto` can fall back to CPU after full preflight; `backend=vulkan` terminates on failure and never switches encoders inside one MOV; `device_lost` error category | Complete (release gates are not all closed) |

**Measured result (240-frame 4096×3072 real sample, RTX 3060):**

- CPU end-to-end: 2.52 fps → GPU path: **6.95 fps (about 2.76×)**;
- `gpu_resident=true`, `direct_frames=240`, `upload_frames=0`, `readback_frames=0`;
- Full-stream FFmpeg decode passed, with audio/video duration matching the CPU output.

## 2. Bottleneck location: the GPU is starved; the bottleneck is the CPU

Phase 6/8 telemetry: GPU slot queue peaks were 6/10, the packet queue peak was
1/16, and **backpressure wait time was zero**. The Vulkan shader and encoder never
became limiting factors; they continuously waited for the CPU to provide data.

Only the final two steps currently run on the GPU (RGB→YUV packing and ProRes
encoding). The main per-frame costs remain on the CPU:

| Stage (per-frame mean) | Location | Time |
|---|---|---:|
| MCRAW RAW decompression + metadata | CPU | ~61 ms |
| Black/white-level calibration | CPU | ~53 ms |
| RCD demosaic (librtprocess) | CPU | ~411–475 ms |
| Camera→DWG + exposure + sharpening + DI encoding (TargetLog RGB) | CPU | ~547 ms (after Phase 7 optimization) |
| RGB→YUV packing + ProRes encoding | **GPU** | Not a bottleneck |

The improvement from 2.52 to 6.95 fps mainly comes from removing CPU
`prores_ks` encoding and YUV packing. The two major hotspots, demosaic and
TargetLog, together still consume about one second per frame on the CPU, which
limits the end-to-end improvement.

### Is the ≤1 LSB parity requirement the main cause?

It is not the main cause, but it has real costs:

1. FP32 is used throughout, and FP16 is explicitly prohibited because no error budget was available (Phase 4).
2. The CPU's deterministic dither hash and its 65,536-entry-per-segment DI LUT must be reproduced bit for bit.
3. To preserve parity, the split was placed after CPU TargetLog RGB generation, leaving the GPU to do only packing. **This conservative split is the largest performance constraint**, rather than precision itself.

## 3. Expected benefit if some precision can be relaxed

1. **Relax acceptance criteria for GPU demosaic and the color chain** (largest benefit): do not require pixel-for-pixel librtprocess RCD output; use tolerance-based acceptance (maximum error, RMSE, percentile, and final 10-bit LSB thresholds). Section 14 of the audit already reserves separate FP32 precise and FP16 fast tracks.
2. **FP16 / mixed precision:** use FP16 for matrix multiplication, sharpening, and chroma filtering to reduce bandwidth and register pressure by half; keep accumulation in FP32 because DI is sensitive in the log curve and shadows.
3. **Evaluate the DI curve analytically inside the shader** instead of using a large LUT. GPU transcendental functions are inexpensive, and this may be faster without sacrificing precision.
4. **Abandon bit-for-bit dither reproduction:** any high-quality GPU noise should be visually indistinguishable.
5. Downgrade the chroma 5-tap quality filter to fast: the benefit is negligible and is not recommended.

These concessions are valuable because they **lower the implementation and
validation barrier for moving the heavy work to the GPU**, not because they make
the existing CPU code faster.

## 4. Full GPU residency / zero-copy status and improvement path

### Current state

- **GPU-to-encoder zero-copy is implemented:** the shader writes `AVVkFrame` directly, synchronized by a timeline semaphore, with no YUV upload or readback.
- **CPU-to-GPU zero-copy is far from implemented:** each frame uploads three FP32 planes, **about 151 MB**, of TargetLog RGB (1.2 GB for eight frames; see Phase 6 telemetry `rgb_upload_bytes=1,207,959,552`).
- The strict meaning of `gpu_resident=true` is only “no uncompressed YUV upload/readback”; it does not mean that the entire pipeline is GPU-resident.

### Improvement path (same priority as Section 11 of the audit)

1. **Move the TargetLog color chain to the GPU:** move the Camera→DWG matrix, exposure, sharpening, and DI encoding into Vulkan compute passes, fused with or chained to the existing packing shader; calculate matrices and white points in CPU FP64 and upload them as uniforms. This removes the largest hotspot, about 547 ms/frame.
2. **GPU RCD demosaic (and merge calibration/unpack):** after completion, the upload falls from 151 MB/frame of FP32 RGB to **25 MB/frame of U16 CFA (about 6× less)**. The CPU then retains only RAW decompression (~61 ms/frame, a theoretical ceiling of ~16 fps that can be hidden by pipelining) and muxing.
3. **Optional MCRAW compression 6/7 GPU decompression:** reduce the upload again to about 11 MB/frame of compressed payload; this is expensive to implement, so decide based on profiling after confirming whether multi-threaded CPU decoding is sufficient.
4. **Supporting work:** keep all intermediate results in VRAM images, with no host round-trip; overlap staging uploads with compute using a dedicated transfer queue.

**Expected scale:** after steps 1 and 2, real-time 4K ProRes HQ (24–30 fps) on an
RTX 3060 is a realistic target. Phase 8 already established that current throughput
is below real time and that the remaining stages are the CPU stages described above.

## 5. Remaining release gates (check these before changing shaders)

From `GPU_PHASE8_PRODUCTION.md` / `GPU_PHASE3_VALIDATION.md`:

- The GPU-assisted validation race in the pinned FFmpeg ProRes DCT shader is unresolved or has not been formally waived.
- Manual acceptance of DaVinci Resolve / Premiere / desktop-player compatibility and chroma siting is incomplete.
- AMD / Intel hardware and second-generation NVIDIA driver coverage is missing.
- A one-hour real conversion / batch resource-growth test has not run (the current 30,000-frame synthetic test covers only 1,000 logical seconds).
- The project-level performance threshold has not yet been agreed.

Until these gates are closed, keep the Vulkan backend opt-in; the CPU backend is the production default and fallback.
