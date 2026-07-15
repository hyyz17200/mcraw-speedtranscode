# GPU Stage 3C Analytic DaVinci Intermediate Validation

Date: 2026-07-15

Status: **GO** for `fast`; `balanced` keeps the approved FP32 LUT.

The sole changed variable was DI evaluation: fast uses the analytic FP32
piecewise equation after the accepted FP16 storage path. Precise RCD, FP32
arithmetic/quantization, quality chroma, deterministic dither, ownership, and
transfer behavior were unchanged.

Frames 0/120/239 each measured Y/Cb/Cr max and P99 of 1 LSB. RMSE ranges were
0.199-0.201 Y, 0.152-0.155 Cb, and 0.161-0.163 Cr, well inside the fast budget.

Matched one-warm-up/three-official-run results:

| Mode | Median fps | Min-max fps | Median wall ms | DI mean median |
|---|---:|---:|---:|---:|
| balanced FP32 LUT | 35.577 | 35.570-36.463 | 6,745.873 | 0.907 ms |
| fast FP32 analytic | 36.770 | 36.468-36.778 | 6,527.024 | 0.497 ms |

The end-to-end gain is 3.353%; analytic DI reduced its stage mean by 45.2%.
The slowest fast run remained above the fastest balanced run. Sidecars identify
`performance_mode=fast`, `intermediate_storage=fp16`, and
`di_implementation=fp32_analytic`.

D4 must evaluate dither independently from this commit.
