# GPU Phase 5 Validation

Date: 2026-07-14

## Scope

This phase removes the uncompressed YUV transfer at the application/encoder
boundary. `VulkanRgbToYuvFrameWriter` allocates an `AV_PIX_FMT_VULKAN` frame
from the encoder's exact `AVHWFramesContext` pool, creates storage views for
its Y, Cb, and Cr planes, and writes the Phase 4 RGB-to-YUV result directly to
that frame. The same `AVFrame` is then passed to `prores_ks_vulkan`.

The production handoff does not call `av_hwframe_transfer_data()`. A separate
golden test deliberately downloads one frame to compare raw codes with the CPU
reference; that diagnostic transfer is outside the production writer and is
not included in its telemetry.

## Ownership and synchronization

- FFmpeg owns the logical device, frame pool, images, memory, and timeline
  semaphores.
- The application accepts only frames from the exact encoder pool.
- The frame pool is created with storage-image usage and queried rather than
  replacing its image allocation.
- The application locks `AVVkFrame`, waits each image's current timeline
  value, signals the value plus one, and publishes the resulting `GENERAL`
  layout, shader-write access, timeline value, and compute queue family.
- Queue submission uses FFmpeg's owner-provided queue mutex, so an application
  submission cannot race an encoder submission to the same `VkQueue`.
- A per-dispatch fence protects the transient image views, descriptor set,
  command buffer, and RGB staging buffers. There is no per-frame
  `vkQueueWaitIdle()`. Removing this fence through bounded slots is Phase 6 / Task 8.
- The encoder keeps the submitted `AVFrame` reference for as long as libavcodec
  needs it; frame-pool reuse remains under FFmpeg control.

The currently selected RTX 3060 frame pool uses one multi-planar
`VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM` image. The shader writes three
`VK_FORMAT_R16_UINT` plane views, matching the 16-bit code storage consumed by
the pinned FFmpeg encoder. Other devices fail explicitly if the queried pool
cannot provide storage-compatible images.

## Golden and encoder tests

The direct-frame golden test uses the Phase 4 adversarial patterns and verifies:

- all Y/Cb/Cr codes remain within 1 LSB of the CPU reference;
- the pool allocation contains one encoder-owned image;
- layout is `GENERAL`, access is shader-write, and the timeline value advances;
- production telemetry remains `yuv_upload_frames=0` and
  `yuv_readback_frames=0`.

The encoder test runs a changing 300-frame / 10-second sequence through
shader, frame pool, `prores_ks_vulkan`, and the software ProRes decoder. It
checks packet count, packet PTS, decoded frame count, strictly increasing
decoded PTS, and at least 290 distinct decoded luma hashes. Telemetry asserts:

```text
gpu_resident=true
direct_frames=300
upload_frames=0
readback_frames=0
```

With `MCRAW_VULKAN_DIRECT_STRESS_FRAMES=30000`, the same process encoded and
decoded 30,000 frames (1,000 seconds at 30 fps) with 180,011 assertions and no
old-frame recurrence, random corruption, synchronization failure, or resource
lifetime failure.

## Validation layer and remaining boundary

With `MCRAW_VULKAN_SHADER_VALIDATION=1`, the direct image-write, transition,
timeline, and diagnostic readback test completed without an application
ownership, layout, lifetime, or synchronization diagnostic. The unresolved
GPU-assisted race report inside FFmpeg's own ProRes DCT shader remains recorded
in `GPU_PHASE3_VALIDATION.md`; this phase does not hide or waive that production
gate.

`gpu_resident=true` describes the uncompressed YUV frame handoff required by
the audit invariant. The current API still uploads three CPU-produced FP32 RGB
planes into host-visible Vulkan staging buffers, reported separately as
`rgb_upload_bytes`. Moving RAW decode, demosaic, color, and sharpening ahead of
this point onto Vulkan is a later pipeline phase. No claim of zero host-to-GPU
source traffic is made here.
