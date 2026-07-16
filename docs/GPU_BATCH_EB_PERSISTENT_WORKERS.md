# Batch E-B: persistent bounded frame workers

Date: 2026-07-16

## Implemented boundary

The CLI no longer launches one `std::async` thread per frame. Each benchmark or
conversion owns one `PersistentWorkerPool` for its complete lifetime. The pool:

- creates a fixed number of `std::jthread` workers once;
- bounds its pending queue to the resolved frame-worker count;
- returns futures that the caller consumes in source order;
- propagates the first observed task exception through that ordered future;
- cancels queued work and joins active workers during failure unwinding;
- exposes worker count, queue capacity/peak, submit wait count/time, and
  started/completed/cancelled task counts in benchmark JSON and MOV sidecars.

The existing `McrawReader` decoder lease pool remains the decoder-instance
ownership boundary. A persistent frame worker therefore reuses its thread while
leasing an official decoder instance for each frame; no shared decoder is used
concurrently.

## Unified CPU and memory budget

`cpu_threads` remains the total process-side producer budget. The execution plan
sets:

```text
frame_workers = max_parallel_frames (resolved)
threads_per_frame = max(1, cpu_threads / frame_workers)
```

so configured frame-level and intra-frame pipeline parallelism cannot multiply
past the budget through nested per-frame thread creation. Automatic mode starts
from at most six persistent frame workers, further limited by selected frames,
logical processors, and the existing quarter-of-available-memory working-set
guard. Explicit `max_parallel_frames` remains available for the required
1/2/4/6/8 capacity matrix.

The official upstream decoder owns and reuses its compressed scratch buffer per
decoder instance. Its updated output API allocates the exact U16 byte count; the
application then copies into the final typed `RawMosaicU16` owner. Removing that
last ownership copy is reserved for a separately measured decoder API change,
not mixed into this scheduling commit.

## Validation

- MSVC Release build passed.
- All 77 registered tests passed (seven optional extracted-corpus tests skipped
  because their artifact environment variables were not set).
- New unit coverage verifies bounded queue depth, persistent worker reuse,
  ordered result consumption, task exception propagation, and queued-task
  cancellation.
- An eight-frame real compression 7 compute smoke completed with all tasks
  started/completed and no cancellation.
- An eight-frame forced-Vulkan precise conversion completed with eight video
  packets, zero RAW readback, and the worker telemetry present in the sidecar.

No RAW decode algorithm, metadata semantics, frame order, PTS, audio, Vulkan
ownership, or fallback behavior changed in this batch.
