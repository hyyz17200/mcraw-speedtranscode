# Batch D.1-B: Effective Vulkan Encoder Async Depth

Date: 2026-07-15
Status: Completed; depth matrix passed.

The vendored FFmpeg port now applies
`prores-vulkan-async-depth.patch`, changing the ProRes Vulkan compute exec
pool from a hardcoded depth of one to the requested `async_depth`. The
application sidecar records requested/effective depth, compute pool size, and
compute queue family/index. The effective depth is reported only after the
real writer/encoder has initialized; capability probing does not claim a
full-resolution encoder was initialized.

The matched matrix used one warm-up plus three formal runs at depth 1, 2, 4,
and 8 on the same Release executable, 240-frame 4096x3072 compression-7
sample, precise configuration, and GPU. All runs reported effective depth
equal to requested depth, 240 direct frames, 240 video packets, zero RGB/YUV
upload and readback, and passed the output/quality checks.

| Requested/effective depth | Median FPS | Formal range | Encoder send mean | Peak VRAM |
|---:|---:|---:|---:|---:|
| 1/1 | 35.370 | 35.326–35.373 | 22.73 ms | 2304 MiB |
| 2/2 | 34.970 | 34.765–34.977 | 13.71 ms | 3121 MiB |
| 4/4 | **35.580** | 35.552–35.655 | 4.98 ms | 3419 MiB |
| 8/8 | 35.560 | 35.443–35.580 | 1.57 ms | 4014 MiB |

Depth 4 is the selected candidate: it is the fastest median, while depth 8
adds memory without a measurable end-to-end gain. The encoder-send timer falls
substantially with depth, but combined E2E remains processing/GPU-bound; this
does not justify queue separation by itself.

Verification on 2026-07-15:

- `git diff --check`: passed.
- Release-only FFmpeg/Vulkan build passed in 4m58s with 73 CTest cases: 66
  passed, 7 skipped for missing Stage 0 assets, and 0 failed.
- The four depth reports are retained under the local test-output benchmark
  directory and were captured at the clean implementation commit.
