# MCRAW → High-quality ProRes transcoder: Route B architecture and implementation specifications

> Document Purpose: As the overall design basis for VS Code/Codex projects.  
> Current stage: Phase 0~2 CPU path has been implemented and parallel optimization has been completed; the GPU stage has not yet started.  
> Project name: `MCRAW SpeedTranscode` (`mcraw-speedtranscode`).

---

## 1. Project goals

Develop a high-quality, high-performance transcoder specifically for MotionCam `.mcraw`:

1. Directly parse MCRAW containers and frame-by-frame metadata.
2. The CPU reference decoder must be bit-exact with MotionCam's official decoder.
3. Provide a Vulkan GPU MCRAW decompression path in a later phase.
4. Provides a variety of high-quality Bayer demosaicing algorithms:
   - AMaZE
   - RCD
   - IGV
   - DCB
   - LMMSE
   - Extensible to other algorithms
5. Nodes such as black level, white level, lens correction and bad pixel repair are configurable.
6. Strictly implement DNG dual matrix and dual light source color temperature adaptation, and do not use unclear simplified interpolation.
7. Accurately implement linear Log/gamut combinations for common scenarios, including:
   - F-Log/BT.2020
   - S-Log2 / S-Gamut
   - S-Log3/S-Gamut3
   - S-Log3 / S-Gamut3.Cine
   - DaVinci Intermediate / DaVinci Wide Gamut
   - ARRI LogC3 / ARRI Wide Gamut 3
8. Use FFmpeg/libavcodec to encode ProRes and do not implement the ProRes encoder by yourself.
9. Preserve audio, timestamps, and necessary metadata.
11. All high-overhead steps can be independently measured, turned off, or replaced.

---

## 2. Clear non-goals

Not to do in the first stage:

- Non-linear "beautification" or automatic stylization.
- Display transformation, HDR tone mapping, Rec.709 film look.
- AI Demosaic or AI Noise Reduction.
- RAW, chroma or temporal noise reduction; noise processing is left to post-production color correction.
- Implement ProRes code stream by yourself.
- Depends on DNG intermediate files.
- Embed the entire vkdt or RawTherapee framework into your project.
- Pursue GPU-ization of all algorithms from the beginning.
- GUI first; complete stable CLI and library interfaces first.

The project should output a "digital negative ready for further color correction" rather than automatically stylizing the finished film.

---

## 3. Core design principles

### 3.1 Correctness takes precedence over fusion optimization

Each stage first establishes an independent and verifiable reference implementation, and then considers:

- kernel fusion;
- FP16;
- fast-math;
- Approximate curve;
- LUT replaces analytical formula;
- GPU-specific optimizations.

The default quality mode must not use approximation algorithms without error quantization.

### 3.2 Separation of format analysis and image processing

MCRAW containers, frame index, audio, JSON metadata belong to the I/O layer.

RAW decompression, correction, demosaicing, color conversion, and Log encoding belong to the processing layer.

ProRes, MOV, and audio multiplexing belong to the output layer.

The three layers cannot depend on each other for internal implementation details.

### 3.3 Nodeization instead of main process heap conditional branch

Each processing function should be a node with clear input and output formats. The node has:

- name and version;
- Input and output pixel types;
- CPU/GPU backend capabilities;
- Whether to process it in situ;
- Boundary/halo requirements;
- Whether to support chunking;
- Is it certain?
- Estimated video memory and memory;
- Timing statistics;
- Serializable parameters;
- Verification rules.

The configuration file just builds the node graph and there should be no hidden behavior.

### 3.4 Reference path and high-speed path coexist

Two paths remain:

- **Reference**: CPU, FP32/FP64, no fast-math for easy verification.
- **Accelerated**: Vulkan, asynchronous pipeline, optional controlled approximation.

The high-speed path must be comparable stage by stage with the reference path.

---

## 4. Recommended technology stack

### 4.1 Primary language and build system

- C++20
- CMake
- Windows 11 and Linux are the primary platforms
- Vulkan 1.2/1.3 as GPU backend
- SPIR-V shader
- FFmpeg/libavcodec/libavformat/libswresample
- JSON configuration and metadata parsing
- Unit testing framework: Catch2 or GoogleTest
- Benchmark test: Google Benchmark or self-built stable benchmark runner

### 4.2 External components

| Components | Purpose | Principles |
|---|---|---|
| MotionCam official `motioncam-decoder` | Containers, indexes, metadata, CPU reference decoding | As a compatibility baseline |
| FFmpeg | ProRes, MOV, PCM/audio multiplexing | No rewriting of encoding and packaging |
| Vulkan | GPU RAW decompression and image processing | Main acceleration backend |
| librtprocess | AMaZE/RCD/IGV CPU reference implementation | Affected by GPLv3, you need to decide on the license first |
| OpenColorIO Config ACES/CLF | Validation source for accurate color transformations | Not required as a runtime dependency by default |
| Adobe DNG SDK/Specification | Specification basis for dual matrix color model | For testing and behavioral comparison |

---

## 5. License pre-decisions

`motioncam-decoder` is Apache-2.0; the license of FFmpeg depends on the specific build; `librtprocess` is GPLv3 or GPLv3+ as a whole; different license files exist in the vkdt repository and must be confirmed file by file.

### Recommended to choose one of the two

#### Solution L1: The entire application adopts GPLv3

Advantages:

- Can link directly to librtprocess.
- GPL-compliant code can be reused more freely.
- The fastest way to get mature AMaZE, RCD, and IGV.

Disadvantages:

- The entire combined program must comply with GPL obligations when distributed.
- Greater restrictions on future closed source or commercial integration.

#### Scenario L2: Core remains permissive

Requirements:

- Do not copy or link directly with GPL demosaiced code.
- Reimplemented based on public papers and algorithm descriptions.
- OpenColorIO/ACES CLF can be used as a reference for testing, but still need to check specific file permissions and copyright headers.
- vkdt code can only be used after per-file permission confirmation.

**Important:** Dynamic plug-ins or standalone processes do not automatically circumvent GPL obligations. The license structure should be decided before writing a large amount of code; seek professional legal advice if necessary.

### Practical development suggestions

If the goal is firstly self-use and open source, adopting GPLv3 will save the most time.  
If closed source distribution is possible in the future, choose a loose core from day one and use librtprocess only for local control testing and not enter the release build.

---

## 6. Overall architecture

```text
Application/CLI
│
├── mcraw_io
│ ├── container reader
│ ├── frame index
│ ├── metadata normalization
│ ├── compressed frame reader
│ └── audio reader
│
├── raw_reference
│ └── official CPU decompression adapter
│
├── processing_graph
│ ├── raw calibration nodes
│ ├── demosaic plugins
│ ├── color science
│ ├── target gamut / transfer function
│ └── YCbCr / chroma subsampling
│
├── backend_cpu
│ ├── scalar reference
│ ├── SIMD/OpenMP
│ └── optional librtprocess
│
├── backend_vulkan
│ ├── compressed RAW unpack
│ ├── GPU correction nodes
│ ├── GPU demosic implementations
│ ├── color transforms
│ └── output packing
│
├── media_output
│ ├── ProRes encoder
│ ├── MOV muxer
│ ├── audio
│ └── sidecar metadata
│
└── validation
    ├── frame compare
    ├── transfer-function tests
    ├── matrix tests
    ├── codec round-trip
    └── stage benchmark
```

---

## 7. Module responsibilities

### 7.1 `mcraw_io`

Responsibilities:

- Open and verify MCRAW.
- Read container version.
- Indexing of frames, audio and metadata.
- Provides thread-safe random reads.
- Return compressed frame bytes instead of forcing CPU to decompress.
- Keep the original JSON and generate a standardized strongly typed structure.
- Compatible with old field spellings, such as the historical `sensorArrangment`.
- Not responsible for color processing.

If the official decoder API can only directly return decompressed frames, a thin adaptation layer or minimum fork should be added to allow the GPU path to obtain the original compressed payload. The official CPU decoder remains as a reference.

### 7.2 `raw_reference`

Responsibilities:

- Call the official CPU decompression.
- Provides benchmarks for formats such as compression type 6/7.
- Output original U16 Bayer without black level processing.
- For automatic verification of GPU decompression results.

### 7.3 `processing_graph`

Responsibilities:

- Build directed acyclic graph based on configuration.
- Verify whether the node sequence is legal.
- Automatically insert necessary format conversions.
- Estimate memory, video memory and queue depth.
- Collect the time consumption, throughput and errors of each node.
- Allow test frames to be exported from intermediate nodes.

### 7.4 `backend_cpu`

Responsibilities:

- Correctness reference.
- High-quality CPU demosaicing.
- Full fallback when GPU is not available or not supported by GPU.
- SIMD/OpenMP is available, but must remain in deterministic mode.

### 7.5 `backend_vulkan`

Responsibilities:

- Compressed data upload.
- GPU MCRAW decompression.
- Optional RAW node.
- GPU demosaicing will be gradually implemented in the future.
- Color matrix, Log, YUV and quantization.
- Use persistent mapping staging ring and multi-frame asynchronous pipeline.

### 7.6 `media_output`

Responsibilities:

- Receive RGB log or YUV422P10.
- Use FFmpeg `prores_ks` or the proven ProRes encoder.
- MOV packaging.
- Audio alignment.
- Write expressible color metadata.
- Generate full sidecar JSON.

---

## 8. Pixel data types and stages

Explicit types should be used, and the use of ambiguous "float image" is prohibited.

| Type | Meaning |
|---|---|
| `CompressedFrame` | MCRAW original compressed frame |
| `RawMosaicU16` | The decompressed sensor code value, without blackening |
| `RawMosaicF32` | Bayer converted to float, can include negative values and super white |
| `RawNormalizedF32` | Complete black level and white point scale normalization by CFA channel |
| `CameraRGBF32` | Camera native linear RGB after demosaicing |
| `XYZD50F32` | Scene linear XYZ D50 obtained according to DNG model |
| `TargetLinearRGBF32` | Scene linear RGB in the target gamut |
| `TargetLogRGBF32` | Target Log RGB after accurate OETF |
| `YUV444F32` | Nonlinear R'G'B' conversion to Y'CbCr |
| `YUV422P10` | 10-bit planar 422 after filtering, chroma sampling and quantization |

### Precision principle

- RAW, Demosaic, Color and Log default FP32.
- Matrix generation, white point solution, reference curves and test vectors use FP64.
- FP16 can only be used as a standalone `fast` mode and must give error reports.
- Do not crop to `[0,1]` in advance in the intermediate stage.
- Reserve negative values ​​and super white until specific output strategies require limits.

---

## 9. Recommended processing order

```text
MCRAW compressed frame
→ lossless RAW decompression
→ CFA/crop phase resolution
→ black-level subtraction
→ white-level normalization
→ optional static/dynamic bad-pixel correction
→ optional lens shading / flat-field correction
→ optional highlight reconstruction preparation
→ high-quality demosaic
→ optional post-demosaic chromatic aberration correction
→ white balance + DNG dual-illuminant camera transform
→ XYZ D50
→ target-white chromatic adaptation
→ target linear gamut
→ optional exposure offset
→ exact Log OETF
→ RGB-to-YCbCr packing transform
→ high-quality 4:2:2 chroma filtering
→ dither and 10-bit quantization
→ ProRes encoding
→ MOV/audio mux
```

### "Lens Correction" must be split

It is not a single node:

1. **Lens Shading/Flat Field Correction**: RAW mosaic field, before demosaicing.
2. **Lateral Chromatic Aberration**: Usually RGB domain or dedicated RAW algorithm.
3. **Geometric distortion**: RGB domain, after demosaicing.
4. **Crop/Scale**: Pay special attention to the Bayer phase during the CFA stage; generally the final RGB domain is safer.

---

## 10. Black level and white level processing rules

The user interface can display black and white fields as "optional nodes", but from a color science point of view they are not ordinary image quality effects.

### Black level

Applied by file metadata by default:

- Supports 1, 2 or 4 CFA channel black levels.
- Based on actual CFA layout mapping, fixed RGGB is not assumed.
- Allow negative values ​​after blackening.
- No cropping at this stage.
- Supports manual override and diagnostic shutdown.

### White level

The white level is used to convert the sensor code value into a linear scale relative to the scene:

\[
R_\mathrm{norm} = \frac{R_\mathrm{raw}-B_c}{W_c-B_c}
\]

where \(c\) is the CFA color position.

### Three modes

- `metadata`: By default, use MCRAW metadata.
- `manual`: user specified.
- `bypass`: only used for diagnostics or RAW export.

If `bypass` is selected for normal Log/color output, the program should warn that it is better to ask for `force`, because the exposure scale, white balance and log mapping lack reliable physical meaning at this time.

### Highlights

- Do not automatically hard crop at white level.
- If there is a valid super white above the white level, it should be retained.
- Separate masks for saturation detection and highlight reconstruction.
- Each channel saturation threshold can be configured independently.

---

## 11. CFA and cropping phase

MCRAW may record RGGB, BGGR, GRBG, or GBRG.

Must meet:

- Strong typing of original CFA permutations.
- Any odd pixel crop will change the CFA phase.
- The demosaic plug-in receives the "CFA corresponding to the current image origin", not the read-only original container value.
- When tile demosaicing, the local CFA phase of each tile is calculated from the global coordinates.
- Unsupported sensor arrangements should be reported as clear errors, not guesswork.

The first phase only commits to regular 2×2 Bayer. Quad Bayer, X-Trans or special permutations should be considered future capabilities and Bayer algorithms should not be misused.

---

## 12. Demosaic subsystem

### 12.1 Plug-in interface concept

Each demosaicer should declare:

- Algorithm ID and version.
- Support CFA.
- CPU, Vulkan or other backend.
- Enter range requirements.
- Whether to require negative values ​​to be clipped first.
- Border halo width.
- Minimum tile size.
- Whether to support independent tiles.
- Whether internally multi-threaded.
- Is it certain?
- Temporary memory requirements.
- Output channel layout.
- Adjustable parameters and recommended ranges.

### 12.2 The first batch of algorithms

#### AMaZE

Positioning: One of the highest quality candidates, focused on testing details, edges and false colors.

Note:

- The branches and neighborhoods are complex and the GPU porting workload is heavy.
- The first version can be implemented first with mature CPU to establish a golden reference.
- It should not be assumed that it will outperform other algorithms on all materials.

#### RCD

Positioning: Candidate for balancing quality and performance.

Note:

- Suitable as a first GPU high-quality port.
- Still needs to be tested on fine textures, saturated edges and high-noise scenes.

#### IGV

Positioning: Another high-quality candidate, focusing on noise and false color performance.

Note:

- Do not equate algorithm names with fixed scenario advantages.
- The final default should be determined by the test set, not empirically named "best".

### 12.3 Available low-cost algorithms

- Bilinear: only used for preview, debugging and performance floor.
- BayerFast/VNG4: Optional fast mode.
- Bilinear is not allowed as the default high quality mode.

### 12.4 Recommended presets

Presets must simply expose combinations of parameters:

| Default | Suggestions |
|---|---|
| `preview` | Quickly demosaic and turn off expensive repairs |
| `balanced` | Historical preset name; current GPU public configuration is no longer available |
| `quality` | AMaZE, high-quality reconstruction using the algorithm itself |
| `alternate` | IGV, used for user comparison |
| `reference` | CPU, FP32, single definite version |

The default algorithm is marked "tentative" until sample and chart testing is completed.

### 12.5 Chunking and video parallelism

High-quality algorithms may require larger neighborhoods, so:

- Plugin reporting halo.
- The full halo is used internally in the tile, and only the center area is ultimately written.
- The boundary policy must be exactly consistent with the whole frame reference.
- Implement the correct version of the whole frame first, then the tiles.
- Video is naturally suitable for frame-level parallelism; avoiding oversubscription between OpenMP within the algorithm and external multi-frame thread pools.
- Provides a thread budgeter to uniformly allocate I/O, demosaic and encoder threads.

---

## 13. False color suppression (removed)

The first CPU version implemented independent full-frame false-color suppression, but the official sample profile showed that it consumed about 60% of total runtime.
The image-quality benefit did not justify that cost. Product paths no longer expose an FCS node, level, or configuration item; false-color behavior is the responsibility of the selected demosaic algorithm.
Any future reconsideration requires a new ADR, an independent quality corpus, and a performance budget; the old full-frame median implementation must not be restored.

---

## 14. Bad pixel repair

As an optional RAW node, before demosaicing.

### Two sources

1. **Static bad pixel table**
   - From user calibration files or metadata.
   - The lowest cost, recommended to be used first.
2. **Dynamic detection**
   - Compare with CFA neighbors of the same color.
   - Thresholding takes into account local gradients and noise models.
   - Distinguish between hot pixels, dead pixels and real highlighted dots.

### Video Notes

- The dynamic detection threshold must have temporal stability to avoid bad pixel mask inter-frame jumps.
- Can cache stable detection results.
- Interpolation only uses CFA ne…4203 tokens truncated…Persistent mapping staging buffer.
- timeline semaphore.
- Multi-frame in-flight.
- Try to keep compressed data directly into GPU-accessible buffers.
- Avoid decompressing the complete CPU Bayer and then uploading it again.
- Independent timestamp query for each node.
- shader uses explicit precision.
- Correctness phase does not enable `fast-math`.
- Keep the independent kernel first, and then determine the integration based on the profiler.

### 24.4 Real-life Problems of CPU High-Quality Demosaic

If the GPU completes RAW decompression while AMaZE/RCD/IGV is still on the CPU, a GPU→CPU readback will occur.

So in stages:

1. **Correctness Phase**: CPU decompression + CPU demosaic.
2. **GPU decompression verification phase**: Read back after GPU decompression, only for testing.
3. **Hybrid production phase**: Evaluate whether there are still benefits from host-visible buffering and readback.
4. **Full GPU Phase**: Port at least common RCD to Vulkan.
5. AMaZE/IGV may continue to be the "highest quality" option for slower CPUs until GPU version verification is complete.

Don't port complex algorithms prematurely to avoid readback; the cost of incorrect GPU demosaic outweighs the temporary slowness.

### 24.5 Coding bottleneck

After GPU image processing is accelerated, `prores_ks` is likely to become a bottleneck. Therefore the benchmark must measure separately:

- MCRAW I/O
- CPU decompression
- GPU decompression
- black and white field
- bad pixels
- RAW NR
- demosaic
- color/matrix
- Log
- RGB→YUV
- chroma filter
- ProRes
- mux/write

Simply reporting end-to-end FPS is not enough to guide optimization.

---

## 15. Noise reduction boundary

The transcoder does not implement RAW, chroma, or temporal noise reduction. The output is positioned as a digital negative film that can continue to be color graded, noise processing
Leave it to post-grading tools with lens, scene and time context to avoid irreversible texture loss during the transcoding phase.

---

## 16. Lens shading and geometric correction

### Lens shading / flat field

- Located in the RAW domain.
- Apply gain to CFA positions individually.
- If MCRAW has a frame-by-frame or container-level shading map, confirm the field semantics and grid layout before enabling it.
- Do not make guesses just because field names are similar.
- Supports external calibration grid.
- Grid interpolation may use bilinear or higher-quality interpolation.
- Noise amplification should be recorded in large gain areas.

### Geometric distortion

- In the RGB domain.
- Possible via external lens profile system.
- Not required in the initial stage; reserve the interface first.
- After processing, you need to define the output size, cropping and edge filling strategies.

### Horizontal chromatic aberration

- Can be placed separately in RGB domain.
- If RAW-specific CA correction is used, it should be implemented as an alternative and tested to prove that it does not break the CFA details.

---

## 17. MCRAW metadata standardization

The official example contains:

- `width`
- `height`
- `asShotNeutral` per frame
- `blackLevel`
- `whiteLevel`
- `sensorArrangement` or historical spelling
- `colorMatrix1`
- `colorMatrix2`
- `forwardMatrix1`
- `forwardMatrix2`

Other branches or tools may also appear:

- `calibrationMatrix1`
- `calibrationMatrix2`
- `colorIlluminant1`
- `colorIlluminant2`
- Noise model
- lens shading
- crop/active area
- baseline exposure
- per-frame exposure, ISO/gain, timestamp

### Processing requirements

1. Keep the original JSON.
2. Generate `NormalizedCameraMetadata`.
3. Record the source of each standardized field:
   - container
   - frame
   - external override
   - default
4. Check the matrix:
   - number of elements;
   - definition of rows and columns;
   - whether all values are finite;
   - Determinant/condition number;
   - whether it is all zeros;
   - whether the two matrices were accidentally populated with the same values;
   - whether there is a risk of transposition.
5. Missing fields must not be silently fabricated.
6. `inspect` must output complete diagnostics.
7. Create sample libraries for different MotionCam versions.

---

## 18. White balance and dual matrix color model

This part is based on the camera color model of Adobe DNG 1.7.1 as the specification, rather than copying the specific behavior of vkdt.

### 18.1 Basic definition

For each calibration light source there are:

- ColorMatrix \(CM_i\)
- CameraCalibration \(CC_i\), if missing, it is the identity matrix
- ForwardMatrix \(FM_i\), may be missing
- CalibrationIlluminant \(I_i\)
- AnalogBalance \(AB\), if missing, it is the unit diagonal matrix

Frame-by-frame white balance usually comes from `asShotNeutral`, notated as `CameraNeutral`.

### 18.2 Double matrix interpolation rules

DNG 1.2 and later specify:

- Calculate weights from the user white balance and the correlated color temperatures of the two calibrated light sources.
- Perform linear interpolation in **reciprocal correlated color temperature**.
- When the white balance falls outside the range of the two calibrated light sources, clamp it to the nearest matrix.
- Interpolate CM, CC, and FM separately; do not interpolate only the final Camera→RGB matrix.
- Do not interpolate directly from the camera setting's Kelvin value; derive xy/CCT from the white-balance neutral.

Weight can be conceptualized as:

\[
w =
\frac{1/T_\mathrm{WB}-1/T_1}
     {1/T_2-1/T_1}
\]

Then clip to `[0,1]`. The matrix order must be sorted according to the actual low/high color temperature. It cannot be assumed that Matrix1 must be a low color temperature.

### 18.3 Iteration of CameraNeutral → xy

Because the interpolation matrix depends on the white point, and the white point is determined by the CameraNeutral and the matrix, iteration is required:

1. Use a reasonable xy as the initial value.
2. Obtain the double matrix weight from the current xy/CCT.
3. Interpolate to obtain CM, CC, FM.
4. Compute:
   \[
   XYZtoCamera = AB \cdot CC \cdot CM
   \]
5. Derive:
   \[
   XYZ = (XYZtoCamera)^{-1} \cdot CameraNeutral
   \]
6. XYZ to xy.
7. Iterate until convergence.

Requirements:

- FP64.
- Maximum number of iterations.
- Explicit convergence tolerances.
- Record the reason for failure.
- Use stable pseudo-inverse when the matrix is ​​non-square.
- Error handling for anomalous condition numbers and invalid neutral values.

### 18.4 Path with ForwardMatrix

Follow DNG:

\[
ReferenceNeutral=(AB\cdot CC)^{-1}\cdot CameraNeutral
\]

\[
D=\mathrm{Invert}(\mathrm{Diagonal}(ReferenceNeutral))
\]

\[
CameraToXYZ_{D50}=FM\cdot D\cdot(AB\cdot CC)^{-1}
\]

ForwardMatrix already contains the profile designer's D50 mapping intention, so there is no need to do an additional wrong calibration light source→D50 adaptation.

### 18.5 Path without ForwardMatrix

1. Find the inverse:
   \[
   CameraToXYZ=(AB\cdot CC\cdot CM)^{-1}
   \]
2. Use Linear Bradford to fit the selected white point to D50:
   \[
   CameraToXYZ_{D50}=CA\cdot CameraToXYZ
   \]

### 18.6 White balance and exposure must be separated

- White balance changes the proportion of three channels.
- Exposure offset is a linear unity multiplier of the scene.
- Don't mix green normalization, exposure compensation and white balance into the same unexplainable gain.
- Recorded in the log output:
  - Original `asShotNeutral`
  - Find xy, CCT, Duv (if calculated)
  - Matrix interpolation weights
  - Final Camera→XYZ D50 Matrix
  - Exposure shift

### 18.7 Manual white balance

Support:

- `as-shot`
- CCT + tint
- xy
- Direct CameraNeutral
- Direct RGB gains (advanced/diagnostic)

CCT+tint must be converted to xy before entering the unified DNG solution path. Multiple sets of color logic cannot be maintained for different input methods.

---

## 19. Color conversion main path

It is recommended to use `XYZ D50` as the canonical intermediate anchor point between the camera profile and the output color gamut:

```text
Camera RGB linear
→ DNG CameraToXYZ_D50
→ XYZ D50
→ chromatic adaptation to target white
→ target linear RGB
→ exact target Log OETF
```

Most target Log gamuts use D65, so explicit chromatic adaptation from D50 to D65 is required. Suggestions:

- Default linear Bradford.
- Adaptation methods are recorded in metadata.
- Implicit multiplication of D50 XYZ directly by D65 RGB matrix is ​​not allowed.
- The target RGB matrix uses the officially announced matrix; if there are only primaries, the test vector is calculated and fixed by FP64.

For pure transcoding without subsequent edit nodes, there is no need to force a detour through ACEScg.  
If advanced processing of the RGB domain is added in the future, ACEScg or other workspaces can be provided, but the definition of the basic color conversion cannot be changed.

---

## 20. "Log format" must be defined as color gamut + OETF

The user option cannot just be called `S-Log3`, but should be an explicit profile:

- `FLog_BT2020`
- `SLog2_SGamut`
- `SLog3_SGamut3`
- `SLog3_SGamut3Cine`
- `DaVinciIntermediate_DWG`
- `LogC3_AWG3_EI800`

Fixed per profile:

1. Target RGB primaries.
2. White point.
3. XYZ↔RGB matrix.
4. OETF/EOTF exact formula.
5. Negative value strategy.
6. scene-linear benchmark.
7. 18% gray mapping.
8. RGB→YCbCr matrix.
9. packing range.
10. QuickTime/FFmpeg color labeling strategy.
11. Sidecar name and version.
12. Official sources and check vectors.

---

## 21. Precise Log implementation rules

### 21.1 General rules

- Use analytic piecewise formula, not spline fitting.
- Do not use a dense LUT as the reference implementation.
- A high-precision LUT acceleration mode is possible, but it must be compared with the analytical formulas and its error must be bounded.
- CPU reference uses FP64.
- GPU defaults to FP32, disabling aggressive fast-math which can change results.
- Each curve implements forward and inverse.
- Do continuity tests on both sides of the breakpoint.
- Explicit handling of negative input, NaN, Inf and very high super white.
- Generate dense test grids for comparison with official/ACES CLF.
- Record formula version and source document version.

### 21.2 F-Log/BT.2020

Using Fujifilm's official step-by-step formulas and constants:

- `a=0.555556`
- `b=0.009468`
- `c=0.344676`
- `d=0.790453`
- `e=8.735631`
- `f=0.092864`
- linear cut `0.00089`
- log-domain cut `0.100537775223865`

Official reference:

- 0% reflection about 10-bit 95
- 18% reflection approx. 470
- 90% reflective approx. 705
- gamut for BT.2020/D65
- Official information states that F-Log uses full range

During implementation, "Log signal normalization" and "final ProRes YUV range packing" are separated. Just because the original F-Log camera record is full range, we cannot directly assume any MOV/ProRes packing behavior.

### 21.3 S-Log3

Using Sony's official formula:

- Segmentation point: scene linear `0.01125`
- 18% gray should be mapped to 10-bit code 420
- 0% black reference code 95
- 90% reference code 598
- The formula does not change with EI

Must support:

- S-Gamut3
- S-Gamut3.Cine

Both share the S-Log3 OETF, but the gamut matrix is different.

### 21.4 S-Log2

S-Log2 must be implemented separately and bound to S-Gamut.

Implementation rules:

- Does not fit from graphs.
- No backcasting from S-Log3.
- Use official Sony technical documentation, official LUTs, or explicit reference transforms adopted by ACES/OCIO.
- Before getting the traceable formula/transform, the profile is marked as `experimental` and cannot claim to be bit-exact.
- Verify at least Sony reference points:
  - 0% black code 90
  - 18% gray code 347
  - 90% white code 582
- Document S-Log2 version and which Sony implementations are compatible.

### 21.5 DaVinci Intermediate / DWG

Use official Blackmagic specs:

- `DI_A = 0.0075`
- `DI_B = 7.0`
- `DI_C = 0.07329248`
- `DI_M = 10.44426855`
- `DI_LIN_CUT = 0.00262409`
- `DI_LOG_CUT = 0.02740668`

Reference:

- scene-linear 0.18 → 0.336043
- scene-linear 1.0 → 0.513837
- scene-linear 100 → 1.0

Uses official DWG primaries, whitepoints and published matrices, no approximate splines.

### 21.6 ARRI LogC3/AWG3

LogC3 is not a single fixed curve.

Its parameters depend on:

1. Is linear domain sensor signal or exposure value?
2. EI.

For generating digital negatives from MCRAW scene linear data, the **exposure-value** path is used by default.

Design:

- `logc3_domain = exposure_value | sensor_signal`
- `logc3_ei = 160...1600`, expand to higher EI parameters if necessary
- Default `EI800`
- The parameter table comes directly from ARRI official specifications, hand-copied without testing is prohibited
- 18% gray should map to approximately 0.391, which is 10-bit 400/1023
- The output profile name must contain EI, for example `LogC3_AWG3_EI800`

ARRI clearly states that different EI curves are different; it is fixed only for compatibility. EI800 can be used as the default, but it cannot hide this fact.

### 21.7 Negative value strategy

Supported globally:

- `preserve_by_curve`: Use the official linear toe of the curve, and preserve it if it can be encoded.
- `clamp_zero`: Clamp to 0 before entering OETF.
- `soft_floor`: Configurable smooth compression of negative values.
- `error`: Stop when a negative value is found, used for verification.

The default is curve specification.  
Unconditional clamping must not be performed to avoid illegal `log()`.

---

## 22. RGB → YCbCr and ProRes 422

This step must be separate from Log OETF.

### 22.1 Output profile also needs to define packing profile

For example:

```text
color encoding:
    SLog3_SGamut3Cine

packing:
    ProRes422HQ
    10-bit
    video or full range
    YCbCr matrix identifier
    chroma sitting
    chroma filter
```

### 22.2 Mistakes to avoid

- Treat 0–1 of Log RGB directly as Y plane code value.
- Failing to define the RGB→YCbCr matrix.
- Confusing full-range Log curve definition with MOV YUV range.
- Performing 4:2:2 conversion before Log encoding.
- Do standard Y'CbCr directly on scene linear RGB.
- Using nearest-neighbor or box filtering for high-quality chroma downsampling.
- Wrong or missing color labels, but rely on NLE to automatically recognize them.

### 22.3 4:2:2 downsampling

- Performed after nonlinear R'G'B' conversion to Y'CbCr.
- Use a clear low pass filter.
- Define horizontal chroma siting.
- Handle screen borders.
- Independently test saturated color fine lines and high-frequency patterns.
- Provide `fast` and `quality` filters, defaulting to `quality`.

### 22.4 Quantification

- Add controllable dither before FP32/FP64 reaches 10-bit.
- Defaults to using deterministic, fixed seed or frame/coordinate based repeatable noise.
- Avoid fixed streaks or random flickering between video frames.
- Test endpoints separately for legal/full range.
- Measure the proportion of clipped pixels.

### 22.5 Metadata Realistic Limitations

QuickTime/ProRes's standard color tags cannot fully express all "Camera Log + Wide Color Gamut" combinations.

Therefore:

1. Write correct `colr`/nclc/nclx tags as much as possible.
2. Use `unspecified` when it cannot be expressed accurately, instead of writing an incorrect standard.
3. Always write sidecar JSON.
4. The file name or metadata clearly indicates the profile.
5. Conduct an import recognition test in DaVinci Resolve.
6. The documentation explains situations where Input Color Space needs to be specified manually.

Do not assume that a `color_trc` tag is sufficient to represent the full camera color space.

---

## 23. ProRes and MOV output

### Encoding

Preferred FFmpeg `prores_ks`, supports:

- ProRes 422 Proxy
- ProRes 422LT
- ProRes 422
- ProRes 422HQ

Internal input:

- `yuv422p10le`

May be added in the future:

- ProRes 4444
- ProRes 4444XQ
- 16-bit TIFF/EXR test output

Processing architectures should not assume that the final color node must be 422 to allow for future extensions to 4444.

### Time

MCRAW frame timestamps are the source of truth.

Provided by:

- `source_timestamps`: keep actual times.
- `cfr`: Rebuild according to the specified frame rate and explicitly handle missing frames.
- `drop/duplicate/error` strategy.

The default is to first try to preserve source time relationships, but NLE compatibility with the MOV time base needs to be verified.

### Audio

- Preserve sample rate, number of channels and timestamps.
- Output PCM.
- Don't simply assume the audio starting point is the first frame.
- Statistics of A/V start difference and end difference.
- Provide silent filling or cropping strategies.
- Output sync reports.

---

## 24. Performance architecture

### 24.1 Target pipeline

```text
async read/prefetch
    ↓
compressed frame ring
    ↓
Vulkan RAW unpack
    ↓
RAW processing
    ↓
demosic
    ↓
color+log
    ↓
RGB/YUV+4:2:2
    ↓
CPU ProRes encoding
    ↓
async mux/write
```

In steady state:

- GPU processes frame N+1;
- CPU encoding frame N;
- I/O read frame N+2;
- Disk writes older packets.

### 24.2 I/O

- Use `pread`, memory mapping or independent handles to avoid shared seek locks.
- Directly read randomly according to frame index.
- Support prefetching.
- The compressed frame buffer uses a limited ring to prevent unlimited memory growth.
- Log read, wait and page fault times.

### 24.3 Vulkan

- Persistent mapping staging buffer.
- timeline semaphore.
- Multi-frame in-flight.
- Try to keep compressed data directly into GPU-accessible buffers.
- Avoid decompressing the complete CPU Bayer and then uploading it again.
- Independent timestamp query for each node.
- shader uses explicit precision.
- Correctness phase does not enable `fast-math`.
- Keep the independent kernel first, and then determine the integration based on the profiler.

### 24.4 Real-life Problems of CPU High-Quality Demosaic

If the GPU completes RAW decompression while AMaZE/RCD/IGV is still on the CPU, a GPU→CPU readback will occur.

So in stages:

1. **Correctness Phase**: CPU decompression + CPU demosaic.
2. **GPU decompression verification phase**: Read back after GPU decompression, only for testing.
3. **Hybrid production phase**: Evaluate whether there are still benefits from host-visible buffering and readback.
4. **Full GPU Phase**: Port at least common RCD to Vulkan.
5. AMaZE/IGV may continue to be the "highest quality" option for slower CPUs until GPU version verification is complete.

Don't port complex algorithms prematurely to avoid readback; the cost of incorrect GPU demosaic outweighs the temporary slowness.

### 24.5 Coding bottleneck

After GPU image processing is accelerated, `prores_ks` is likely to become a bottleneck. Therefore the benchmark must measure separately:

- MCRAW I/O
- CPU decompression
- GPU decompression
- black and white levels
- bad pixels
- RAW NR
- demosaic
- color/matrix
- Log
- RGB→YUV
- chroma filter
- ProRes
- mux/write

Simply reporting end-to-end FPS is not enough to guide optimization.

---
## 25. Configure the system

JSON or YAML is recommended; it has version number and schema verification internally.

### Top-level concepts

- input
- timing
- raw calibration
- bad pixel
- lens shading
- demosaic
- white balance
- camera profile
- target color encoding
- output packing
- codec
- audio
- performance
- diagnostics

### Principles

- All default values can be displayed in `print-effective-config`.
- After preset is expanded, it is a normal configuration.
- Configuration written to sidecar.
- Illegal combination fails before running.
- Priority of input metadata and overrides is fixed and visible.
- Configure schema with version, which can be migrated in the future.

---

## 26. CLI function planning

### `inspect`

Output:

- MCRAW/container version
- Frame number, timestamp, frame rate statistics
- Resolution, CFA
- compression type
- black/white levels
- matrices
- illuminants
- as-shot neutral range
- Audio
- optional metadata
- Field sources and exceptions

### `convert`

Perform transcoding.

### `extract-frame`

Output the specified stage:

- compressed payload
- raw U16
- normalized Bayer
- linear RGB
- XYZ D50
- target linear RGB
- target Log RGB
- YUV

### `validate`

- CPU official decompression vs custom CPU/GPU
- Matrices and curves
- Pixel-by-pixel comparison of selected frames
- Output difference heatmaps and statistics

### `benchmark`

- warm-up
- Time of each stage
- P50/P95/P99
- Throughput FPS
- CPU/GPU utilization
- Memory/Video memory peak
- Queue waiting ratio

### `list-capabilities`

List:

- demosaic algorithm available
- CPU/GPU backend
- Available Log profiles
- FFmpeg encoder
- Support CFA
- build license/features

---

## 27. Error handling and rollback

### Errors that must be stopped

- The frame index is out of bounds or corrupted.
- Compressed payload lengths are inconsistent.
- GPU decompression is inconsistent with the reference algorithm.
- CFA Unknown.
- Wrong matrix dimensions, NaN/Inf, unacceptable condition number.
- Double matrix has matrix but no explanation illuminant.
- The target profile is missing necessary gamut/OETF definitions.
- Encoder returns corrupted packet.
- Audio and video timestamps are reversed.

### Can warn and roll back

- Missing ForwardMatrix: use ColorMatrix + Bradford.
- Missing CameraCalibration: identity matrix.
- Missing AnalogBalance: identity matrix.
- Only one ColorMatrix: single light mode.
- Missing noise model: Turn off physical RAW NR.
- Missing lens shading data: close this node.
- GPU not available: CPU backend.
- A demosaic without GPU implementation: CPU or user-specified fallback.

All rollbacks are written to the log and sidecar and cannot be silent.

---

## 28. Test system

### 28.1 MCRAW decompression

- compression 6/7 and other actual version samples.
- All CFA.
- Different widths, atypical strides.
- First frame, last frame, random frame.
- CPU official results and custom results must be bit-exact.
- GPU results must be bit-exact.
- Exceptions/truncated files should fail safely.

### 28.2 Metadata

- History field spelling.
- Missing matrix.
- Single matrix/double matrix.
- ForwardMatrix Yes/No.
- White balance changes every frame.
- Exception matrix.
- Metadata reading results are saved as golden JSON.

### 28.3 Demosaic

Create a material set:

- Siemens star
- zone plate
- black and white thin lines
- Slashes and text
- Fabric
- Blades
- roof tiles
- Fence in the distance
- Saturated red and blue LEDs
- Skin color
- Low light and high noise
- Single channel clipping highlights
- bad pixels
- Image borders

Compare:

- AMaZE
- RCD
- IGV
- RawTherapee/librtprocess reference
- DaVinci CinemaDNG results

Metrics can't just be PSNR; look for false colors, zippers, moiré, temporal flicker, and edge shapes as well.

### 28.4 Double matrix color

Unit tests:

- Known identity profile.
- Known to synthesize ColorMatrix.
- Calibration light endpoint weights should be exactly 0/1.
- Press inverse CCT for intermediate color temperature.
- Over range clamp.
- CameraNeutral→xy iteration converges.
- Two paths with/without ForwardMatrix.
- neutral should be neutral when converted to D50.
- Compare to Adobe DNG SDK or trusted reference implementation.
- Generate DNG from the same MCRAW for reference comparison in Adobe/Resolve.

### 28.5 Log Curve

For each curve:

- forward/inverse round-trip.
- Before breakpoint, breakpoint, after breakpoint.
- 0, 18%, 90%, 1.0 and multiple super whites.
- Negative value.
- Dense FP64 mesh.
- GPU FP32 vs CPU FP64.
- Official reference code value.
- ACES CLF/OCIO reference comparison.
- Goal: The error is low enough before quantization so that the known 10-bit points do not exceed 0.5 code value; the reference point rounding results are accurate and consistent.

### 28.6 YUV/ProRes

- Grayscale ramp.
- Saturated RGB patch.
- Single pixel/dual pixel color lines.
- full/video range.
- chroma sitting.
- Code value endpoint.
- FFmpeg decode round-trip.
- DaVinci Resolve import.
- Check waveform, RGB parade, metadata and manual input color space.
- ProRes profile/bit depth/tag verification.

### 28.7 Video Stability

- Still scene checks for demosaic false color and temporal flickering.
- Bad pixel mask stability.
- RAW NR stability.
- Frame timestamps and audio synchronization.
- Long movie memory leak.
- Suspend and resume.
- Output corruption handling.

---

## 29. Performance acceptance method

Fixed for each benchmark:

- Input file checksum.
- CPU/GPU/driver version.
- Compilers and flags.
- backend.
- Algorithm configuration.
- Number of threads.
- Output tray.
- warm-up.
- Whether to write files.
- Whether to enable audio.
- Resolution and frame rate.

Report:

| Phase | ms/frame | FPS Equivalent | CPU% | GPU% | Memory/Video Memory | Wait |
|---|---:|---:|---:|---:|---:|---:|

Three benchmarks are provided:

1. `compute-only`: No encoding, no writing to disk.
2. `encode-only`: input pre-generated YUV.
3. `end-to-end`: complete MCRAW→MOV.

---

## 30. Implementation phase suggestions

### Phase 0: Spec Freeze and Samples

- Build test MCRAW set.
- Fixed official decoder commit.
- Fixed DNG, Sony, Fujifilm, Blackmagic, ARRI spec versions.
- Determine project license.
- Create golden metadata and raw frame.
- Write architecture decision records.

### Phase 1: I/O and reference decompression

- `inspect`
- frame/audio index
- CPU official decode adapter
- raw U16 export
- Timestamp and audio reading
- Abnormal file testing

Acceptance: All samples can be stably analyzed, and official raw can be reproduced.

### Phase 2: CPU High Quality Correct Path

- black/white
- CFA/crop
- RCD
- AMaZE
- IGV
- DNG Dual Matrix
- XYZ D50
- First accurate output profile: DWG/DaVinci Intermediate recommended
- ProRes 422HQ
- sidecar

Acceptance: Single frame colors, curves, and ProRes round-trip are correct.

### Phase 3: Complete Log profile

- F-Log
- S-Log3 two kinds of gamut
- LogC3 EI parameters
- S-Log2 reference will be added after confirmation.
- Color/packing metadata
- Resolve control project

### Phase 4: Vulkan MCRAW decompression

- Refer to vkdt algorithm structure
- compressed payload directly to GPU
- prefix/offset
- decode shader
- bit-exact automatic verification
- Multi-frame ring

Acceptance: All test frames are exactly the same as the official CPU.

### Phase 5: GPU color and output preparation

- black/white
- matrix
- chromatic adaptation
- exactOETF
- RGB→YUV
- quality 422
- dither/quantization
- Asynchronous read/compute/encode/write

### Phase 6: Optional RAW Repair

- bad pixels
- lens shading
- RAW spatial NR
- highlight reconstruction
- CA/geometry interface

Each independent benchmark and quality regression.

### Phase 7: GPU high quality demosaic

Prioritization suggestions:

1.RCD
2. IGV or AMaZE, determined based on previous profile results

GPU algorithms must be compared to a CPU reference on a pixel-by-pixel basis or by explicit tolerances.

### Phase 8: Productization

- stable preset
- Configure schema migration
- Crash recovery
- Log
- Batch processing
- GUI as a thin front-end to CLI/library

---

## 31. Codex work splitting principle

Don't let Codex generate the "full transcoder" in one go. Each task should have:

- A module boundary.
- An input/output contract.
- A correspondence test.
- A completion standard.
- Does not span multiple yet-to-be-validated color stages.

### Recommended task granularity

1. Build the project skeleton and dependency locking.
2. Encapsulate MotionCam decoder.
3. Output standardized metadata.
4. Do CPU raw golden test.
5. Define pixel type and frame buffer.
6. Implement black/white reference node.
7. Adapt a single demosaic.
8. Add two more demosaics.
9. Build CPU thread budget and bounded multi-frame execution interface.
10. Implement DNG neutral→xy iteration.
11. Implement dual matrix interpolation.
12. Implement the ForwardMatrix path.
13. Implement XYZ D50→DWG.
14. Implement DI exact OETF.
15. FFmpeg ProRes single frame encoding.
16. MOV and timestamps.
17. Audio.
18. End-to-end CPU CLI.
19. Vulkan device/context.
20. GPU compressed frame input.
21. GPU decode bit-exact.
22. GPU color/log.
23. GPU YUV422.
24. pipeline overlap.
25. optional correction nodes.

Codex is only required to modify a limited number of files each time, and the test fails first and then passes.

---

## 32. Recommendations for recording architectural decisions

Create `docs/adr/` in the repository:

- ADR-001 License posture
- ADR-002 Official MotionCam decoder role
- ADR-003 Vulkan as primary GPU API
- ADR-004 XYZ D50 as interchange anchor
- ADR-005 DNG dual-illuminant algorithm
- ADR-006 Demosaic plugin ABI
- ADR-007 Exact analytic Log functions
- ADR-008 ProRes via FFmpeg
- ADR-009 YUV range and chroma sitting
- ADR-010 Timestamp policy
- ADR-011 Negative linear value policy
- ADR-012 CPU/GPU determinism and tolerance

Each ADR contains: context, choices, alternatives, consequences, and testing methods.

---

## 33. First version default value suggestions

These default values can be changed after testing:

| Project | Tentative Default |
|---|---|
| black/white | metadata |
| bad pixel | off, can be on when static map is valid |
| lens shading | off, unless reliable metadata/profile |
| RAW NR | off |
| demosaic | RCD (tentative, waiting for testing) |
| white balance | as-shot |
| color transform | DNG dual-illuminant + ForwardMatrix priority |
| target | DWG / DaVinci Intermediate |
| exposure offset | 0 |
| negative policy | preserve_by_curve |
| chroma filter | quality |
| dither | on, deterministic |
| ProRes | 422HQ |
| timing | source timestamps |
| audio | PCM passthrough/repack |
| backend | auto, reference can force CPU |
| precision | FP32 pixels + FP64 setup/reference |

For output digital negatives, noise reduction is left to post-production and geometric correction is turned off by default; black and white field and color calibration are turned on by default.
Because they are fundamental steps in correctly interpreting sensor data.

---

## 34. Things that still need to be confirmed before starting

1. Whether the project adopts GPLv3 or permissive license.
2. Whether to allow direct linking to librtprocess.
3. Whether the first target platform is Windows + NVIDIA.
4. Is Vulkan the only GPU backend, or will CUDA be added in the future?
5. The first version will only be ProRes 422 HQ, or also Standard/LT.
6. Is ProRes 4444 required as a color verification and high quality alternative.
7. The default range and matrix of Log YUV packing need to be finalized through actual measurement of Resolve.
8. The authoritative reference transform of S-Log2 needs to be collected and solidified separately.
9. Fields such as lens shading, noise model, and baseline exposure in MCRAW need to be audited with real samples.
10. Sample library is required for matrix and metadata compatibility of different versions of MotionCam, different mobile phones, and different lenses.

---

## 35. Key Risks

| Risk | Response |
|---|---|
| Misunderstanding MCRAW version differences | Official decoder benchmark + multi-version samples |
| GPU RAW decompression is not bit-exact | Automatic frame-by-frame comparison, failure to prohibit production output |
| High quality demosaic license conflicts | Decide on license before starting construction |
| Double matrix implementation error | According to DNG specification + Adobe/DNG comparison |
| ForwardMatrix repeated color adaptation | Clarify two DNG paths |
| Log curve approximation | Analytical formula + official test vector |
| Log and gamut names are confused | profiles must be named in pairs |
| Confusing full range with ProRes range | Separating encoding and packing |
| 4:2:2 causing extra false colors | High quality filtering + 4444 test outputs |
| ProRes becomes a bottleneck | encode-only benchmark |
| CPU demosaic causes GPU readback | Implemented in stages, prioritizing GPU RCD |
| Flicker between video frames | Still sequence regression test |
| Metadata automatic identification is unreliable | Sidecar + Resolve manual profile testing |

---

## 36. References

### MCRAW

- MotionCam official decoder
  https://github.com/mirsadm/motioncam-decoder
- MotionCam decoder example showing matrices and AsShotNeutral
  https://github.com/mirsadm/motioncam-decoder/blob/main/example.cpp
- vkdt
  https://github.com/hanatos/vkdt

### Demosaic

- librtprocess
  https://github.com/CarVac/librtprocess
- RawTherapee
  https://github.com/Beep6581/RawTherapee

### DNG color model

- Adobe DNG Specification 1.7.1.0
  https://helpx.adobe.com/content/dam/help/en/camera-raw/digital-negative/jcr_content/root/content/flex/items/position/position-par/download_section_733958301/download-1/DNG_Spec_1_7_1_0.pdf

Key chapters:

- One, Two, or Three Color Calibrations
- inverse correlated color temperature interpolation
- CameraNeutral to white-balance xy
- Camera to XYZ D50
- ForwardMatrix path

### Log and gamut specifications

- Blackmagic DaVinci Wide Gamut / Intermediate
  https://documents.blackmagicdesign.com/InformationNotes/DaVinci_Resolve_17_Wide_Gamut_Intermediate.pdf
- Fujifilm F-Log Data Sheet
  https://dl.fujifilm-x.com/support/lut/F-Log_DataSheet_E_Ver.1.2.pdf
- Sony S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3 Technical Summary
  https://pro.sony/s3/cms-static-content/uploadfile/06/1237494271406.pdf
- ARRI LogC3 specification/downloads
  https://www.arri.com/en/learn-help/learn-help-camera-system/technical-downloads
- ARRI ALEXA Log C Curve in VFX
  https://www.arri.com/resource/blob/31918/66f56e6abb6e5b6553929edf9aa7483e/2017-03-alexa-logc-curve-in-vfx-data.pdf
- OpenColorIO Config ACES reference transforms
  https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES

---

## 37. Summary of top-level instructions given to Codex

1. Don’t implement the entire application at once.
2. No guessing on MCRAW fields or color formulas allowed.
3. The official MotionCam CPU decoder is the true value of RAW decompression.
4. The Adobe DNG specification is a dual matrix color truth.
5. Finding the white point of camera neutral requires iteration.
6. Bi-matrix uses inverse CCT weights.
7. There are different paths with and without ForwardMatrix.
8. Log must use the official analytical formula and do not use spline.
9. Log curve and RGB gamut must be defined in pairs.
10. Color encoding and YUV packing must be separated.
11. Do not clamp negative values ​​and super white in advance.
12. High quality demosaic is pluggable module.
13. No independent FCS is provided; false color quality is the responsibility of demosaic selection and regression swatches.
14. Black and white field is turned on by default; bad pixels, lenses and RAW NR can be turned off by default.
15. Any GPU optimization requires CPU reference and automatic comparison.
16. FFmpeg takes care of ProRes and MOV.
17. Each stage must have unit tests, golden data and benchmarks.
18. All fallbacks, overrides and approximations must be recorded to logs and sidecars.
