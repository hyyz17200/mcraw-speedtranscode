# GPU Stage 3B FP16 Intermediate Storage Validation

Date: 2026-07-15

Status: **GO** for the `balanced` performance mode.

## Boundary

Only the three post-RCD Camera-to-DWG, sharpening, and DaVinci Intermediate
device-local plane sets use packed FP16 storage. Calibration and precise RCD
remain FP32, all shader arithmetic/accumulation and final YUV quantization remain
FP32, the DI implementation remains the approved FP32 LUT, and production still
uploads only U16 RAW and performs no pixel/YUV readback.

The implementation uses core `packHalf2x16`/`unpackHalf2x16`; it does not require
optional native FP16 arithmetic or 16-bit storage features. The sidecar reports
`fp16-storage/fp32-compute` and `intermediate_storage=fp16`.

## Quality result

| Frame | Y/Cb/Cr max LSB | Y/Cb/Cr P99 LSB | Y/Cb/Cr RMSE LSB |
|---:|---:|---:|---:|
| 0 | 1 / 1 / 1 | 1 / 1 / 1 | 0.199 / 0.153 / 0.161 |
| 120 | 1 / 1 / 1 | 1 / 1 / 1 | 0.199 / 0.152 / 0.162 |
| 239 | 1 / 1 / 1 | 1 / 1 / 1 | 0.201 / 0.155 / 0.163 |

This passes the balanced max 4, P99 1, and RMSE 0.5 LSB budget and kept the
observed maximum at one LSB on the fixed real frames.

## Matched performance result

The same Release executable, RTX 3060, 4096x3072 240-frame input, quality
chroma, deterministic dither, precise RCD, and ProRes HQ settings were used.
Each candidate ran one warm-up plus three official conversions.

| Mode | Median fps | Min-max fps | Median wall ms |
|---|---:|---:|---:|
| precise FP32 | 34.901 | 34.868-35.050 | 6,876.618 |
| balanced FP16 storage | 35.577 | 35.570-36.463 | 6,745.873 |

The median gain is 1.938%. The slowest balanced run exceeded the fastest
precise run. Median-of-run GPU stage means for color/sharpen/DI changed from
approximately 0.97/0.99/1.09 ms to 0.69/0.64/0.91 ms. RCD and YUV were
effectively unchanged.

One first-pass balanced run completed all 240 frames and MOV validation but the
sync-drive briefly held the destination during the final rename. A fresh output
directory then passed the full warm-up plus three-run sequence.

Release build, resident MOV, real-frame quality (38 assertions), transfer,
timestamp, and matched benchmark invariants passed. D3 may evaluate DI analytic
independently from this accepted rollback point.
