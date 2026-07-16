# Batch F: FFmpeg ProRes GPU-AV Waiver

Date: 2026-07-16
Status: accepted, narrowly scoped release-gate waiver

## Finding

The pinned FFmpeg 8.1.2 `prores_ks_vulkan` DCT shader continues to produce a
GPU-assisted-validation shared-memory race diagnostic at `dct.glsl:143` and
`dct.glsl:167`. The diagnostic reports a read and write by the same local
invocation. The shader caller has workgroup barriers between its row-wise and
column-wise DCT passes.

The finding was reproduced on the current machine with:

- NVIDIA GeForce RTX 3060, driver 610.62;
- Vulkan validation layer 1.4.350;
- the repository-pinned FFmpeg 8.1.2 build and overlay patch set;
- two real 4096x3072 frames, the production Vulkan preset, and `--validation`.

The conversion returned success, published a valid MOV, and FFmpeg software
decoded the entire output without error. No core Vulkan VUID, application-owned
shader diagnostic, device loss, encoder failure, or mux validation failure was
observed.

## Decision

The project accepts this exact upstream GPU-AV diagnostic as a release waiver.
It is not described as validation-clean and is not suppressed in diagnostic
runs. The waiver avoids carrying an unproven local change to FFmpeg's DCT math
or synchronization when the current evidence does not demonstrate an output or
lifetime defect.

The waiver covers only the shared variable `blocks` diagnostic whose two source
locations are `dct.glsl:143` and `dct.glsl:167` in the pinned FFmpeg source.
Every other validation diagnostic remains release-blocking.

## Revocation and retest

Reopen this gate if any of the following occurs:

- FFmpeg, glslang, the Vulkan validation layer, or the GPU driver is upgraded;
- the DCT shader or its workgroup layout changes;
- the output becomes nondeterministic, undecodable, corrupt, or fails a reader;
- the diagnostic begins naming another invocation pair or source location;
- any core VUID or application-owned shader diagnostic appears.

The minimal retest is a two-frame real 4096x3072 forced-Vulkan conversion with
GPU-assisted validation followed by a complete software decode of the MOV.

