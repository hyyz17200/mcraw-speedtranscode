# GPU Stage 3E Fast Demosaic Experiment

Date: 2026-07-15

Status: **NO-GO**. All shipped GPU modes retain `gpu_rcd_precise`.

Profiler data justified a bounded prototype because precise RCD remained the
largest application compute pass at about 6.07 ms/frame. A separately named
single-pass bilinear GPU demosaic prototype was evaluated on real frames 0,
120, and 239 before any end-to-end performance run.

| Frame | Y/Cb/Cr max LSB | Y/Cb/Cr P99 LSB | Y/Cb/Cr RMSE LSB |
|---:|---:|---:|---:|
| 0 | 127 / 473 / 108 | 81 / 451 / 89 | 54.8 / 246.5 / 60.0 |
| 120 | 116 / 454 / 103 | 77 / 444 / 80 | 61.3 / 168.2 / 55.2 |
| 239 | 118 / 464 / 105 | 83 / 449 / 87 | 63.8 / 179.8 / 57.4 |

The prototype exceeded the fast max 8, P99 2, and RMSE 1 LSB budgets by wide
margins and showed broad structural chroma error rather than isolated clipping.
Per the Stage 2/3 quality stop condition, no performance benchmark was run.

The prototype shader and selection code were removed before this commit. Fast
continues to mean precise RCD plus accepted FP16 intermediate storage and FP32
analytic DI; the sidecar remains `demosaic_implementation=gpu_rcd_precise`.
