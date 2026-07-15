# GPU Stage 2B Calibration Validation

Date: 2026-07-15
Status: calibration pass complete; production remains on Stage 1

## Implemented boundary

Stage 2B adds one new GPU pixel semantic:

```text
packed host-visible U16 CFA -> calibrate_raw_fp32 -> device-local FP32 CFA
```

Two U16 samples are unpacked from each 32-bit storage-buffer word, avoiding a
shader-16-bit-storage requirement while preserving exactly `width * height * 2`
source bytes. Per-position black and white values use a fixed 48-byte push ABI.
The shader does not clamp negative or super-white results.

The Stage 2 test resource owns the device-local calibrated plane and an optional
test-only readback. Production resources cannot invoke readback. The production
encoder still enters at Stage 1 Camera RGB; no transfer accounting or sidecar
identity changes in this rollback point.

## Synthetic golden

The Release test uses a 64x32 mosaic for all RGGB, BGGR, GRBG and GBRG phases,
four fractional black/white pairs, a below-black sample and a super-white
sample. CPU truth is unchanged `calibrate_raw_for_demosaic()`.

| Metric | Result | Gate |
|---|---:|---:|
| max absolute error, 0..65535 domain | 0.0078125 | <=0.01 |
| RMSE | 0.00137959 | <=0.002 |
| negative preserved | yes | required |
| super-white preserved | yes | required |
| CFA phases | 4/4 | required |

The measured difference is caused by CPU FP64 division followed by FP32 storage
versus portable shader FP32 arithmetic. The technical design now freezes this
measured intermediate budget; final Stage 2 YUV remains limited to one LSB.

## Timestamp and resource evidence

The targeted run reported four calibration timestamp samples with a positive
GPU total (`0.063136 ms` in this small synthetic run; min `0.005376 ms`, max
`0.046016 ms`). Timing is correctness/contract evidence only, not a performance
claim for the full frame.

Two-slot capacity assertions passed for U16 upload, calibrated CFA, three Camera
RGB planes, five RCD scratch planes and separate U16/FP32 test readbacks. The
existing exact U16 two-slot round trip continues to cover all four CFA patterns.

## Validation

- MSVC Release build: passed;
- targeted Stage 2A/2B tests: passed;
- Vulkan validation-layer targeted calibration run: passed without application
  validation errors;
- default Release suite: passed; opt-in real/4K tests retain their existing skip
  behavior;
- `git diff --check`: passed.

Stage 2C may now add precise RCD passes that consume the calibrated buffer.
