# Reproducible FFmpeg Vulkan build

The GPU backend uses the repository's vcpkg manifest. It does not discover an
arbitrary `ffmpeg.exe` from `PATH`.

Locked inputs:

- vcpkg builtin baseline: `cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`
- FFmpeg port version at that baseline: `8.1.2`
- FFmpeg features: `avcodec`, `avformat`, `swresample`, `swscale`, `vulkan`
- Vulkan Headers and Loader: `1.4.350.0` at the same baseline
- glslang host tools: `16.3.0#1` at the same baseline
- Windows compiler: MSVC selected by the checked-in CMake presets

The checked-in `cmake/vcpkg-ports/ffmpeg` overlay delegates to the baseline's
FFmpeg port recipe and patches, but adds `glslang[tools]` as a host dependency.
This is required because `prores_ks_vulkan` depends on FFmpeg's
`spirv_compiler` configure capability; Vulkan headers alone do not enable the
encoder.

Run `scripts/bootstrap-vcpkg.ps1`, then configure and build through
`scripts/build.ps1`. Both scripts require the vcpkg checkout to resolve to the
exact builtin baseline commit above; this matters because the overlay delegates
to that checkout's FFmpeg recipe and patch set. The manifest rebuilds FFmpeg
when its feature ABI does not include `vulkan`.

After building, run:

```powershell
.\scripts\check-vulkan-capability.ps1
```

The check calls the project's `list-capabilities` command. That command probes
`avcodec_find_encoder_by_name("prores_ks_vulkan")` in the libraries actually
linked into `mcraw-speedtranscode`, and records `av_version_info()` plus the linked
libavcodec configuration. A system `ffmpeg.exe -encoders` result is diagnostic
only and cannot satisfy this gate.

`MCRAW_ENABLE_VULKAN=OFF` remains supported for a CPU-only build. The proven CPU
backend stays the default even when the Vulkan dependency and encoder probes
pass.
