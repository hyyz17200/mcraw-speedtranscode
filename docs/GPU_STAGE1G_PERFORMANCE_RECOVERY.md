# GPU Stage 1G Performance Recovery and Acceptance

Date: 2026-07-15

Status: Stage 1 performance recovery complete. The matched full-sample result
passes the frozen Stage 1 `+20%` acceptance gate. This supersedes the Stage 1F
NO-GO decision for the current candidate; the Stage 1F report remains the
historical regression record.

## Decision

The clean `042e179` candidate produced a 13.791 fps median on the same RTX
3060, 4096x3072 240-frame sample and frozen Stage 0 config used by Stage 1F.
The rebuilt Stage 0 median was 6.863 fps, so the accepted candidate is
100.943% faster than Stage 0 and 177.871% faster than the rejected 4.963 fps
Stage 1F candidate.

| Candidate | Median fps | Min-max fps | Median wall time |
|---|---:|---:|---:|
| Rebuilt Stage 0, `622070c` | 6.863 | 6.774-7.114 | 34,969.527 ms |
| Rejected Stage 1F, `7491bac` | 4.963 | 4.934-5.052 | 48,357.423 ms |
| Accepted Stage 1G, `042e179` | 13.791 | 13.429-13.873 | 17,402.977 ms |

The accepted-run spread is 3.221% of its median and cannot hide the result.
The 24 fps product target is still not met; Stage 1 acceptance is the frozen
relative gate, not the final product-performance gate.

## Root cause and corrections

The Stage 1F `prores_submit_wait` value combined producer queue backpressure
with downstream processing and did not mean that ProRes itself consumed 181.8
ms/frame. Split telemetry established that resident-slot fences, frame-pool
allocation, queue locking, packet delivery and mux were not blocking.

Three corrections resolved the regression:

1. `f6928be` split job, slot, packet, frame-pack, encoder-send, allocation and
   queue-submit telemetry.
2. `bac9640` separated Vulkan frame preparation from ProRes submission so CPU
   upload/command preparation and delayed encoder work can overlap. It also
   removed the redundant CPU scan of all 151 MB of Camera RGB per frame.
   Non-finite pixels continue to fail through the resident DI shader's
   control-status flag before an output MOV can be published; a production E2E
   fault test covers this contract.
3. `042e179` decoupled encoder async depth from resident preparation depth.
   ProRes retains the configured depth of eight, while two resident slots cover
   producer/consumer overlap without allocating eight copies of both full-frame
   FP32 intermediate ping sets.

The rejected implementation serialized approximately 140 ms of frame packing
and 48 ms of encoder submission. In the accepted official runs, frame packing
was 24.066-24.826 ms/frame, encoder submission was 26.144-29.285 ms/frame, and
the two stages overlapped on separate workers.

## Official runs

| Run | fps | wall ms | CPU mean % | GPU mean % | VRAM delta MiB |
|---|---:|---:|---:|---:|---:|
| 1 | 13.873 | 17,300.027 | 78.08 | 34.44 | 2,025 |
| 2 | 13.429 | 17,872.303 | 74.75 | 36.61 | 2,064 |
| 3 | 13.791 | 17,402.977 | 77.02 | 33.75 | 2,032 |

All three runs reported two resident slots, prepared-frame queue peak at most
two, packet queue peak two and zero job/slot/packet backpressure. Median VRAM
delta fell from the rejected Stage 1F value of 3,782 MiB to 2,032 MiB.

Median-of-run GPU pass means were Camera-to-DWG 12.056 ms, sharpening 0.969 ms,
DI 1.068 ms and RGB-to-YUV 0.635 ms. With the Stage 1 queue no longer blocked,
the next full-pipeline limit is again CPU RAW/calibration/RCD production rather
than the resident Stage 1 chain.

## Identity and correctness

| Item | Value |
|---|---|
| Candidate commit | `042e179a1f6d3c5ddcb389f4a6acf70f39b71ed4` |
| Dirty | `false` |
| Executable SHA-256 | `C704045F562C48E9EC7601CC992D9595F381912773F8A6845B991BB0C962E112` |
| Config SHA-256 | `C0EE7C9E58BDFF969D512F7FEE705BC1AFCE5B162899A735BAED3614D0BE82E9` |
| Input SHA-256 | `2B4066B21E63458B9BCFCB9B503B58D241FFE3AE99E021BF330E3F612F17F706` |
| Output bytes | `1,402,785,678` |
| Output SHA-256 | `AEB9E1C347D6F542BE38B3BDEDFBA58BC14996B0A38A523EC379E499AC8347EB` |

The output identity is unchanged from the rejected Stage 1F candidate. The
final MOV contains 240 ProRes HQ `yuv422p10le` packets and the preserved 48 kHz
stereo audio stream and passed the benchmark's MOV/FFprobe validation. The
real-frame Stage 1 golden test passed frames 0, 120 and 239 with final Y, Cb and
Cr maximum error exactly 1 LSB, plus all 23 assertions.

The final MSVC Release suite contains 63 tests: 58 passed and five opt-in
4K/real-frame tests skipped in the default run. The real Stage 1 test was run
separately and passed. Vulkan capability validation also passed with FFmpeg
8.1.2 and `prores_ks_vulkan`.

## Stage 1 closure

Stage 1 now satisfies its correctness, zero-readback, exact-transfer,
continuous-encoder-supply and relative-performance gates. It is accepted as
the current Vulkan Camera RGB rollback point. Stage 2 prioritization may now be
based on the restored CPU producer bottleneck; the independent production
release gates and 24 fps product target remain open.
