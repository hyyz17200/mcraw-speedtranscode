# Batch D.1-A: Output Validation and Preflight Boundary

Date: 2026-07-15
Status: Implemented; full FFmpeg/Vulkan runtime verification is pending a
workspace-local dependency restore.

## Scope

This batch removes the unconditional full MOV packet scan from normal
conversion timing and removes the duplicate full-resolution Vulkan capability
preflight. It does not change encoder async depth, queue allocation, staging,
pixel semantics, or fallback policy.

## Implemented contract

- Normal conversion reopens the completed MOV and validates stream metadata,
  codec tag, ProRes profile/color contract, trailer readability, and the
  writer's internal video packet count/PTS/duration invariants.
- `convert --verify-output` retains the explicit packet-by-packet MOV scan for
  release/test gates and adds its time to `output_validation`.
- Sidecars now separate `startup_preflight`, `conversion_core`,
  `output_validation`, and `process_wall` timing entries.
- Vulkan capability probing retains a small functional smoke but no longer
  allocates a full-resolution frame pool and encoder before the real writer.
- CPU and Vulkan mux paths reject missing, non-increasing, or non-positive
  duration video packets before writing them.

## Verification

- `git diff --check`: passed.
- A fresh FFmpeg/Vulkan configure was attempted, but this checkout has no
  usable local FFmpeg/vcpkg installation; the existing build cache points to
  an older absolute workspace path. The runtime and full integration tests
  therefore require dependency restoration before execution.
- No async-depth or queue-separation experiment is included in this batch.
