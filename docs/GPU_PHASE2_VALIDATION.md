# GPU Pipeline Phase 2 Validation

Phase 2 creates `AVHWFramesContext` pools on the Phase 1 FFmpeg-owned Vulkan
device. The context validates its software format against
`av_hwdevice_get_hwframe_constraints`, requires bounded pool sizes, and requests
storage plus transfer source/destination usage for the future compute path.

## 2026-07-14 local acceptance

- Device: NVIDIA GeForce RTX 3060, driver 576.02
- Software format: `AV_PIX_FMT_YUV422P10LE`
- Hardware format: `AV_PIX_FMT_VULKAN`
- Selected image format: `VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM`
- Image organization: one multiplane `VkImage`
- Required usage accepted: storage, transfer source, transfer destination
- 64×32 CPU upload and readback: exact for all Y, Cb and Cr samples
- Timeline semaphore values increased after upload/readback
- 4096×3072 pool allocation: passed with a bounded two-frame pool
- Odd width rejection: passed
- Release suite: 30/30 tests passed
- Validation-layer upload/readback: 102 assertions passed with no validation
  message or error

Readback exists only in the Phase 2 test path. It is not part of the production
pipeline, and this phase makes no GPU-resident or performance claim. The next
phase may use this frame context for the explicit CPU upload bridge.
