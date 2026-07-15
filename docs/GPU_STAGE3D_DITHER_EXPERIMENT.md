# GPU Stage 3D Dither Experiment

Date: 2026-07-15

Status: **NO-GO**. Precise, balanced, and fast keep deterministic dither.

The experiment changed only `deterministic_dither` from true to false on the
accepted fast FP16-storage/analytic-DI pipeline. Both candidates used one
warm-up and three official 4096x3072 240-frame runs.

| Candidate | Median fps | Min-max fps | Median wall ms | YUV mean median |
|---|---:|---:|---:|---:|
| deterministic dither | 36.770 | 36.468-36.778 | 6,527.024 | 0.660 ms |
| dither disabled | 36.793 | 36.538-37.718 | 6,522.914 | 0.520 ms |

Although the isolated YUV shader became faster, the end-to-end median gain was
only 0.063% and the run ranges overlap substantially. This is below benchmark
noise and does not justify removing dither or spending a quality variable.

No production code or preset semantics changed. Sidecars continue to report
`dither_mode=deterministic` for the shipped precise, balanced, and fast configs.
