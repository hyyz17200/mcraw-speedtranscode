# MCRAW Audio Timestamp Analysis

## Scope

This report covers valid indexed PCM audio with irregular MCRAW capture timestamps.

## Conclusion

MCRAW audio chunk timestamps are capture/callback timestamps, not a reliable PCM
packet clock. They can jitter by approximately one 20 ms scheduling interval and
can briefly repeat or move backward even when PCM data is complete and ordered.

PCM order and per-chunk sample counts are the reliable timing source. The first
audio timestamp is a valid A/V anchor; subsequent audio PTS must be rebuilt from
cumulative samples at the declared sample rate.

## Evidence

### Affected sample

`build/msvc-release/Release/260710_142201_VIDEO_49mm.mcraw`

| Field | Value |
|---|---:|
| Format | PCM S16LE, 48 kHz, stereo |
| Audio chunks | 323 |
| Samples per channel | 330,602 |
| PCM duration | 6,887.542 ms |
| First audio minus first video timestamp | +0.009666 ms |
| Maximum absolute raw-vs-sample-clock residual | 20.120 ms |
| Raw audio end minus sample-clock audio end | -0.150 ms |

The first two chunk timestamps are non-monotonic:

```text
chunk 0: 964339428358447 ns, 917 samples
chunk 1: 964339428358113 ns, delta = -334 ns
chunk 2: 964339468017624 ns, delta = +39.659511 ms
```

Expected durations are 19.104167 ms for chunk 0 and 20.437500 ms for chunk 1.
The backward timestamp is followed by a long interval that compensates for it.

Three additional short/long interval pairs occur later:

| Boundary | Short interval | Following interval | Expected regular interval |
|---:|---:|---:|---:|
| 8/9/10 | 1.334 ms | 41.426 ms | 21.333 ms |
| 108/109/110 | 1.333 ms | 41.352 ms | 21.333 ms |
| 260/261/262 | 1.302 ms | 41.274 ms | 21.333 ms |

These paired errors nearly cancel. They indicate timestamp sampling jitter, not
missing PCM.

### Control sample

`mcraw_sample/260710_142121_VIDEO_49mm.mcraw` contains 377 chunks and 385,489
samples per channel. Its timestamps are strictly increasing, but its maximum
raw-vs-sample-clock residual is larger: 20.852 ms. Its raw audio end differs from
the sample-clock end by only -0.709 ms.

Therefore, strict monotonicity does not identify good timing. A monotonic stream can
still contain the same approximately 20 ms callback jitter.

## Current implementation

The code already contains a sample-count normalization pass in
`src/cli/main.cpp::normalize_audio_timestamps()`. It anchors the first timestamp and
advances a continuous cursor by chunk duration.

The affected sample cannot reach that pass:

1. `src/io/mcraw_reader.cpp::load_audio()` rejects any timestamp less than or equal
   to the previous timestamp.
2. `normalize_audio_timestamps()` repeats the same strict monotonicity check.
3. `inspect` and `convert` therefore fail on the -334 ns step before normalization.

Nanosecond normalization alone does not guarantee gapless writer PTS. The writer
currently rescales every chunk timestamp back to sample units. Independent rounding
can make an adjacent chunk one sample earlier than `next_audio_pts`, depending on
the fractional-sample position of the audio anchor relative to the video origin.
The robust fix is to rescale the first audio anchor once, then derive every writer
PTS from cumulative integer samples.

## Root cause

The container records one timestamp per audio callback/buffer. Callback delivery is
subject to Android scheduling and buffer handoff jitter. The PCM payload remains in
capture order, while timestamp observations can cluster, repeat, or move slightly
backward.

The evidence does not support sample loss:

- all chunks have valid aligned PCM payloads;
- short intervals are followed by compensating long intervals;
- cumulative timestamp error remains bounded to about one callback interval; and
- raw and sample-clock audio end times differ by less than 1 ms in both samples.

## Timing authority

Use the following precedence:

1. audio index order defines PCM order;
2. sample count and sample rate define duration;
3. the first source audio timestamp defines the A/V start anchor;
4. later source timestamps are diagnostic observations only; and
5. video source timestamps remain the video clock.

Do not sort audio chunks by timestamp. Sorting would reorder valid PCM.

## A/V duration note

After normalization, audio ends 28.823 ms after estimated video end in the affected
sample and 40.288 ms after video end in the control. This is expected capture-tail
coverage, not timestamp drift. Full-file conversion should preserve it. A
frame-limited conversion may crop audio at the selected video boundary.

## Classification

```text
pcm_payload: valid
pcm_order: audio_index_order
timestamp_anchor: first_audio_timestamp
timestamp_sequence: jittered_and_non_monotonic
repair: rebuild_pts_from_cumulative_samples
sample_mutation_required: false
resampling_required: false
```

## Related document

See
[MCRAW_AUDIO_TIMESTAMP_NORMALIZATION_TECHNICAL_DESIGN.md](MCRAW_AUDIO_TIMESTAMP_NORMALIZATION_TECHNICAL_DESIGN.md)
for the implementation contract and tests.
