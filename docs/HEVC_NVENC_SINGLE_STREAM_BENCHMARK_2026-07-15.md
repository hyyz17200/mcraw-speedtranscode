# H.265 NVENC Single-Stream Benchmark

Date: 2026-07-15  
GPU: NVIDIA GeForce RTX 3060  
Input: 4096×3072, 30 fps, `testsrc2`  
Target bitrate: 200 Mbps, actual output about 200.009 Mbps

## Test objective

Determine whether a single NVENC encoding stream needs additional software
parallelism, and whether FFmpeg's `-surfaces` option can improve single-stream
throughput. Each configuration ran three times, with 90 frames per run; the
median is reported. Other encoding parameters matched the intermediate-video
benchmark:

```text
preset=p1, tune=ll, rc=cbr, cbr_padding=1
g=30, bf=0, rc-lookahead=0, zerolatency=1
```

## Single-stream results

### HEVC Main 10, 4:2:0

| `surfaces` | Median throughput | Mean cost/frame | Mean NVENC utilization | Three-run throughput |
|---:|---:|---:|---:|---:|
| 0 (automatic) | **50.36 fps** | **19.86 ms** | 45.75% | 50.36 / 52.19 / 48.00 |
| 8 | 47.55 fps | 21.03 ms | 42.58% | 47.55 / 47.20 / 48.79 |
| 16 | 41.77 fps | 23.94 ms | 37.77% | 43.64 / 40.59 / 41.77 |
| 32 | 36.36 fps | 27.50 ms | 26.56% | 36.26 / 36.88 / 36.36 |

### HEVC RExt 10-bit, 4:4:4

| `surfaces` | Median throughput | Mean cost/frame | Mean NVENC utilization | Three-run throughput |
|---:|---:|---:|---:|---:|
| 0 (automatic) | **29.02 fps** | **34.46 ms** | 37.88% | 29.02 / 29.02 / 28.87 |
| 8 | 27.63 fps | 36.18 ms | 34.38% | 27.64 / 27.63 / 27.25 |
| 16 | 24.91 fps | 40.15 ms | 32.75% | 25.23 / 24.90 / 24.91 |
| 32 | 20.57 fps | 48.60 ms | 21.12% | 20.57 / 21.18 / 20.29 |

## Single-stream versus eight-stream aggregate

The eight-stream data comes from independent encoding-process tests using the
same encoding parameters, input, and GPU. It represents aggregate throughput,
not the latency of one video stream.

| Mode | Single-stream median | Eight-stream aggregate median | Aggregate throughput increase | Eight-stream NVENC peak |
|---|---:|---:|---:|---:|
| HEVC Main10 4:2:0 | 50.36 fps | 88.92 fps | +76.6% | 97% |
| HEVC RExt 10-bit 4:4:4 | 29.02 fps | 36.73 fps | +26.6% | 94% |

At eight streams, the per-stream wall-clock frame intervals were about 89.97 ms
and 217.81 ms, respectively. Parallelism therefore improves total GPU
throughput; it does not reduce one stream's end-to-end latency to the aggregate
mean cost per frame.

## Conclusion

- A single NVENC stream on the RTX 3060 does not need additional `surfaces` parallelism; automatic `surfaces=0` was the best single-stream point in this test.
- For 4:2:0, a single stream reached about **50.36 fps**, clearly above 30 fps. Eight streams increased only aggregate throughput to **88.92 fps**; they did not make one stream encode at 88.92 fps.
- For 4:4:4, a single stream reached about **29.02 fps**, close to but slightly below real-time 30 fps. Eight streams reached about **36.73 fps** aggregate, with substantially higher per-stream latency.
- Explicit `surfaces=8/16/32` continued to reduce single-stream throughput. The intermediate-video single-stream configuration should therefore keep `surfaces=0` rather than copying the multi-stream configuration.

## Additional bitrate-sensitivity test

For one 4:2:0 stream with `surfaces=0` and all other parameters unchanged,
`cbr_padding=1` was tested at different target bitrates:

| Target bitrate | Median throughput | Mean cost/frame | Three-run throughput |
|---:|---:|---:|---:|
| 50 Mbps | 47.55 fps | 21.03 ms | 47.42 / 47.55 / 50.49 |
| 150 Mbps | 47.17 fps | 21.20 ms | 45.67 / 47.17 / 48.38 |
| 200 Mbps | 50.48 fps | 19.81 ms | 48.79 / 51.33 / 50.48 |

The 50/150/200 Mbps results do not show a downward trend as bitrate increases.
Within the current test error, the lower single-stream throughput cannot be
attributed to 200 Mbps. Factors more likely to affect a HandBrake comparison
include input resolution and pixel format, CQ/VBR versus CBR, preset, B-frames /
lookahead, AQ, and whether color conversion plus CPU-to-GPU upload time is
included in the encoding time.
