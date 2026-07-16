# `prores_ks_vulkan` Encoder-Only Benchmark

Date: 2026-07-15  
Hardware: NVIDIA GeForce RTX 3060  
Input: 4096×3072, 90 frames per stream, ProRes 422 HQ  
Encoder: FFmpeg `prores_ks_vulkan`, `async_depth=1`

## Test method

FFmpeg generated fixed gray video frames, which were passed directly to
`prores_ks_vulkan` after `format=yuv422p10le,hwupload`. The test excludes MCRAW
decompression, demosaic, color grading, and all other project processing.

Each concurrency level ran three consecutive tests; the median is reported. GPU
utilization and VRAM were sampled with `nvidia-smi` every 100 ms. GPU utilization
is a device-level sample.

## Median results

| Parallel encoder processes | Throughput | Aggregate mean cost/frame | Mean frame interval per stream | Mean GPU | GPU peak | VRAM peak |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 45.16 fps | 22.14 ms | 22.14 ms | 53.67% | 89% | 2,429 MiB |
| 2 | 53.25 fps | 18.78 ms | 37.56 ms | 69.71% | 97% | 3,155 MiB |
| 4 | 55.93 fps | 17.88 ms | 71.51 ms | 75.26% | 97% | 4,579 MiB |
| 8 | 56.61 fps | 17.66 ms | 141.31 ms | 75.75% | 97% | 7,478 MiB |

“Aggregate mean cost/frame” is calculated over the total frames produced by all
parallel processes. “Mean frame interval per stream” is the wall-clock duration
of each independent encoding stream; the two metrics have different meanings.

## Three raw runs

| Concurrency | Run 1 throughput | Run 2 throughput | Run 3 throughput | Throughput variation |
|---:|---:|---:|---:|---:|
| 1 | 45.12 fps | 45.16 fps | 48.40 fps | 7.3% |
| 2 | 52.87 fps | 54.76 fps | 53.25 fps | 3.6% |
| 4 | 55.93 fps | 56.20 fps | 55.53 fps | 1.2% |
| 8 | 57.17 fps | 56.61 fps | 56.20 fps | 1.7% |

## Conclusion

Eight parallel streams achieved the highest throughput in this test, with a
median of **56.61 fps** and an aggregate mean cost of **17.66 ms/frame**. Mean GPU
utilization was about **75.75%**, with a peak of **97%**; the GPU was not saturated
continuously. The benefit of adding more streams should be evaluated against CPU,
VRAM, and scheduling overhead.

The three-run variation was small for four and eight streams, about 1.2% and
1.7%, respectively, so the medians are comparable. The single-stream test lasted
only about two seconds, making startup and sampling overhead more significant and
producing roughly 7.3% variation.
