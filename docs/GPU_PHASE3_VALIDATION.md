# GPU Phase 3 Validation

Date: 2026-07-14
Scope: CPU `yuv422p10le` upload to FFmpeg-owned Vulkan frames, `prores_ks_vulkan`, MOV mux, delayed drain/flush, reopen validation, and telemetry. This is an upload bridge and is not a GPU-resident or final-performance pipeline.

## Environment

- GPU: NVIDIA GeForce RTX 3060
- Driver: NVIDIA 576.02
- Device UUID: `dd5de1eb6f7fc5a0b22b61cc4a7169db`
- FFmpeg: 8.1.2 from the repository-pinned vcpkg baseline and overlay
- Encoder: `prores_ks_vulkan`, profile HQ, alpha disabled

## Automated coverage

- `async_depth` 1, 2, 4, and 8: send/EAGAIN/drain/null-frame flush passed.
- A synthetic 300-frame, 30 fps (10 second) clip was encoded into MOV, reopened, packet-validated, and fully decoded.
- The synthetic output contract is ProRes HQ (`apch`), 64x32, video range, BT.2020 NCL matrix, and unspecified primaries/TRC.
- Telemetry assertions passed: `gpu_resident=false`, `upload_frames=N`, `readback_frames=0`, and `video_packets=N`.
- With `MCRAW_VULKAN_STRESS_ITERATIONS=100`, one process completed 100 create/encode/flush/reopen/decode/destroy cycles (30,000 frames and 61,501 assertions). The post-warm-up private-memory growth guard remained below 128 MiB.
- `scripts/validate-samples.ps1 -ValidateGpuPipeline` checks CPU and Vulkan-upload outputs with ffprobe, software decode, telemetry invariants, and forced-device failure cleanup.

## Real 4K sample

Command:

```powershell
& .\build\msvc-release\Release\mcraw-speedtranscode.exe convert `
  .\mcraw_sample\260710_142121_VIDEO_49mm.mcraw `
  .\test-output\phase3-vulkan-full.mov `
  --config .\config\vulkan-upload-bridge.json --overwrite
```

The available sample is 7.96 seconds rather than 10 seconds. Its complete 240-frame, 4096x3072 conversion passed the application's pre-rename MOV validation and a full FFmpeg software decode.

- wall time: 30.107 s
- throughput: 7.971 fps
- ffprobe: ProRes HQ, `yuv422p10le`, 4096x3072, tv range, BT.2020 NCL, 240 packets, 7.991 s
- audio: 377 chunks, start delta 0.019 ms, end delta 40.288 ms
- transfer counters: 240 uploads, 0 readbacks
- local output SHA-256: `65C1A4BF7C21B4C550A244F253D0DF082893A41BFB6069BB9EE7B71A7C8E7182`

The application reopens every completed partial MOV and verifies codec/profile, color metadata, strictly increasing PTS, positive durations, and packet count before replacing the requested final path.

## Validation-layer finding

`convert --validation` completed and produced a valid, decodable output, but FFmpeg's requested GPU-assisted validation reported repeated shared-memory race diagnostics inside the vendored FFmpeg 8.1.2 shader:

```text
libavcodec/vulkan/dct.glsl:143  read blocks[block][7*stride + offset]
libavcodec/vulkan/dct.glsl:167  write blocks[block][7*stride + offset]
```

Minimal reproduction: encode two 4K frames with `config/vulkan-upload-bridge.json` and `--validation`. The report identifies the same local invocation's read-before-write inside `fdct8`; the caller has workgroup barriers between the row and column passes. No application-owned shader is involved in Phase 3, and no core Vulkan VUID, invalid lifetime, or FFmpeg failure was emitted.

This is not hidden or counted as a clean validation run. Batch F reproduced the
same source locations on driver 610.62 and validation layer 1.4.350, then accepted
the narrowly scoped release waiver in `GPU_BATCH_F_VALIDATION_RACE_WAIVER.md`.
Any other validation diagnostic remains release-blocking.
