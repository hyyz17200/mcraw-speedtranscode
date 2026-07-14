# GPU Phase 7 Optimization Report

Date: 2026-07-14

## Profile decision

The Phase 6 real-sample run showed no GPU-slot, job-queue, packet-queue, or mux
backpressure. The GPU queue peaked at 6/10, the packet queue at 1/16, and all
backpressure counters stayed zero. The dominant measured stage was instead the
CPU split path needed to feed the final Vulkan RGB-to-YUV shader:

```text
camera_to_dwg_di_rgb mean: 1,357.388 ms/frame
demosaic mean:                 475.390 ms/frame
end-to-end:                      2.306 fps
```

Therefore this task did not speculate about shader occupancy, descriptor
churn, image layouts, queue count, or mux buffering. It optimized the measured
TargetLog RGB producer only.

## Change

- Camera RGB to DWG matrix evaluation now uses the conversion plan's bounded
  per-frame OpenMP worker count.
- Capture sharpening builds one stable FP32 luma scratch plane, then updates
  the owned RGB planes in parallel. This preserves neighbor semantics without
  copying all three RGB planes.
- The split path moves its owned TargetLinear planes through sharpening and DI
  encoding rather than allocating two additional full RGB images.
- DI encoding uses the existing per-pipeline 65,536-entry segmented LUT in
  parallel, with the same negative policy and analytic fallback above the LUT
  domain.
- CPU-backend fused packing is unchanged.

The code-value regression compares this parallel, moved-storage, LUT path with
the fused CPU reference after sharpening and dithering. All Y/Cb/Cr samples
remain within 1 final 10-bit LSB.

## Matched 4096x3072 comparison

Both runs used the same executable configuration, input, first eight frames,
RCD demosaic, `async_depth=8`, ProRes HQ, audio, and RTX 3060. Only the targeted
optimization differs.

| Metric | Phase 6 baseline | Optimized | Change |
|---|---:|---:|---:|
| End-to-end throughput | 2.306 fps | 3.618 fps | +56.9% |
| End-to-end wall time | 3469.097 ms | 2211.314 ms | -36.3% |
| TargetLog RGB mean | 1357.388 ms | 547.078 ms | -59.7% |
| GPU queue peak | 6/10 | 6/10 | unchanged |
| Packet queue peak | 1/16 | 1/16 | unchanged |
| Backpressure waits | 0 | 0 | unchanged |
| RGB staging bytes | 1,207,959,552 | 1,207,959,552 | unchanged |

Both outputs passed the application's MOV reopen validation and FFmpeg software
decode. Decoded-output comparison between baseline and optimized MOVs reported
average PSNR 87.919 dB (minimum frame 85.454 dB), consistent with the approved
sub-code-value LUT difference rather than a visual pipeline change.

This is a short matched engineering comparison, not a broad hardware
benchmark. The remaining top stages are still CPU TargetLog production and RCD
demosaic. Moving those stages to Vulkan requires new shader parity work and is
not disguised as a Task 9 micro-optimization.
