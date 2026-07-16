# `prores_ks_vulkan` encoder-only async-depth benchmark

Date: 2026-07-15  
Hardware: NVIDIA GeForce RTX 3060  
Input: 4096x3072, 300 frames, 30 fps, ProRes 422 HQ  

## Scope

This test measures the direct FFmpeg chain:

```text
lavfi color -> yuv422p10le -> hwupload(Vulkan) -> prores_ks_vulkan -> null
```

It excludes MCRAW decode, demosaic, camera color processing, and the project's
Vulkan RGB/YUV writer. Each depth ran three times in a fresh FFmpeg process.
The reported value is the median of 300 frames divided by wall time.

The executable was the system FFmpeg 8.1.2 full build. This is a direct encoder
path check, not a replacement for the project's Release harness: the system
binary is not the vcpkg overlay build used to validate the async-depth patch.

## Results

| async depth | run 1 | run 2 | run 3 | median |
|---:|---:|---:|---:|---:|
| 1 | 77.291 fps | 78.467 fps | 78.129 fps | **78.129 fps** |
| 2 | 78.954 fps | 79.242 fps | 76.809 fps | **78.954 fps** |
| 4 | 78.957 fps | 75.568 fps | 77.866 fps | **77.866 fps** |
| 8 | 76.837 fps | 77.967 fps | 78.690 fps | **77.967 fps** |

Relative to depth 1, the median changes are +1.1% (depth 2), -0.3%
(depth 4), and -0.2% (depth 8). This run does not show a repeatable
encoder-only throughput gain from increasing async depth.

## Correctness check

A separate depth-8 run wrote a 30-frame MOV. `ffprobe` reported ProRes,
4096x3072, and exactly 30 video frames. All benchmark runs exited successfully.

## Interpretation

The earlier end-to-end result and this direct encoder-chain result point in the
same direction for this machine: increasing async depth is not a useful
performance lever at the tested workload. The patch remains valuable as a
correctness/telemetry change because it removes the hard-coded pool-depth
limitation and makes the requested/effective depth observable, but it should
not be expected to improve the current pipeline's throughput.
