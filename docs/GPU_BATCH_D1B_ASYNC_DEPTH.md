# Batch D.1-B: Effective Vulkan Encoder Async Depth

Date: 2026-07-15
Status: Implementation committed; matched depth matrix pending a completed
FFmpeg build.

The vendored FFmpeg port now applies
`prores-vulkan-async-depth.patch`, changing the ProRes Vulkan compute exec
pool from a hardcoded depth of one to the requested `async_depth`. The
application sidecar records requested/effective depth, compute pool size, and
compute queue family/index. The effective depth is reported only after the
real writer/encoder has initialized; capability probing does not claim a
full-resolution encoder was initialized.

Required follow-up remains a warm-up plus three formal runs at depth 1, 2, 4,
and 8 on the same executable, input, and configuration, with output and
telemetry gates from the formal guide. Queue separation is explicitly out of
scope until this matrix is measured.

Verification on 2026-07-15:

- `git diff --check`: passed.
- vcpkg accepted the overlay patch and started the FFmpeg build.
- The FFmpeg/MSVC build exceeded the available 300-second command window and
  produced no installable headers/libraries, so no depth benchmark or runtime
  test is claimed by this commit.
