# v0.1 Architecture Decision Record

Status: frozen on 2026-07-13 for Phases 0–2.

| ADR | Decision | Consequences and validation |
|---|---|---|
| 001 License posture | The whole application uses GPLv3-or-later | Direct static or dynamic linking to GPLv3 librtprocess is allowed; distribution must satisfy GPL obligations |
| 002 Official decoder | MotionCam decoder `release/0.2` is the CPU RAW ground truth for compression 6/7 | The custom index exposes only compressed payloads; RAW decompression must call the official implementation |
| 003 GPU API | Vulkan is the future primary GPU API; v0.1 contains no GPU implementation | Any future GPU decompression must be pixel-for-pixel bit-exact |
| 004 Color anchor | XYZ D50 is used between the camera and output profiles | Paths with and without ForwardMatrix are tested separately |
| 005 Dual illuminant | Iterate CameraNeutral→xy; interpolate matrices by inverse CCT | FP64, a 50-iteration limit, and an `1e-10` xy convergence threshold |
| 006 Demosaic ABI | High-quality demosaic calls librtprocess through an explicit enum/function boundary | RCD is the default; AMaZE/IGV/DCB/LMMSE are replaceable; no separate FCS is added |
| 007 Log | The analytic formula is authoritative; the production path generates a per-conversion LUT from the formula | Only DaVinci Intermediate is frozen for v0.1; the final 10-bit output must differ from the reference by at most 1 LSB |
| 008 ProRes | Use FFmpeg libavcodec/libavformat `prores_ks` | Do not implement the ProRes bitstream; input is yuv422p10le |
| 009 Packing | Video range, BT.2020 NCL, left siting, and the quality 5-tap filter | CPU/Vulkan frames and the encoder context are all marked left; MOV readback does not preserve chroma location; primaries/TRC are unspecified, and the DWG/DI identity is stated explicitly in the sidecar |
| 010 Timing | Preserve the source frame-to-frame timing relationship by default | Use the nanosecond source clock as the authority and convert it to the 90 kHz MOV video time base |
| 011 Negative values | Default to `preserve_by_curve` | The DI linear toe preserves negative values; clipping occurs only at the YUV quantization boundary |
| 012 Determinism | CPU FP32 pixels, FP64 setup/reference, and deterministic dither | Hidden approximations are disabled by default; each stage is timed independently |
| 013 FCS removal | Do not implement separate false-color suppression | The current full-frame median implementation has insufficient benefit and accounts for about 60% of the original runtime; rely on demosaic quality itself |
| 014 CPU execution | Bounded multi-frame parallelism with a total thread budget and RAM awareness | Default `0=auto`; collect results in order and then mux by source timestamp to prevent out-of-order output |
| 015 Detail processing boundary | Linear DWG Capture Sharpening defaults to `0.4`; the transcoder does not denoise | Sharpening compensates for detail loss in the capture and decode chain; noise processing belongs in color grading |
| 016 FFmpeg ProRes GPU-AV waiver | Grant a limited release waiver for the same-invocation shared-memory race diagnostic in pinned FFmpeg 8.1.2 `dct.glsl` | Covers only the recorded diagnostics on lines 143/167; core VUIDs, application shader diagnostics, and output failures are not covered; retest after upgrading FFmpeg or the validation layer |

## Compatibility rule for a missing MotionCam illuminant

The official decoder example writes `ColorMatrix1` as DNG illuminant 21 (D65)
and `ColorMatrix2` as illuminant 17 (Standard Light A). When an MCRAW file
contains both matrices but lacks illuminant fields, v0.1 follows this official
compatibility convention and records an explicit warning in `inspect`, the logs,
and the sidecar; it never fabricates the value silently.
