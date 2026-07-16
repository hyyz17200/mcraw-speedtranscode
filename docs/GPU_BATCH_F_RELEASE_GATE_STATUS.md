# Batch F Release Gate Status

Date: 2026-07-16
Decision: local Batch F actions complete; production release not approved

| Gate | Status | Evidence / blocker |
|---|---|---|
| FFmpeg DCT GPU-AV diagnostic | Waived | Narrow waiver for the pinned 143/167 diagnostic; all other diagnostics block |
| Reader/NLE compatibility | Partial | FFmpeg and VLC pass; Resolve scripting disabled; Premiere absent |
| Chroma and color metadata | Pass | CPU/Vulkan left siting; BT.2020 NCL packing; DWG/DI sidecar identity |
| Hardware and driver matrix | Partial | RTX 3060 / 610.62 passes; AMD, Intel, and second NVIDIA driver unavailable |
| One-hour real material | Blocked | Longest real source is about 27.5 seconds; logical-hour synthetic test does not waive this |
| Batch/restart/cancel/device-loss/resources | Partial | Real batch, restarts, failure cancellation semantics, and logical-hour resource guard pass; actual device loss not injected |
| 24 fps minimum | Pass | 240-frame precise: 37.853 fps; repeated real batch: 35.78-38.34 fps |

## Release decision

All actions executable on the current repository, samples, software, GPU, and
driver have been performed. The remaining rows require external state that is
not present on this host: enabling Resolve external scripting and completing
its visual checks, installing/running Premiere, AMD and Intel Vulkan hardware,
a second NVIDIA driver generation, a one-hour real MCRAW source, and a real or
deterministic device-loss injection environment.

These are not converted into waivers. Until they pass or receive separate
written waivers, the CPU backend remains the default and Vulkan remains opt-in.
Batch F produced reproducible scripts and exact rerun instructions for every
open row.

