# Batch E status

Date: 2026-07-16

## Completed and committed

| Scope | Commit | Result |
|---|---|---|
| E-A official truth and allocation fix | `aebccfb` | Pinned upstream compression 6/7 decoder; six existing compression 7 U16 goldens unchanged |
| E-B persistent bounded scheduling | `5c734bb` | Replaced per-frame `std::async`; ordered bounded workers, cancellation, failure propagation, sidecar telemetry |
| E-C updated capacity baseline | `752172c` | Added formal decoder benchmark; completed compression 7 decoder-only, `loadFrame`, and full Vulkan matrices |
| E-F measured automatic defaults | `846496d` | Precise/CPU limit 6 workers; Vulkan/auto fast limit 2 workers; explicit override retained |

The compression 7 `loadFrame` medians are 88.39/168.86/290.86/355.54/367.04
fps for 1/2/4/6/8 workers. Full-pipeline medians select six precise workers at
37.77 fps and two fast workers at 39.43 fps. The updated official decoder
satisfies the 90/120/130 fps capacity tiers and is not the full-pipeline
bottleneck.

E-D's repository-owned compression 7 fast decoder is therefore a formal no-go;
the conditional performance gate is not met. E-E's compression 6 fast decoder
is also a no-go without a real legacy corpus. No M1-M5 or ISA implementation was
started.

## Compression 6 decision and waiver

The workspace contains two real MCRAW files and both report compression 7. On
2026-07-16 the project explicitly waived the missing compression 6 corpus rather
than waiting for external footage. This is a test-coverage waiver only:

- runtime must continue trying the pinned official legacy decoder when metadata
  reports compression 6;
- the console must emit a clear warning that compression 6 has not been tested
  with a real project corpus and therefore does not carry compression 7's
  validation guarantee;
- the software and documentation must not claim compression 6 is bit-exact,
  safe-input, capacity, or production-validated by this project;
- this decision does not modify the upstream decoder or the project decoder
  implementation. This documentation change intentionally contains no code
  change.

The waived validation would otherwise have required:

- first/middle/last compressed payload hash and metadata;
- official U16 SHA-256 goldens;
- bit-exact output across the supported worker matrix;
- truncated/corrupt legacy payload and invalid header/offset/width tests;
- decoder-only, `loadFrame`, and full-pipeline capacity measurements.

The upstream legacy decoder is pinned and compiled, but runtime behavior for
compression 6 remains unverified by this project. Batch E is therefore complete
with an explicit compression 6 validation waiver; it must not be reported as
full compression 6 production validation.

The GPU pipeline boundary is frozen at U16 RAW upload. The open corpus gate does
not reopen GPU MCRAW decoder work.
