# GPU Stage 2D Resident RAW Chain Validation

Date: 2026-07-15

## Result

Stage 2D is accepted. Production Vulkan conversion now submits decoded U16 CFA
data and normalized metadata directly to the bounded Vulkan writer. Calibration,
the eight-pass precise RCD implementation, camera-to-DWG, capture sharpening,
DaVinci Intermediate encoding, YUV packing and ProRes frame-pool handoff execute
in one command buffer without an intermediate CPU RGB upload or GPU readback.

Only precise RCD is supported by the Stage 2 Vulkan path. Automatic selection
falls back to CPU before output creation for another configured demosaic;
forced Vulkan fails with `unsupported_format`.

## Production resource and transfer contract

Each resident slot owns its U16 upload buffer, calibrated CFA, three Camera RGB
planes, five RCD scratch planes and the existing Stage 1 intermediate buffers.
The RAW and Stage 1 descriptors share those device-local planes directly.

The three-frame production E2E test measured:

- `pipeline.entry = raw_mosaic_u16`;
- `demosaic_location = gpu_rcd_precise`;
- U16 upload bytes exactly `width * height * 2 * frames`;
- FP32 RGB upload bytes `0`;
- YUV readback frames `0`;
- one calibration, RCD, color, sharpening, DI and YUV timestamp sample per frame;
- one four-byte control-status read per frame and zero status failures;
- a valid, reopenable and decodable ProRes HQ MOV.

Sidecar and CLI JSON now expose `raw_calibration` and `rcd_demosaic` GPU stage
timings in addition to the Stage 1 timing chain.

## Validation

- Release build of `mcraw_tests` and `mcraw-transcoder`: passed.
- Vulkan validation-layer resident RAW E2E: 33 assertions passed.
- Unsupported-demosaic selection test: 4 assertions passed.
- Full Release CTest: 71/71 tests passed, with six existing opt-in real-sample
  tests skipped.
- `git diff --check`: passed.

Stage 2E may now run real-frame final-YUV gates, full conversion stability and
matched performance benchmarks against the accepted Stage 1 executable.
