# GPU Phase 4 Validation

Date: 2026-07-14

## Scope and fixed math

This phase moves the final RGB-to-YUV packing pass to an application-owned Vulkan compute shader. Its input is planar FP32 RGB already expressed as DaVinci Wide Gamut primaries with the DaVinci Intermediate transfer function. It does not ask the ProRes encoder to perform an implicit color conversion.

The shader fixes the same contract as the CPU reference:

- matrix coefficients: BT.2020 NCL, `Kr=0.2627`, `Kb=0.0593`, `Kg=1-Kr-Kb`;
- luma legal-range mapping: `64 + 876 * Y`, clipped to 64..940;
- chroma legal-range mapping: `512 + 896 * C`, clipped to 64..960;
- chroma phase: even/left sample position;
- quality chroma filter: `[-1, 4, 10, 4, -1] / 16`, with edge-coordinate clamping;
- fast chroma filter: the even/left source sample;
- rounding: positive clipped value plus deterministic dither and 0.5, then integer conversion;
- dithering: the CPU reference's 64-bit hash, reduced to the identical high 24 random bits representable by FP32;
- odd widths: rejected before allocation or dispatch.

The precise mode is FP32. FP16 configuration fails explicitly because no FP16 error budget has been approved.

## Build and ownership

`glslangValidator` from the pinned host vcpkg dependency compiles the GLSL shader for Vulkan 1.3. A CMake script embeds the generated SPIR-V words into the library, so runtime execution does not depend on a loose shader file.

The Phase 4 validation object borrows the FFmpeg-owned logical device, uploads three RGB planes to coherent storage buffers, dispatches one invocation per horizontal pixel pair, waits for completion, and downloads three raw code-value planes. This intentional download exists only for golden comparison. It does not set `gpu_resident=true` and is not yet wired to the encoder.

## Golden tests

The combined test image covers:

- grayscale and Log gradients;
- a saturated single-pixel vertical color line;
- red/blue checkerboard;
- a one-dimensional zone plate;
- saturated hard edges and values outside 0..1;
- mixed nonlinear channel gradients;
- minimum supported width and both horizontal boundaries.

For quality+dither and fast+no-dither modes, every output plane is compared with `pack_dwg_log_to_yuv422p10`:

- maximum absolute error: at most 1 final 10-bit LSB;
- per-plane RMSE: below 0.08 LSB;
- repeated dispatch with the same frame index: bit-identical;
- legal code range and plane sizes: validated.

The high-memory test (`MCRAW_VULKAN_4K_TEST=1`) successfully dispatched 4096x3072 FP32 RGB and produced correctly sized neutral YUV planes.

## Validation layer

With `MCRAW_VULKAN_SHADER_VALIDATION=1`, GPU-assisted, synchronization, and core validation completed the quality-filter golden test without an application-shader error. The Vulkan SDK emitted only its settings warning that GPU-assisted and normal core checks were enabled together, plus loader diagnostics for installed implicit OBS/Bandicam/NVIDIA layers.

The separate FFmpeg DCT GPU-assisted diagnostic recorded in `GPU_PHASE3_VALIDATION.md` is not triggered by this application shader and remains an upstream production gate.
