# Batch E-A: official compression 6/7 decoder truth

Date: 2026-07-16

## Frozen upstream

- Repository: `https://github.com/mirsadm/motioncam-decoder.git`
- Commit: `2c49edb17277c07989ff90bd3a3bf557c2f68b4a`
- License: Apache-2.0 (`LICENSE` Git object
  `261eeb9e9f8b2b4b0d119366dda99c6fd7d35c64`)
- Project build flags: C++17; upstream Release builds request `-O3` on non-MSVC;
  this repository additionally applies its existing per-target AVX2 option when
  `MCRAW_ENABLE_AVX2=ON`.

The previous pin, `06bf1a87bca218bfd555b6b2e4394a220c39fbc9`
(`release/0.2`), accepted only compression 7 and returned a
`std::vector<uint16_t>` whose element count was incorrectly specified in bytes.
The new frozen revision adds the independent `RawData_Legacy.cpp` decoder for
compression 6 and changes `Decoder::loadFrame` to return an exactly sized byte
buffer. The application copies those bytes into its typed, contiguous
`RawMosaicU16` ownership boundary after requiring an exact `width * height * 2`
length.

Relevant frozen upstream Git objects:

| Source | Git object |
|---|---|
| `lib/Decoder.cpp` | `1b9cb3cff9d8534c3672f12e22611e2f3d78897a` |
| `lib/RawData.cpp` (compression 7) | `ac9e943e09eb3b59adf8f4eb74e04e2c8b533884` |
| `lib/RawData_Legacy.cpp` (compression 6) | `ab36a6c6b9c2a31089523518c201308259348e63` |

No vendored source is modified. The FetchContent commit remains the official
bit-exact reference and production decoder for both formats.

## Input and output contract

For a frame-index record, `motioncam::Decoder` reads the indexed `BUFFER`
payload and the immediately following `METADATA` JSON. The metadata supplies
`width`, `height`, and `compressionType`; normalization additionally validates
the visible dimensions and accepts only compression 6 or 7. Successful decode
must return exactly `width * height * sizeof(uint16_t)` bytes. The application
then owns a contiguous native-endian U16 mosaic of exactly `width * height`
samples.

Metadata-only calls use upstream `loadFrameMetadata`, so inspection no longer
decodes and allocates an unused full RAW frame.

## Corpus state

The two locally available real samples are both compression 7. Their existing
Stage 0 corpus remains the compression 7 golden source. No real compression 6
sample is present in `mcraw_sample`. Per the 2026-07-16 project decision, the
compression 6 material-level validation gate is explicitly waived. Runtime
policy remains to attempt the official legacy decoder and emit a console warning
that compression 6 has not been validated against a real project corpus.

The upgrade was checked against the previous `release/0.2` decoder at the
first/middle/last frames of both local samples. All six U16 SHA-256 values are
unchanged:

| Sample | Frame | U16 SHA-256 |
|---|---:|---|
| `260623_135705_VIDEO_25mm.mcraw` | 0 | `A0809CB6A1B04AE303388E6B65EF06D5BB0A5142F3FCCD26D7B1E55240A9EE1C` |
| same | 413 | `4A01C76AC4CB641D8C55F143B0B96E10981B07ECD794255CABBCC425045842ED` |
| same | 825 | `8D58CF7D4FB72360DDAE2A2328A08A543F3C9ACF1613201340CC19C3F67AF8D5` |
| `260710_142121_VIDEO_49mm.mcraw` | 0 | `47A45930EC8ECE0E44D657D9093587769F87ADA751A368FF75FD58CEE7A21071` |
| same | 120 | `684A1B0F876C230045BA5C32DB393F5A9807F0514BE0A515EEA8C2D911C6CF43` |
| same | 239 | `40B97E068B62432971C2B76B73697167D92F65D9DA444FA16DD2146F7DF8B82B` |

The MSVC Release build and all 75 registered tests passed after the pin update
(seven optional real-corpus tests were skipped because their extracted artifact
environment variables were not set).

The next Batch E step may establish the bounded worker scheduler and updated
compression 7 baseline. A repository-owned fast decoder must not begin until
the updated official baseline is complete and demonstrates a qualifying need.
