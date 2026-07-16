# Batch F Hardware Matrix

Date: 2026-07-16

## Current host

`list-capabilities` and `vulkaninfo --summary` agree that this host exposes one
Vulkan physical device:

| Vendor | Device | Driver | Vulkan API | Compute queues | Result |
|---|---|---:|---:|---:|---:|
| NVIDIA | GeForce RTX 3060 | 610.62 | 1.4.341 | 8 in selected family | Pass |

The full 240-frame precise release candidate, Release tests, validation-waiver
reproduction, FFmpeg decode, seek, and VLC checks all ran on this device.

Windows also reports virtual display adapters, but the Vulkan loader does not
expose them as physical Vulkan devices. They are not counted as vendor coverage.

## Missing coverage

No AMD or Intel Vulkan physical device is installed. No second NVIDIA driver
generation can be tested without changing the host driver, which this release
gate does not authorize or simulate. These three rows remain release-blocking:

| Coverage row | Status |
|---|---|
| AMD Vulkan hardware | Not available on host |
| Intel Vulkan hardware | Not available on host |
| Second NVIDIA driver generation | Not installed |

On each future host, archive `list-capabilities`, run the complete Release test
suite, convert the 240-frame real sample with forced Vulkan, run
`validate-release-candidate.ps1`, and record device/driver identity from the
sidecar. Capability enumeration alone is not a passing result.

