# Batch F Stability and Long-Run Gate

Date: 2026-07-16

## One-logical-hour resource test

The Vulkan upload-bridge E2E test was extended to permit 360 ten-second
iterations in one process. The release run completed:

- 360 create/encode/flush/reopen/decode/destroy cycles;
- 108,000 synthetic frames;
- 3,600 seconds of logical video;
- 123.850 seconds test wall time;
- post-warm-up private-memory growth guard: pass (maximum allowed growth 128 MiB);
- packet count, PTS, drain, trailer, full software decode, and zero-readback
  assertions: pass.

This closes the one-process logical-hour resource-growth test. It does not
replace a one-hour real MCRAW input.

## Real multi-file and repeated-start test

`scripts/run-batch-f-stability.ps1` ran two complete iterations over both real
samples. Every conversion used forced production-shaped Vulkan processing,
then passed full FFmpeg video/audio decode, random-access decode, VLC playback,
packet-count checks, and partial-file absence checks.

| Iteration | Frames | Throughput | Output bytes |
|---:|---:|---:|---:|
| 1 | 826 | 35.98 fps | 4,842,563,439 |
| 1 | 240 | 38.03 fps | 1,402,782,733 |
| 2 | 826 | 35.78 fps | 4,842,563,439 |
| 2 | 240 | 38.34 fps | 1,402,782,733 |

The repeated output sizes were identical. No `.partial.mov` remained.

## Failure semantics

The Release suite passed the bounded-worker cancellation/error-propagation
tests, forced-Vulkan no-fallback test, invalid explicit-device rejection,
partial-output cleanup path, and stable `device_lost` error taxonomy.

An actual `VK_ERROR_DEVICE_LOST` was not injected on this host. The taxonomy
and cleanup implementation are covered, but a real or deterministic fault-
injection run remains release-blocking.

## Remaining real-duration gate

The longest available real MCRAW contains 826 frames, approximately 27.5
seconds at 30 fps. It completed in both batch iterations. Because no one-hour
real source is present, the one-hour real-material conversion gate remains
**not run**. Synthetic logical duration is not used to waive it.

