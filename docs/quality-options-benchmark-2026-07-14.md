# Quality options benchmark — 2026-07-14

## Test conditions

- Source: `mcraw_sample/260710_142121_VIDEO_49mm.mcraw`
- Raster: 4096 x 3072
- Build: MSVC Release
- Compute benchmark: 16 frames, 16 CPU threads, 8 in-flight frames, 2 threads/frame
- Default processing remains RCD, with capture sharpening and RAW chroma denoise disabled

## Demosaic results

| Algorithm | Throughput | Relative to RCD | Mean demosaic stage |
|---|---:|---:|---:|
| RCD, two-run mean | 4.477 fps | baseline | 405.0 ms |
| AMaZE | 3.097 fps | -30.8% | 1219.0 ms |
| DCB, two-run mean | 3.493 fps | -22.0% | 835.2 ms |
| LMMSE, two-run mean | 3.086 fps | -31.1% | 1159.2 ms |

DCB and LMMSE use librtprocess with two iterations. DCB enhancement is disabled.
With concurrent frames, per-stage timings overlap; throughput is the primary total-cost metric.

## Optional processing results

| RCD configuration | Throughput | Relative to same-run RCD |
|---|---:|---:|
| Baseline | 4.450 fps | baseline |
| Capture sharpening 0.25, threshold 0.002 | 3.955 fps | -11.1% |
| RAW chroma denoise 1.0 | 3.799 fps | -14.6% |

The RAW chroma stage averaged 335.4 ms/frame. It is a project-specific spatial
filter driven by DNG `NoiseProfile` S/O values, not an algorithm standardized by
the DNG specification. The tested file contains four per-CFA S/O pairs on every
frame, so it requires no sidecar metadata. An enabled filter fails explicitly if
another source has no usable profile.

## Full comparison transcodes

- `test-output/sample-full-amaze.mov`: 240 frames, 2.217 fps end-to-end
- `test-output/sample-full-rcd-sharpen-0.25.mov`: 240 frames, 2.655 fps end-to-end

Both outputs are 4096 x 3072 ProRes 422 HQ (`yuv422p10le`) with 48 kHz stereo
PCM. `ffprobe` reported 240 video frames and FFmpeg decoded all streams without
errors.

Against the existing default-RCD `sample-full-optimized.mov`, decoded-video PSNR
is 61.30 dB average for AMaZE and 63.63 dB average for RCD + capture sharpening.
These figures only confirm that the option materially changes the encoded image;
they are not quality scores and do not replace a Resolve/DNG visual comparison.
