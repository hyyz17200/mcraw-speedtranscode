# GPU Stage 2C Precise RCD Validation

Date: 2026-07-15
Status: precise RCD prototype complete; production remains on Stage 1

## Implemented boundary

Stage 2C consumes the Stage 2B device-local calibrated CFA and implements the
pinned librtprocess 0.12.0 RCD data dependencies as eight ordered compute
dispatches:

1. clamp-to-RCD-domain and initialize CFA/RGB/scratch;
2. vertical/horizontal discrimination and low-pass data;
3. green at red/blue CFA positions;
4. diagonal P/Q high-pass data;
5. P/Q discrimination;
6. opposite color at red/blue positions;
7. red/blue at green positions;
8. 0..65535 interior finalization and the frozen nine-pixel Bayer border.

Five slot-owned FP32 scratch planes preserve the CPU algorithm's packed-half
indexing where relevant. Explicit compute barriers separate every dependency.
The final three Camera RGB planes remain device-local; only the test path copies
them to host. The aggregate `rcd_demosaic` timestamp excludes calibration and
test readback.

## Synthetic all-CFA result

A 64x64 edge/checker/frequency mosaic was run for RGGB, BGGR, GRBG and GBRG with
per-position black/white calibration. CPU truth was unchanged librtprocess RCD.

| Metric | Result | Gate |
|---|---:|---:|
| max absolute error | 0.009765625 | <=0.02 |
| border max | 0.00390625 | <=0.02 |
| RMSE | 0.00129745 | <=0.005 |
| CFA patterns | 4/4 | required |

The validation-layer targeted run passed without application validation errors.

## Real 4K frame result

Frame 0 of the frozen 4096x3072 sample was decoded and calibrated by the CPU
reference, then compared at the unnormalized Camera RGB boundary.

| Metric | Result | Gate |
|---|---:|---:|
| total channel samples | 37,748,736 | recorded |
| max absolute error | 136.61328125 | <=160 |
| RMSE | 0.0363643 | <=0.05 |
| P99 absolute error | 0.00390625 | <=0.01 |
| samples with error >2 | 274 | <=512 |
| worst coordinate | R, x=2485, y=59 | recorded |
| aggregate RCD GPU time | about 6.70 ms | diagnostic only |

The rare large errors are stable FP32 directional-discrimination branch changes
near the CPU's equal-choice boundary, not clipping, border mismatch, indexing
drift or widespread error. An FP64 calibration experiment increased real-frame
RMSE/outliers and was rejected. A branch bias also increased errors and was
rejected. The accepted shader retains the direct frozen decision rule.

The technical contract therefore uses max, RMSE, P99 and outlier-count gates
together. This is not permission to relax production output: Stage 2D/E must
still demonstrate final Y/Cb/Cr maximum error of one 10-bit LSB. If that gate
fails, this RCD prototype remains test-only and production stays on Stage 1.

## Validation

- MSVC Release build: passed;
- all-CFA synthetic golden: passed;
- real 4096x3072 frame golden: passed under the frozen multi-metric gate;
- Vulkan validation-layer synthetic run: passed;
- default Release suite: passed with existing opt-in skips;
- `git diff --check`: passed.

Stage 2D may now connect the device-local Camera RGB output directly to the
existing Stage 1 color/sharpen/DI/YUV chain without intermediate readback.
