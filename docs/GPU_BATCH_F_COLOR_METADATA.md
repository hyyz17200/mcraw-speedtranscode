# Batch F Color Metadata and Chroma Decision

Date: 2026-07-16
Status: product contract frozen

## Contract

- image identity: DaVinci Wide Gamut / DaVinci Intermediate;
- packing matrix: BT.2020 non-constant luminance;
- range: video/legal;
- chroma filter: quality five-tap by default;
- 4:2:2 chroma sample location: left;
- MOV primaries and transfer: unspecified;
- the sidecar explicitly records DWG/DI and the manual input color-space name.

BT.2020 NCL describes the RGB-to-YCbCr packing coefficients. It does not claim
that the RGB primaries are BT.2020. MOV/FFmpeg has no standard enumerated tags
for DWG primaries or the DaVinci Intermediate transfer function, so writing
BT.2020 primaries or a different standard TRC would be false metadata.

## Boundary correction

The pixel implementation already sampled chroma at even/left positions, and
the configuration and sidecar described left siting. The CPU and Vulkan FFmpeg
frame metadata had nevertheless remained `AVCHROMA_LOC_UNSPECIFIED`. Batch F
sets `AVCHROMA_LOC_LEFT` on every CPU/Vulkan codec context and frame, including
the Vulkan preflight.

The MOV/ProRes muxer does not preserve a queryable chroma-location value in the
reopened `AVCodecParameters`; ffprobe reports it as unspecified. The release
gate therefore validates the left-position sampling math, the encoder input
contract, CPU/Vulkan parity, and the sidecar declaration. It does not make an
unverifiable claim that the container carries a left-siting enum.

This contract is shared by CPU and Vulkan backends. Reader-specific visual
color validation remains part of the NLE compatibility gate; it no longer
changes the declared packing semantics silently.
