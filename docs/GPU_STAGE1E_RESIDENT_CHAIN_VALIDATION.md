# GPU Stage 1E Resident Chain Validation

Date: 2026-07-15

Status: Stage 1E production Camera RGB resident chain, direct encoder-frame
handoff, telemetry, failure propagation and E2E validation complete. Formal
matched Stage 0/Stage 1 performance acceptance remains Stage 1F.

## Implemented boundary

```text
CPU Camera RGB FP32 upload
  -> Camera-to-DWG/exposure (ping A)
  -> capture sharpening (ping B)
  -> DaVinci Intermediate (ping A)
  -> RGB-to-YUV 4:2:2
  -> encoder-owned AVVkFrame
  -> Vulkan ProRes
```

The production Vulkan route now enters at `camera_rgb_f32`. It no longer
produces or uploads CPU TargetLog RGB. The Stage 0 TargetLog writer overload is
retained as a rollback boundary, but is not selected by the Stage 1 CLI route.
There is no production TargetLinear, TargetLog or YUV pixel readback.

## Ownership and synchronization

Each bounded slot owns the Camera RGB upload planes, two device-local planar
FP32 ping buffers, one control-status word, descriptor sets, command buffer,
fence and eight timestamp queries. The immutable DI LUT remains shared.

One command buffer records status reset, all four compute passes and the
AVVkFrame image transition. Compute write/read barriers protect every producer
and consumer, including ping A reuse for DI output. Submission uses the
FFmpeg-owned device, compute queue, queue lock and AVVkFrame timeline
semaphores. Slot recycling and final drain wait on the slot fence; normal
operation adds no queue- or device-idle wait.

The AVVkFrame source stage was narrowed to the compute shader stage after the
validation layer identified an invalid broad-stage/access pairing. The
corrected resident-chain validation run reported zero application validation
errors. Test-only FFmpeg frame download is excluded from that run because its
transfer-only queue cannot represent the preceding compute access; the same
test performs the numeric download separately without validation.

## Numeric and failure results

Hardware: RTX 3060, NVIDIA 576.02. Build type: MSVC Release.

| Gate | Result |
|---|---|
| Synthetic final Y maximum code difference | 0 LSB, pass |
| Synthetic final Cb maximum code difference | 0 LSB, pass |
| Synthetic final Cr maximum code difference | 0 LSB, pass |
| Camera RGB upload | exact `width * height * 3 * sizeof(float)` |
| TargetLog upload | 0 bytes |
| Production pixel/image readback | 0 frames |
| Negative-policy status fault | reaches caller, pass |
| Control-status success path | 4 bytes/frame, zero failures |

The full encoder test writes and decodes a ProRes 422 HQ MOV from the resident
Camera RGB route. A separate injected negative-policy failure aborts during
drain and does not publish a valid final output.

## Thirty-frame real-sample E2E

`scripts/validate-samples.ps1` converted the fixed 4096x3072 sample through the
CPU reference and Stage 1 Vulkan routes, decoded both complete video streams,
checked ProRes HQ/yuv422p10 packet counts, exercised auto fallback, and verified
forced invalid-device cleanup.

| Item | CPU | Stage 1 Vulkan |
|---|---:|---:|
| Frames | 30 | 30 |
| End-to-end wall time | 8807.036 ms | 7160.097 ms |
| Throughput | 3.406 fps | 4.190 fps |
| Video packets | 30 | 30 |

The Stage 1 run uploaded exactly 4,529,848,320 Camera RGB bytes, uploaded zero
TargetLog bytes, read 120 control-status bytes, and reported 30 timestamp
samples for each of Camera-to-DWG, sharpening, DI and RGB-to-YUV. Audio was
present; start/end A/V offsets were +0.018743/-0.012522 ms. These are E2E
validation numbers, not the repeated Stage 1F benchmark.

## GPU timestamps from the E2E run

| Pass | Mean | P50 | P95 | Samples |
|---|---:|---:|---:|---:|
| Camera-to-DWG | 13.817 ms | 12.173 ms | 24.556 ms | 30 |
| Capture sharpening | 3.277 ms | 1.362 ms | 13.126 ms | 30 |
| DaVinci Intermediate | 3.965 ms | 2.356 ms | 13.011 ms | 30 |
| RGB-to-YUV 4:2:2 | 2.254 ms | 1.085 ms | 8.490 ms | 30 |

## Regression

The complete MSVC Release build passed. CTest reported 61 tests with no
failure: 57 ran and passed and four opt-in 4K/real-frame tests skipped. The
resident golden then passed separately in normal mode with 0 LSB on all three
planes and in validation mode with zero application validation errors.

The pinned FFmpeg 8.1.2 ProRes DCT shader still triggers the previously
documented GPU-assisted shared-memory race report. This is the existing release
waiver in the formal action guide and is not introduced by Stage 1E.

## Artifact identity

These hashes describe the validated candidate based on Stage 1D commit
`5154668`:

| Artifact | SHA-256 |
|---|---|
| `src/vulkan/vulkan_rgb_to_yuv_frame.cpp` | `E97A9B4089EB55FFBBE0EF0AA7293FC4DFF35D101BF371F4D65425B135D23CF2` |
| Release `mcraw-transcoder.exe` | `C27EB9ECC411FFBD4B4EE79FAB39BDAB457DB6D5B75C49EE318E552BCE021559` |
| Release `mcraw_tests.exe` | `E1A7A64CD87A2B1FB8E76008817D19DDBA2DA032D6C9F5BA9B7476B6903588E7` |

## Next boundary

Stage 1F runs the frozen Stage 0 executable and this Stage 1 executable under
matched full-sample conditions: one warm-up and at least three official runs
per candidate. It records median/spread, output and executable hashes, complete
telemetry and the >=20% end-to-end go/no-go decision.
