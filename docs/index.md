# Documentation Index

Single entry point for all project documentation. Files keep their original names for
compatibility with existing cross-references and commit history — do not rename or move them.

**When adding a new doc:** follow the existing naming pattern for its category (see
[Conventions](#conventions) at the bottom) and add a one-line entry to the matching section here.

---

## 1. Overview and Architecture

Start here to understand the project.

| Doc | Date | Summary |
|---|---|---|
| [MCRAW_ProRes_RouteB_Architecture_Spec_CN.md](MCRAW_ProRes_RouteB_Architecture_Spec_CN.md) | 2026-07-15 | MCRAW → high-quality ProRes transcoder: Route B architecture and implementation specification |
| [VULKAN_PRORES_GPU_PIPELINE_GUIDE.md](VULKAN_PRORES_GPU_PIPELINE_GUIDE.md) | 2026-07-14 | Windows Vulkan ProRes GPU pipeline implementation guide |
| [implementation-status.md](implementation-status.md) | 2026-07-15 | v0.1 implementation status |
| [GPU_PIPELINE_SUMMARY_AND_NEXT_STEPS.md](GPU_PIPELINE_SUMMARY_AND_NEXT_STEPS.md) | 2026-07-14 | GPU pipeline build summary and bottleneck analysis |
| [GPU_PIPELINE_FORMAL_ACTION_GUIDE.md](GPU_PIPELINE_FORMAL_ACTION_GUIDE.md) | 2026-07-16 | Formal action guide for the next GPU pipeline stage |
| [adr/README.md](adr/README.md) | 2026-07-16 | v0.1 Architecture Decision Records |

## 2. Build and Environment

| Doc | Date | Summary |
|---|---|---|
| [FFMPEG_VULKAN_BUILD.md](FFMPEG_VULKAN_BUILD.md) | 2026-07-14 | Reproducible FFmpeg Vulkan build instructions |

## 3. Technical Designs and Analyses

| Doc | Date | Summary |
|---|---|---|
| [GPU_PIPELINE_AUDIT.md](GPU_PIPELINE_AUDIT.md) | 2026-07-14 | Vulkan ProRes GPU pipeline audit |
| [GPU_STAGE1_CAMERA_RGB_TECHNICAL_DESIGN.md](GPU_STAGE1_CAMERA_RGB_TECHNICAL_DESIGN.md) | 2026-07-15 | Stage 1: Camera RGB technical design |
| [GPU_STAGE2_U16_RAW_TECHNICAL_DESIGN.md](GPU_STAGE2_U16_RAW_TECHNICAL_DESIGN.md) | 2026-07-15 | Stage 2: U16 RAW technical design |
| [GPU_STAGE3_PERFORMANCE_MODES_TECHNICAL_DESIGN.md](GPU_STAGE3_PERFORMANCE_MODES_TECHNICAL_DESIGN.md) | 2026-07-15 | Stage 3: performance modes technical design |
| [GPU_PIPELINE_SERIALIZATION_ANALYSIS_2026-07-15.md](GPU_PIPELINE_SERIALIZATION_ANALYSIS_2026-07-15.md) | 2026-07-15 | Pipeline serialization analysis and fix plan |
| [MCRAW_AUDIO_TIMESTAMP_ANALYSIS.md](MCRAW_AUDIO_TIMESTAMP_ANALYSIS.md) | 2026-07-17 | Evidence and root-cause analysis for jittered and non-monotonic MCRAW audio timestamps |
| [MCRAW_AUDIO_TIMESTAMP_NORMALIZATION_TECHNICAL_DESIGN.md](MCRAW_AUDIO_TIMESTAMP_NORMALIZATION_TECHNICAL_DESIGN.md) | 2026-07-17 | Agent-ready PCM sample-clock normalization contract, diagnostics, and tests |

## 4. Phase Validation Reports (Phases 1–8)

Chronological validation of the initial GPU pipeline bring-up.

| Doc | Date | Summary |
|---|---|---|
| [GPU_PHASE1_VALIDATION.md](GPU_PHASE1_VALIDATION.md) | 2026-07-14 | Phase 1 validation |
| [GPU_PHASE2_VALIDATION.md](GPU_PHASE2_VALIDATION.md) | 2026-07-14 | Phase 2 validation |
| [GPU_PHASE3_VALIDATION.md](GPU_PHASE3_VALIDATION.md) | 2026-07-16 | Phase 3 validation |
| [GPU_PHASE4_VALIDATION.md](GPU_PHASE4_VALIDATION.md) | 2026-07-14 | Phase 4 validation |
| [GPU_PHASE5_VALIDATION.md](GPU_PHASE5_VALIDATION.md) | 2026-07-14 | Phase 5 validation |
| [GPU_PHASE6_VALIDATION.md](GPU_PHASE6_VALIDATION.md) | 2026-07-14 | Phase 6 validation |
| [GPU_PHASE7_OPTIMIZATION.md](GPU_PHASE7_OPTIMIZATION.md) | 2026-07-14 | Phase 7 optimization report |
| [GPU_PHASE8_PRODUCTION.md](GPU_PHASE8_PRODUCTION.md) | 2026-07-16 | Phase 8 productionization report |

## 5. Stage Validation and Experiment Reports (Stages 0–3)

Per-stage validation of the resident GPU processing chain.

### Stage 0

| Doc | Date | Summary |
|---|---|---|
| [GPU_STAGE0_BASELINE.md](GPU_STAGE0_BASELINE.md) | 2026-07-15 | Stage 0 baseline and profiler contract |

### Stage 1 (Camera RGB)

| Doc | Date | Summary |
|---|---|---|
| [GPU_STAGE1B_COLOR_VALIDATION.md](GPU_STAGE1B_COLOR_VALIDATION.md) | 2026-07-15 | 1B: Camera RGB color/exposure validation |
| [GPU_STAGE1C_SHARPENING_VALIDATION.md](GPU_STAGE1C_SHARPENING_VALIDATION.md) | 2026-07-15 | 1C: TargetLinear sharpening validation |
| [GPU_STAGE1D_DAVINCI_INTERMEDIATE_VALIDATION.md](GPU_STAGE1D_DAVINCI_INTERMEDIATE_VALIDATION.md) | 2026-07-15 | 1D: DaVinci Intermediate validation |
| [GPU_STAGE1E_RESIDENT_CHAIN_VALIDATION.md](GPU_STAGE1E_RESIDENT_CHAIN_VALIDATION.md) | 2026-07-15 | 1E: resident chain validation |
| [GPU_STAGE1F_E2E_BENCHMARK.md](GPU_STAGE1F_E2E_BENCHMARK.md) | 2026-07-15 | 1F: E2E benchmark decision |
| [GPU_STAGE1G_PERFORMANCE_RECOVERY.md](GPU_STAGE1G_PERFORMANCE_RECOVERY.md) | 2026-07-15 | 1G: performance recovery and acceptance |

### Stage 2 (U16 RAW)

| Doc | Date | Summary |
|---|---|---|
| [GPU_STAGE2B_CALIBRATION_VALIDATION.md](GPU_STAGE2B_CALIBRATION_VALIDATION.md) | 2026-07-15 | 2B: calibration validation |
| [GPU_STAGE2C_RCD_VALIDATION.md](GPU_STAGE2C_RCD_VALIDATION.md) | 2026-07-15 | 2C: precise RCD demosaic validation |
| [GPU_STAGE2D_RESIDENT_CHAIN_VALIDATION.md](GPU_STAGE2D_RESIDENT_CHAIN_VALIDATION.md) | 2026-07-15 | 2D: resident RAW chain validation |
| [GPU_STAGE2E_E2E_BENCHMARK.md](GPU_STAGE2E_E2E_BENCHMARK.md) | 2026-07-15 | 2E: E2E benchmark and Batch C decision |

### Stage 3 (Performance Modes)

| Doc | Date | Summary |
|---|---|---|
| [GPU_STAGE3B_FP16_STORAGE_VALIDATION.md](GPU_STAGE3B_FP16_STORAGE_VALIDATION.md) | 2026-07-15 | 3B: FP16 intermediate storage validation |
| [GPU_STAGE3C_ANALYTIC_DI_VALIDATION.md](GPU_STAGE3C_ANALYTIC_DI_VALIDATION.md) | 2026-07-15 | 3C: analytic DaVinci Intermediate validation |
| [GPU_STAGE3D_DITHER_EXPERIMENT.md](GPU_STAGE3D_DITHER_EXPERIMENT.md) | 2026-07-15 | 3D: dither experiment |
| [GPU_STAGE3E_FAST_DEMOSAIC_EXPERIMENT.md](GPU_STAGE3E_FAST_DEMOSAIC_EXPERIMENT.md) | 2026-07-15 | 3E: fast demosaic experiment |
| [GPU_STAGE3F_E2E_BENCHMARK.md](GPU_STAGE3F_E2E_BENCHMARK.md) | 2026-07-15 | 3F: E2E benchmark and Batch D decision |

## 6. Batch Reports (D, E, F)

Post-stage work batches: throughput (D), decode capacity (E), release gating (F).

### Batch D (Async Throughput)

| Doc | Date | Summary |
|---|---|---|
| [GPU_BATCH_D1A_OUTPUT_VALIDATION.md](GPU_BATCH_D1A_OUTPUT_VALIDATION.md) | 2026-07-15 | D.1-A: output validation and preflight boundary |
| [GPU_BATCH_D1B_ASYNC_DEPTH.md](GPU_BATCH_D1B_ASYNC_DEPTH.md) | 2026-07-15 | D.1-B: effective Vulkan encoder async depth |
| [GPU_BATCH_D1CD_GO_NO_GO.md](GPU_BATCH_D1CD_GO_NO_GO.md) | 2026-07-15 | D.1-C/D: queue separation and device-local RAW staging decision (no-go) |

### Batch E (Decoder Capacity)

| Doc | Date | Summary |
|---|---|---|
| [GPU_BATCH_E_STATUS.md](GPU_BATCH_E_STATUS.md) | 2026-07-16 | Batch E status overview |
| [GPU_BATCH_EA_OFFICIAL_DECODER.md](GPU_BATCH_EA_OFFICIAL_DECODER.md) | 2026-07-16 | E-A: official compression 6/7 decoder ground truth |
| [GPU_BATCH_EB_PERSISTENT_WORKERS.md](GPU_BATCH_EB_PERSISTENT_WORKERS.md) | 2026-07-16 | E-B: persistent bounded frame workers |
| [GPU_BATCH_EC_DECODER_CAPACITY.md](GPU_BATCH_EC_DECODER_CAPACITY.md) | 2026-07-16 | E-C: updated official decoder capacity |

### Batch F (Release Gates)

| Doc | Date | Summary |
|---|---|---|
| [GPU_BATCH_F_RELEASE_GATE_STATUS.md](GPU_BATCH_F_RELEASE_GATE_STATUS.md) | 2026-07-16 | Release gate status overview |
| [GPU_BATCH_F_COLOR_METADATA.md](GPU_BATCH_F_COLOR_METADATA.md) | 2026-07-16 | Color metadata and chroma decision |
| [GPU_BATCH_F_COMPATIBILITY.md](GPU_BATCH_F_COMPATIBILITY.md) | 2026-07-16 | Compatibility gate |
| [GPU_BATCH_F_HARDWARE_MATRIX.md](GPU_BATCH_F_HARDWARE_MATRIX.md) | 2026-07-16 | Hardware matrix |
| [GPU_BATCH_F_STABILITY.md](GPU_BATCH_F_STABILITY.md) | 2026-07-16 | Stability and long-run gate |
| [GPU_BATCH_F_VALIDATION_RACE_WAIVER.md](GPU_BATCH_F_VALIDATION_RACE_WAIVER.md) | 2026-07-16 | FFmpeg ProRes GPU-AV race waiver |

## 7. Benchmarks

| Doc | Date | Summary |
|---|---|---|
| [quality-options-benchmark-2026-07-14.md](quality-options-benchmark-2026-07-14.md) | 2026-07-14 | Quality options benchmark |
| [PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md](PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md) | 2026-07-15 | `prores_ks_vulkan` encoder-only benchmark |
| [PRORES_KS_VULKAN_ENCODER_DEPTH_BENCHMARK_2026-07-15.md](PRORES_KS_VULKAN_ENCODER_DEPTH_BENCHMARK_2026-07-15.md) | 2026-07-15 | `prores_ks_vulkan` encoder-only async-depth benchmark |
| [HEVC_NVENC_SINGLE_STREAM_BENCHMARK_2026-07-15.md](HEVC_NVENC_SINGLE_STREAM_BENCHMARK_2026-07-15.md) | 2026-07-15 | H.265 NVENC single-stream benchmark |

---

## Conventions

- **Never rename or move existing docs** — filenames are referenced from commit messages and other docs.
- New docs go directly in `docs/` (ADRs in `docs/adr/`), following the established naming patterns:
  - Validation/experiment reports: `GPU_<PHASE|STAGE|BATCH>_<ID>_<TOPIC>.md`
  - Benchmarks: `<TOPIC>_BENCHMARK_<YYYY-MM-DD>.md`
  - Designs/guides: `<TOPIC>_TECHNICAL_DESIGN.md` / `<TOPIC>_GUIDE.md`
- Every new doc gets a one-line entry in the matching section of this index (add a new section if none fits).
- All documentation is written in English. Existing filenames are preserved for compatibility with historical references.
