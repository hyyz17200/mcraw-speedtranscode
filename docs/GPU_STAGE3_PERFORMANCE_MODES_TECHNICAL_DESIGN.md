# GPU Stage 3 Performance Modes Technical Design

Date: 2026-07-15

Status: frozen Batch D implementation contract. Stage 2 FP32 precise remains the
reference and rollback point until the acceptance report explicitly promotes an
experiment.

## 1. Boundary and frozen reference

Batch D changes one performance variable at a time after the accepted Stage 2
U16 RAW resident entry. It does not change RAW decoding, calibration semantics,
the approved precise RCD implementation, color-solution setup, chroma siting,
ProRes settings, audio, timestamps, fallback, or publication rules.

The reference is the Stage 2 pipeline accepted by
`GPU_STAGE2E_E2E_BENCHMARK.md`: U16 upload, FP32 calibration/RCD/color/sharpen/
DI/YUV, quality chroma, deterministic dither, and `gpu_rcd_precise`. Its final
Y, Cb, and Cr budget remains at most 1 LSB.

## 2. Public identity

The public configuration gains a `gpu_performance_mode` with these stable
identities:

| Mode | Meaning | Initial error budget |
|---|---|---:|
| `precise` | Accepted Stage 2 semantics | max/P99 <= 1 LSB |
| `fast` | Only individually accepted approximations | max <= 8, P99 <= 2, RMSE <= 1.0 LSB |

`precision=fp32|fp16` is replaced by this semantic mode before any approximate
path is enabled. The sidecar and conversion telemetry must record the requested
mode plus the actual intermediate storage, DI implementation, dither mode, and
demosaic implementation. CPU behavior remains unchanged; unsupported GPU modes
must fail preflight or forced Vulkan rather than silently claiming a different
identity.

## 3. Ordered experiments

Each experiment starts from a clean accepted commit and changes exactly one
variable:

1. **D1 identity foundation.** Add the modes and actual sidecar/telemetry fields
   while all modes resolve to the precise implementation. Tests must prove that
   identities round-trip and that no mode can misreport an approximation.
2. **D2 FP16 intermediate storage.** Evaluate 16-bit storage for post-RCD
   color/sharpen/DI intermediates while retaining FP32 arithmetic, matrix dot
   products, sharpening accumulation, DI evaluation, and YUV quantization.
   Calibration and precise RCD stay FP32. This is the mixed-precision basis of
   the `fast` candidate.
3. **D3 DI evaluation.** Compare the accepted FP32 LUT with analytic FP32 DI
   evaluation, without changing intermediate precision. This is accepted only
   if quality passes and end-to-end performance improves outside run spread.
4. **D4 dither.** Compare deterministic GPU dither with disabled/non-bit-exact
   alternatives. A change is accepted only with measurable end-to-end benefit;
   removing dither solely because output still decodes is not sufficient.
5. **D5 demosaic.** Profile an explicitly named fast demosaic candidate only if
   the preceding results still show demosaic as a material bottleneck. It must
   never be named RCD and requires the separate demosaic corpus/artifact review
   from the formal guide in addition to final-YUV statistics.
6. **D6 acceptance.** Run matched Stage 2/precise/fast benchmarks and
   publish accepted variables, rejected experiments, quality statistics, and
   the decoder-ROI input for Batch E.

If an experiment cannot be represented independently by the identity fields,
the identity foundation is fixed first; the experiment does not proceed.

## 4. Golden and quality gates

The fixed quality set is:

- synthetic dark, negative-toe, super-white, saturated-color, fine-line,
  diagonal-edge, and high-frequency chroma cases;
- all four Bayer patterns, borders, black/white fields, and bad-pixel
  neighborhoods for any demosaic experiment;
- real sample frames 0, 120, and 239 from the Stage 0 corpus;
- final Y, Cb, and Cr plane max, RMSE, P50, P95, P99, and coordinates for max
  outliers;
- decoded ProRes comparison and explicit visual-review status for any accepted
  approximation.

Fast budgets are ceilings, not automatic acceptance. A candidate
that passes them but introduces structured clipping, zippering, moire, or edge
artifacts is rejected.

## 5. Ownership, synchronization, and telemetry

Slot ownership and the existing timeline-semaphore handoff remain unchanged.
Production performs one U16 upload and no calibrated RAW, Camera RGB,
intermediate RGB, or YUV readback. Test-only readback remains allowed for golden
comparisons.

Every accepted mode reports:

- `performance_mode`;
- actual `intermediate_storage`, `di_implementation`, `dither_mode`, and
  `demosaic_implementation`;
- per-stage GPU timestamps and timestamp sample counts;
- U16/FP16/FP32 transfer counters without counting device-local traffic as
  PCIe transfer;
- VRAM peak, queue depths/waits, encoder time, wall time, and output FPS.

## 6. Failure semantics and non-goals

Device loss, cancellation, partial-file cleanup, automatic fallback, and forced
Vulkan failure retain the Stage 2 contract. A failed fast-mode capability check
may fall back only under the existing whole-file fallback policy; it may not
switch precision or demosaic implementation inside one MOV.

Batch D does not make Vulkan the default, change CPU output, add a GPU MCRAW
decoder, fuse calibration with RCD, change metadata/chroma decisions, or weaken
the precise name.

## 7. Benchmark and completion gate

Each candidate uses the frozen 4096x3072 240-frame input and config, one warm-up,
and at least three official runs. A candidate is promoted only when:

1. all applicable quality and production invariants pass;
2. median end-to-end FPS improves beyond the matched run spread;
3. the relevant GPU timestamp or memory evidence explains the gain;
4. complexity and VRAM behavior remain acceptable;
5. sidecar identity is complete and truthful.

Rejected code is removed before the decision commit; its measured NO-GO result
is retained in the validation report. Batch D is complete only after D1-D6 have
test evidence, the accepted presets are unambiguous, the full Release test suite
passes, and the formal guide plus implementation status record the decision.
