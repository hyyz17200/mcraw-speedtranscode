# MCRAW Audio Timestamp Normalization Technical Design

## Status

Proposed completion of the existing normalization path. The first source timestamp
remains the A/V anchor. No resampling or PCM modification is permitted.

## Clock model

Use two related representations:

1. **Diagnostic nanoseconds:** rebuild expected source timestamps from the first
   source timestamp and cumulative samples.
2. **Mux sample units:** rescale the first audio anchor to the writer time base once,
   then add cumulative samples exactly.

For chunk `i`:

```text
expected_ns(i) = first_source_timestamp_ns
               + samples_to_ns(cumulative_samples_before_i)

writer_pts(i)  = first_audio_pts + cumulative_samples_before_i
```

Audio index order defines PCM order. Never sort chunks by source timestamp.

## Shared timing operation

Implement one shared operation used by both `inspect` and `convert`:

```text
analyze_and_normalize_audio(AudioInfo) -> AudioTimingResult
```

The result must contain:

- normalized chunks;
- first source anchor;
- cumulative sample count;
- residual diagnostics; and
- the warning decision and reason codes.

`inspect` and `convert` must not compute timing statistics independently.

## Validation ownership

### Reader

`McrawReader::load_audio()` owns container structure. It must reject:

- invalid offsets or item types;
- empty or odd-sized PCM payloads;
- channel-misaligned samples; and
- missing timestamp metadata.

It must preserve the signed raw timestamp value and audio index order. It must not
reject duplicate, backward, or negative timestamp values.

### Shared timing operation

The shared operation owns timestamp semantics. It must:

1. require a positive sample rate and supported channel count;
2. reject any negative source timestamp;
3. preserve chunk and sample order;
4. rebuild expected nanosecond timestamps from the first anchor and cumulative
   samples;
5. preserve every PCM sample bit-for-bit; and
6. produce diagnostics before replacing timestamps.

Remove the current strict source-monotonicity rejection from both the reader and
`normalize_audio_timestamps()`.

## Integer rescale contract

Use shared overflow-safe integer helpers for every audio timeline conversion:

```text
samples_to_ns_nearest(samples, sample_rate)
ns_to_samples_nearest(ns, sample_rate)
ns_to_samples_floor(ns, sample_rate)
```

Apply them uniformly:

| Call site | Required conversion |
|---|---|
| Normalized diagnostic timestamp | samples to ns, nearest |
| Writer first audio PTS | anchor-minus-origin ns to samples, nearest |
| Frame-limit crop | available ns to samples, floor |
| A/V sync audio end | final chunk samples to ns, nearest |

Replace the `double` plus `llround`/`floor` calculations in
`normalize_audio_timestamps()`, `av_sync_report()`, and the frame-limit crop. No
audio timeline path may use floating-point arithmetic.

## Writer PTS contract

Do not rescale every normalized nanosecond timestamp independently.

On the first audio chunk:

```text
first_audio_pts = ns_to_samples_nearest(first_anchor_ns - origin_ns)
samples_written = 0
```

For every chunk:

```text
frame_pts = first_audio_pts + samples_written
samples_written += frame_samples
next_audio_pts = first_audio_pts + samples_written
```

Use checked integer addition. Keep the existing overlap/gap check as an internal
invariant assertion. Gaplessness is then exact by construction and independent of
nanosecond rounding or the fractional-sample position of `anchor - origin`.

## Diagnostics

Expose identical fields in `inspect` JSON and the conversion sidecar:

```json
{
  "audio_timing": {
    "mode": "sample_clock_from_first_source_anchor",
    "normalized": true,
    "source_non_monotonic_steps": 1,
    "source_max_backward_step_ns": 334,
    "source_max_abs_residual_ns": 20119935,
    "source_end_residual_ns": -150334,
    "nominal_chunk_interval_ns": 21333333,
    "warning_reasons": ["source_non_monotonic"]
  }
}
```

Definitions:

- `source_non_monotonic_steps`: count of raw deltas `<= 0`;
- `source_max_backward_step_ns`: largest magnitude raw backward delta;
- `source_max_abs_residual_ns`: maximum absolute raw-vs-expected difference;
- `source_end_residual_ns`: raw PCM end minus sample-clock PCM end; and
- `nominal_chunk_interval_ns`: duration of the median positive chunk sample count.

## Warning policy

Routine callback jitter is telemetry, not a warning. Emit one warning only when:

```text
source_non_monotonic_steps > 0
OR
source_max_abs_residual_ns > 2 * nominal_chunk_interval_ns
```

Use stable reason codes:

- `source_non_monotonic`;
- `residual_exceeds_two_chunk_intervals`.

The affected sample warns because it has one backward step. The control sample does
not warn: its 20.852 ms residual is below two nominal 21.333 ms intervals.

Suggested warning:

```text
noteworthy source audio timestamp irregularity; PCM PTS rebuilt from sample count
```

Agents must use structured fields, not parse warning text.

## Discontinuity policy

Timestamp residuals must not create, drop, duplicate, reorder, or resample PCM.
Report large residuals. Add a corruption fail gate only after a broader corpus
establishes a defensible threshold.

## Implementation steps

1. Relax only timestamp-value checks in `src/io/mcraw_reader.cpp`; keep structural
   validation.
2. Replace `normalize_audio_timestamps()` with the shared timing operation.
3. Add the three integer rescale helpers and migrate normalization, crop, sync
   reporting, and writer-anchor conversion.
4. Change `FfmpegWriter::write_audio()` to use one rescaled anchor plus cumulative
   samples.
5. Publish the same diagnostics from `inspect` and the conversion sidecar.
6. Apply the warning policy after diagnostics are complete.
7. Add focused unit and end-to-end tests.

## Test matrix

| Case | Expected result |
|---|---|
| Strictly increasing timestamps with routine jitter | normalize; no warning |
| One -334 ns step | normalize; `source_non_monotonic` warning |
| Duplicate timestamps | normalize; `source_non_monotonic` warning |
| Residual above two nominal intervals | normalize; residual warning |
| Negative timestamp in any chunk | shared timing operation fails |
| Missing timestamp metadata | reader fails |
| Invalid PCM alignment | reader fails |
| Valid all-zero PCM | normalize normally |

Required assertions:

- the affected 323-chunk sample passes `inspect` and `convert`;
- the control sample emits no routine-jitter warning;
- `inspect` and `convert` return identical timing diagnostics;
- first output audio PTS is the single rescale of `anchor - origin`;
- every later PTS equals first PTS plus exact cumulative samples;
- an anchor near a half-sample rounding boundary cannot create a one-sample overlap;
- crop sample count uses integer floor at boundaries immediately below, on, and
  above one sample;
- A/V end reporting uses the same integer sample clock;
- any negative chunk timestamp fails in the shared timing operation;
- PCM SHA-256 is unchanged before and after mux/decode;
- affected diagnostics report one non-monotonic step and approximately 20.120 ms
  maximum residual;
- control diagnostics report zero non-monotonic steps and approximately 20.852 ms
  maximum residual; and
- output contains 48 kHz stereo PCM and passes full-stream decode.

## Acceptance criteria

- Timestamp anomalies no longer block valid PCM audio.
- A/V start alignment uses the first source audio timestamp.
- Audio duration and packet PTS derive from integer sample counts.
- Writer PTS are gapless by construction, not by repeated timestamp rounding.
- Crop, sync reporting, normalization, and muxing share one clock contract.
- No PCM samples are modified, reordered, inserted, or removed.
- Routine callback jitter remains structured telemetry without warning noise.
- `inspect` and `convert` cannot diverge in validation or diagnostics.
