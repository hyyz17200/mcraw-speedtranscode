# Batch D.1-C/D: Queue Separation and Device-Local RAW Staging Decision

Date: 2026-07-15
Status: No-go; no implementation justified by the current evidence.

## Queue reservation/separation

The depth matrix reduced `encoder_send` wall time from 22.73 ms at depth 1 to
4.98/1.57 ms at depth 4/8, but complete conversion medians remained tightly
clustered at 35.37, 34.97, 35.58, and 35.56 fps. Depth 8 added VRAM without
beating depth 4. The runs therefore show asynchronous encoder work was accepted
and overlapped internally, but do not show a recoverable queue-idle interval
that a second queue would turn into end-to-end throughput.

The current telemetry reports a single compute queue assignment
(family 2/index 0), GPU utilization samples, stage timestamps, and backpressure,
but not a matched per-queue idle timeline or cross-queue semaphore overlap
benefit. Device-level utilization is not sufficient evidence to infer queue
idle time. Queue separation is consequently a no-go for this batch.

## Device-local RAW staging

The depth-4 run reports approximately 1.98 ms raw calibration per frame and
6,039,797,760 bytes of U16 RAW upload for 240 frames, with zero readback. This
identifies a possible PCIe/device-memory experiment, but there is no transfer
queue timeline, memory-type comparison, or matched staging measurement in the
current telemetry. Implementing staging now would be speculative and could
change ownership/synchronization without a demonstrated gain. Device-local RAW
staging is also a no-go for this batch.

## Decision

Keep the validated shared queue and host-visible RAW upload path. Select
async_depth 4 as the practical candidate from the measured matrix; depth 8 is
not preferred because it costs more VRAM without an end-to-end gain. Reopen
either experiment only with new profiler evidence that directly measures queue
idle or transfer/memory overlap.
