# Third-party components

The application is GPL-3.0-or-later as a combined work. Third-party components
retain their own copyright notices and licenses.

| Component | Locked version | Role | Upstream license |
|---|---|---|---|
| MotionCam decoder | commit `06bf1a8` (`release/0.2`) | Container API and bit-exact CPU RAW truth | Apache-2.0 |
| librtprocess | `0.12.0` | AMaZE, RCD, IGV | GPL-3.0-or-later |
| FFmpeg | `8.1.2` via vcpkg baseline `cd61e1e` | ProRes, MOV, PCM | Depends on build configuration; build must remain GPL-compatible |
| Vulkan Headers / Loader | `1.4.350.0` via the same vcpkg baseline | Vulkan API and runtime dispatch | Apache-2.0 OR MIT / Apache-2.0 |
| glslang tools | `16.3.0#1` via the same vcpkg baseline | Build-time GLSL to SPIR-V compiler for FFmpeg Vulkan codecs | Apache-2.0, BSD-3-Clause, MIT, GPL-3.0-or-later components |
| Catch2 | `v3.15.0` | Tests only | BSL-1.0 |
| nlohmann/json | Version bundled by locked MotionCam decoder | JSON | MIT |

No vkdt or RawTherapee source is copied into this repository. librtprocess is
fetched and linked as its own upstream project. Distributions must include all
corresponding license notices and source obligations.
