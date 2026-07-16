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

## Remaining blocker

The workspace contains two real MCRAW files and both report compression 7. The
guide requires a real compression 6 corpus before E-F can pass:

- first/middle/last compressed payload hash and metadata;
- official U16 SHA-256 goldens;
- bit-exact output across the supported worker matrix;
- truncated/corrupt legacy payload and invalid header/offset/width tests;
- decoder-only, `loadFrame`, and full-pipeline capacity measurements.

The upstream legacy decoder is pinned and compiled, but those runtime claims
cannot be inferred from compression 7 or from a synthetic format substitute.
Batch E is complete for compression 7 and structurally ready for compression 6;
the all-format completion checkbox remains open until a genuine compression 6
sample is supplied.

The GPU pipeline boundary is frozen at U16 RAW upload. The open corpus gate does
not reopen GPU MCRAW decoder work.
