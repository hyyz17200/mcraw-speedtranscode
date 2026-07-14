# GPU Pipeline Phase 1 Validation

Phase 1 owns no application-created logical device. `VulkanRuntime` enumerates
physical devices with a temporary probe instance, selects a stable identity,
then asks FFmpeg to create the only long-lived Vulkan instance/device through
`av_hwdevice_ctx_create`. The selected UUID is verified against the resulting
`AVVulkanDeviceContext` before the runtime is accepted.

Supported selectors:

- `auto`: discrete, integrated, virtual, then other; software devices excluded
- decimal enumeration index, for interactive convenience only
- case-insensitive device-name substring
- `uuid:<hex>`
- `pci:<vendor-id>:<device-id>` in hexadecimal

Use `list-capabilities` for device, driver, UUID, queue-family and enabled
extension telemetry. Use `vulkan-smoke` for deterministic lifetime testing.

## 2026-07-14 local acceptance

- Device: NVIDIA GeForce RTX 3060, discrete, Vulkan 1.4.303
- Driver: NVIDIA 576.02
- FFmpeg compute queue: family 2, 8 queues
- Release unit tests: 27/27 passed
- `vulkan-smoke --iterations 1000`: passed in 275892.7615 ms
- Mean create/destroy time: 275.8927615 ms
- One additional `--validation` create/destroy: completed with no validation
  error; the loader reported active OBS and Bandicam implicit layers and
  validation-setting warnings, so formal performance captures should disable
  those overlays as required by the audit

The Vulkan encoder remains unavailable at the backend-selection level in this
phase. No frame context, upload bridge, encoder open, shader, or zero-copy claim
is made here.
