# Batch F Compatibility Gate

Date: 2026-07-16
Candidate: `test-output/batch-f/compatibility-candidate.mov` (local artifact)

## Candidate identity

- source: `260710_142121_VIDEO_49mm.mcraw`, all 240 frames;
- output: 4096x3072 ProRes 422 HQ, `yuv422p10le`, PCM audio;
- Vulkan precise pipeline: 37.189 fps, 6.454 s process wall;
- container duration: 8.031021 s;
- video/audio duration: 7.990744 / 8.031021 s;
- output size: 1,402,782,733 bytes.

## Automated readers

| Reader | Decode | Seek | Duration | Audio | Result |
|---|---:|---:|---:|---:|---:|
| FFmpeg/ffprobe | Full pass | 0/2/4/6/7.5 s pass | Pass | Full pass | Pass |
| VLC | Full headless playback exit | MOV index used | Pass | Not monitored in headless run | Pass |

The reusable runner is `scripts/validate-release-candidate.ps1`. It checks the
codec/profile/pixel format, packet count, complete video and audio decode,
random-access decode points, and VLC playback when VLC is installed.

## NLE status

DaVinci Resolve is installed and `scripts/validate-resolve-import.py` provides
a repeatable import/timeline/property test using Blackmagic's supported API.
This machine currently has external scripting disabled in Resolve preferences,
so the API returned no Resolve connection and the import test is **not passed**.
Decode, seek, visual color, duration, and audio sync must not be inferred from
the FFmpeg/VLC result.

Adobe Premiere Pro is not installed on this machine. Its gate is **not run**.

## Decision

The desktop-player portion of the compatibility gate is closed by VLC. The
Resolve and Premiere portions remain release-blocking until their actual runs
are recorded. The candidate and scripts are ready for those runs without a new
build.
