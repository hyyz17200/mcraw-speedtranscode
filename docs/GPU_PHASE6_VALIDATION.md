# GPU Phase 6 Validation

Date: 2026-07-14

## Scope

Task 8 turns the Phase 5 direct handoff into the bounded asynchronous Vulkan
backend used by `FfmpegWriter` and the `convert` command. The CPU backend keeps
its existing fused camera-to-YUV path and multi-context ProRes workers.

When Vulkan is actually selected, `CpuPipeline` stops at planar FP32 TargetLog
RGB. `FfmpegWriter` then runs:

```text
bounded CPU decode futures
  -> bounded TargetLog RGB job queue
  -> GPU worker / bounded Vulkan slots
  -> prores_ks_vulkan send + drain
  -> bounded encoded-packet queue
  -> dedicated MOV mux worker
```

The split TargetLog path includes the same exposure, camera-to-DWG matrix,
capture sharpening, negative policy, and DaVinci Intermediate curve as the CPU
fused reference. A code-value regression test covers the split/fused boundary
and permits at most 1 final 10-bit LSB difference.

## Bounded slots and synchronization

`async_depth` selects 1..32 Vulkan processing slots. Each slot owns three RGB
staging buffers, one persistent descriptor set, one command buffer, one fence,
and transient plane views. Submission returns as soon as the compute command is
queued. The encoder waits the `AVVkFrame` timeline semaphore on the GPU.

A slot fence is checked only when that ring slot is reused or during final
drain. If the slot is still busy, the producer blocks and records a
backpressure event and duration. There is no normal-path `vkQueueWaitIdle()`,
`vkDeviceWaitIdle()`, or `avcodec_flush_buffers()`.

The frame pool is sized from processing slots plus encoder overlap and a safety
margin. The TargetLog job queue is `async_depth + 2` (bounded to 4..64), and the
packet queue is `2 * async_depth` (bounded to 8..128). No queue drops a frame.

## Shutdown, cancellation, and mux backpressure

The GPU worker is the only caller of the Vulkan frame writer and encoder. It
drains available packets after every send and sends the null frame only after
the job queue is empty. The mux worker is the only Vulkan video packet writer;
audio and video writes remain serialized by the mux mutex.

Both queues wait on capacity condition variables. A GPU, encoder, packet, or
mux exception atomically cancels both queues, wakes every waiter, joins both
workers, and is rethrown to the caller. Cleanup stops Vulkan workers before
attempting a partial MOV trailer, so a mux worker cannot race container
teardown. A duplicate-PTS fault-injection test verifies this path reaches the
caller without deadlock.

Telemetry records queue capacities and maximum depths, combined slot/job/packet
backpressure waits and milliseconds, RGB staging bytes, direct/upload/readback
frame counts, compressed mux bytes, and measured packet-write MB/s.

## Automated validation

- Release suite: 42 tests passed; the environment-gated 4096x3072 raw shader
  allocation test was skipped in the ordinary run and had passed in Phase 4.
- A writer-level 300-frame test produced a ProRes HQ MOV through eight slots,
  both bounded queues, and the dedicated mux worker. The MOV reopened, passed
  packet/metadata validation, and decoded 300/300 in software.
- The same test asserts `gpu_resident=true`, `direct_frames=300`,
  `upload_frames=0`, `readback_frames=0`, exact RGB staging-byte accounting,
  bounded maximum queue depths, nonzero mux bytes, and nonzero measured write
  throughput.
- The lower-level 30,000-frame Phase 5 stress remains the long-duration
  timeline/pool reuse test.

## Real 4096x3072 smoke run

Eight frames from `260710_142121_VIDEO_49mm.mcraw` were converted with
`async_depth=8`, audio enabled, and forced Vulkan (no fallback). The application
reopened the partial MOV before rename, and a separate FFmpeg software decode
completed without error.

Observed telemetry:

```text
backend=prores_ks_vulkan
gpu_resident=true
direct_frames=8
upload_frames=0
readback_frames=0
gpu_queue_max_depth=6 / capacity=10
packet_queue_max_depth=1 / capacity=16
backpressure_waits=0
rgb_upload_bytes=1,207,959,552
video_packets=8
throughput=2.306 fps
```

The RGB byte count is intentional and honest: eight 4096x3072 frames, three
FP32 planes each. Phase 6 removes the CPU YUV pack and uncompressed YUV
upload/readback, but RAW decode, demosaic, camera color, sharpening, and DI
encoding are still CPU work. The measured split color stage is now the largest
remaining CPU cost and is the primary profiling input for Task 9.
