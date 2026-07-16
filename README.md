# MCRAW SpeedTranscode

English / [简体中文](./README_CN.md)

`mcraw-speedtranscode` is a MotionCam `.mcraw` → intermediate-format transcoding tool that balances speed and quality. It retains the CPU general-purpose reference pipeline while incorporating a GPU-acceleration design.

## Current Status

- This project is still in early development and is not suitable for production use!

## Features

- The official MotionCam decoder as the CPU RAW ground truth for compression 6/7.
- Independent reading and validation of MCRAW frame indexes and compressed payloads.
- RGGB, BGGR, GRBG, and GBRG.
- Black and white levels for each CFA position; negative and above-white values are preserved.
- librtprocess AMaZE, RCD, IGV, DCB, and LMMSE; RCD remains the default.
- Linear DWG Capture Sharpening with a default strength of `0.4`.
- DNG CameraNeutral → xy iteration and inverse-CCT dual-matrix interpolation.
- Two color paths: ForwardMatrix, and ColorMatrix + Bradford.
- XYZ D50 → D65 → DaVinci Wide Gamut.
- Exact analytic DaVinci Intermediate OETF/EOTF, plus LUTs generated from analytic formulas and cached per conversion instance.
- BT.2020 non-constant-luminance RGB→YCbCr, left-sited quality 4:2:2, deterministic dithering, and 10-bit video-range packing.
- FFmpeg `prores_ks` ProRes 422 HQ, source timestamps, PCM audio, and sidecar JSON.
- OpenMP row parallelism, bounded multi-frame parallelism, a multi-context parallel ProRes encoding pipeline, and a CPU/RAM-aware automatic execution plan.
- An FFmpeg-owned Vulkan device, GPU RGB→10-bit 4:2:2, timeline semaphores, bounded frame/job/packet queues, asynchronous Vulkan ProRes encoding, and an independent mux worker.
- Automatic/forced GPU selection, stable UUID/PCI/name selectors, CPU fallback, structured queue/transfer/mux telemetry, and a partial-file strategy that renames files only after successful validation.
- `inspect`, `convert`, `extract-frame`, `validate`, `benchmark`, `print-effective-config`, and `list-capabilities`.

## Environment

- Windows 10 or 11
- An AVX2-capable x86-64 CPU (the default build uses AVX2 as its minimum CPU baseline)
- The Vulkan GPU backend requires support for compute, timeline semaphores, and storage-compatible 10-bit 4:2:2 images; the CPU backend does not require a GPU
- Visual Studio 2022 C++ workload
- CMake 3.25+
- Git
- vcpkg (the repository script can install it into the ignored `.deps/` directory, or an existing installation can be used)

Dependencies are pinned and fetched during configuration:

- MotionCam decoder commit `2c49edb17277c07989ff90bd3a3bf557c2f68b4a`
- librtprocess `0.12.0`
- Catch2 `v3.15.0`
- FFmpeg `8.1.2` (resolved by the fixed vcpkg baseline)
- nlohmann-json uses the version bundled with the pinned MotionCam decoder to avoid mixing ABIs across DLLs

## Build

In “Developer PowerShell for VS 2022”:

```powershell
# Run this once first if vcpkg is not already installed:
.\scripts\bootstrap-vcpkg.ps1

cmake --preset msvc-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The script installs vcpkg `2026.06.24` into `.deps/vcpkg` and sets `VCPKG_ROOT` for the current PowerShell process. If you use an existing vcpkg installation, set an environment variable with the same name yourself; the baseline in the manifest will still pin the port versions.

The executable is usually located at:

```text
build/msvc-release/Release/mcraw-speedtranscode.exe
```

## Usage

```powershell
$exe = '.\build\msvc-release\Release\mcraw-speedtranscode.exe'

& $exe inspect '.\mcraw_sample\sample.mcraw' --raw-json
& $exe validate '.\mcraw_sample\sample.mcraw' --frame 0 --compare-fused
& $exe benchmark '.\mcraw_sample\sample.mcraw' --frames 8
& $exe print-effective-config
& $exe convert '.\mcraw_sample\sample.mcraw' '.\output.mov'
& $exe convert '.\mcraw_sample\sample.mcraw' '.\output-gpu.mov' `
    --config '.\config\vulkan-gpu-pipeline.json'
```

The conversion atomically renames `.partial.mov` to the target file only after the trailer has been written successfully. Existing output is not overwritten by default; add `--overwrite` explicitly when needed.

Start modifying the effective configuration from [config/default.json](config/default.json). The Vulkan opt-in presets are `config/vulkan-precise.json` and `config/vulkan-fast.json`; the CPU backend remains the default. `precise` uses an FP32 intermediate, while `fast` uses FP16 storage + FP32 compute and FP32 analytic DI; both retain precise GPU RCD, quality chroma, and deterministic dithering:

```powershell
& $exe convert input.mcraw output.mov --config '.\config\default.json'
```

Quality options and comparison configuration:

```json
{
  "schema_version": 1,
  "demosaic": "rcd",
  "capture_sharpening": 0.4,
  "capture_sharpening_threshold": 0.002
}
```

`demosaic` can be set to `rcd`, `amaze`, `igv`, `dcb`, or `lmmse`. Capture Sharpening enhances neutral-luminance detail after camera RGB is converted to linear DWG and before the DaVinci Intermediate OETF; the default strength is `0.4`, and setting it to `0.0` disables it. The threshold is likewise applied in the linear DWG domain.

The repository's `config/compare-amaze.json`, `config/compare-dcb.json`, `config/compare-lmmse.json`, and `config/compare-capture-sharpening.json` can be used directly.

`cpu_threads` is the CPU thread budget for the entire process, while `max_parallel_frames` is the maximum number of frames in flight. A value of `0` for either selects automatically. Automatic mode reserves at least two threads per frame, processes at most eight frames concurrently, and uses a conservative budget of no more than one-quarter of the currently available physical memory. On low-memory machines, set these values explicitly:

```json
{
  "schema_version": 1,
  "cpu_threads": 4,
  "max_parallel_frames": 1
}
```

The GPU configuration's `backend` can be `cpu`, `auto`, or `vulkan`. `auto` first completes device initialization, a full-size frame context/encoder setup, and a small shader→encoder smoke test; if it fails, the specific reason is written to the sidecar and it falls back to CPU according to `fallback`. `vulkan` is forced mode: any preflight or runtime error terminates the current partial file, and the pipeline does not switch to CPU partway through the same MOV. `async_depth` controls both the GPU slots and the baseline depth of the bounded queues; the currently validated precision mode is `fp32`, while `fp16` is explicitly rejected.

In the GPU sidecar, `gpu_resident=true` strictly means that the encoder-compatible YUV frame has no CPU upload/readback. `rgb_upload_bytes` separately reports the FP32 RGB staging traffic still produced by the CPU RAW/color frontend; it must not be interpreted as meaning that the complete RAW pipeline has zero PCIe copies.

`--compare-fused` runs both the analytic reference path and the LUT/fused path on the specified frames, and reports the number of differing samples and the maximum code-value error across all 10-bit Y/Cb/Cr samples.

## Documentation

- See the [docs](./docs/index.md) directory for all technical and development documentation.

## Current Boundaries

- Only the first DWG/DaVinci Intermediate profile is implemented.
- ProRes packing is fixed to BT.2020 NCL coefficients, video range, and left-sited 4:2:2; primaries/TRC remain unspecified because no standard MOV enumerations exist for DWG/DI, while the sidecar clearly records the actual profile.
- `validate` uses the official decoder to generate reproducible RAW hashes; a custom/GPU decoder has not yet been added, so there is currently no “official vs. GPU bit-exact” conclusion.
- The transcoder does not perform RAW, chroma, or temporal denoising; noise processing is left to post-production color grading. Bad-pixel correction, lens shading, geometric correction, other Log profiles, and a GUI are outside the v0.1 scope.
- The Vulkan path enters GPU calibration, RCD, camera color, sharpening, DI, YUV, and ProRes processing after CPU official U16 RAW decoding. A specific GPU-AV diagnostic for the FFmpeg ProRes DCT shader has a documented limited waiver; other validation diagnostics still block release.
- `mcraw_sample/` and output files are not committed to version control.
