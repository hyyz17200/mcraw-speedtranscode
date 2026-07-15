# mcraw-transcoder

`mcraw-transcoder` 是一个以正确性为先的 MotionCam `.mcraw` → ProRes 422 HQ
转码器。稳定 CPU pipeline 仍是默认/reference；可选 Vulkan backend 使用应用 compute
shader 直接写 FFmpeg-owned frame pool，再交给 `prores_ks_vulkan`，不上传或回读未压缩 YUV。

当前版本：`0.1.0`，许可证：`GPL-3.0-or-later`。

## v0.1 能力

- 官方 MotionCam decoder 作为 compression 6/7 的 CPU RAW 真值。
- 独立读取并校验 MCRAW 帧索引和压缩 payload。
- RGGB、BGGR、GRBG、GBRG。
- 每 CFA 位置黑电平和白电平；保留负值与超白。
- librtprocess AMaZE、RCD、IGV、DCB、LMMSE；RCD 保持默认。
- 线性 DWG Capture Sharpening 默认力度 `0.4`。
- DNG CameraNeutral → xy 迭代、inverse-CCT 双矩阵插值。
- ForwardMatrix 和 ColorMatrix + Bradford 两条颜色路径。
- XYZ D50 → D65 → DaVinci Wide Gamut。
- 精确 DaVinci Intermediate 解析 OETF/EOTF，以及按转换实例缓存、由解析公式生成的 LUT。
- BT.2020 non-constant-luminance RGB→YCbCr、left-sited quality 4:2:2、
  确定性 dither 和 10-bit video-range packing。
- FFmpeg `prores_ks` ProRes 422 HQ、源时间戳、PCM 音频和 sidecar JSON。
- OpenMP 行并行、有界多帧并行、多上下文并行 ProRes 编码流水线、
  CPU/RAM 感知的自动执行计划。
- FFmpeg-owned Vulkan device、GPU RGB→10-bit 4:2:2、timeline semaphore、
  bounded frame/job/packet queues、异步 Vulkan ProRes 和独立 mux worker。
- auto/forced GPU selection、稳定 UUID/PCI/name selector、CPU fallback、结构化 queue/
  transfer/mux telemetry，以及成功验证后才 rename 的 partial-file 策略。
- `inspect`、`convert`、`extract-frame`、`validate`、`benchmark`、
  `print-effective-config`、`list-capabilities`。

## 环境

- Windows 10 或 11
- 支持 AVX2 的 x86-64 CPU（默认构建以 AVX2 为最低 CPU 基线）
- Vulkan GPU backend 需要支持 compute、timeline semaphore 和 storage-compatible
  10-bit 4:2:2 image；CPU backend 不需要 GPU
- Visual Studio 2022 C++ workload
- CMake 3.25+
- Git
- vcpkg（可由仓库脚本安装到忽略的 `.deps/`，也可使用现有安装）

依赖在配置时锁定并获取：

- MotionCam decoder commit `06bf1a8`（`release/0.2`）
- librtprocess `0.12.0`
- Catch2 `v3.15.0`
- FFmpeg `8.1.2`（由固定 vcpkg baseline 解析）
- nlohmann-json 使用固定 MotionCam decoder 随附版本，避免跨 DLL ABI 混用

## 构建

在 “Developer PowerShell for VS 2022” 中：

```powershell
# 如果尚未安装 vcpkg，先执行一次：
.\scripts\bootstrap-vcpkg.ps1

cmake --preset msvc-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset msvc-release
ctest --preset msvc-release
```

脚本固定安装 vcpkg `2026.06.24` 到 `.deps/vcpkg`，并为当前 PowerShell
进程设置 `VCPKG_ROOT`。如果使用已有 vcpkg，请自行设置同名环境变量；manifest
中的 baseline 仍会固定端口版本。

可执行文件通常位于：

```text
build/msvc-release/Release/mcraw-transcoder.exe
```

## 使用

```powershell
$exe = '.\build\msvc-release\Release\mcraw-transcoder.exe'

& $exe inspect '.\mcraw_sample\sample.mcraw' --raw-json
& $exe validate '.\mcraw_sample\sample.mcraw' --frame 0 --compare-fused
& $exe benchmark '.\mcraw_sample\sample.mcraw' --frames 8
& $exe print-effective-config
& $exe convert '.\mcraw_sample\sample.mcraw' '.\output.mov'
& $exe convert '.\mcraw_sample\sample.mcraw' '.\output-gpu.mov' `
    --config '.\config\vulkan-gpu-pipeline.json'
```

转换只在成功写完 trailer 后把 `.partial.mov` 原子改名为目标文件。默认不覆盖已
存在输出；需要时显式添加 `--overwrite`。

有效配置可从 [config/default.json](config/default.json) 开始修改。Vulkan opt-in
预设为 `config/vulkan-precise.json`、`config/vulkan-balanced.json` 和
`config/vulkan-fast.json`；CPU backend 仍是默认。precise 使用 FP32 intermediate，
balanced 使用 FP16 storage + FP32 compute，fast 另外使用 FP32 analytic DI；三者均
保留 precise GPU RCD、quality chroma 和 deterministic dither：

```powershell
& $exe convert input.mcraw output.mov --config '.\config\default.json'
```

质量选项及对比配置：

```json
{
  "schema_version": 1,
  "demosaic": "rcd",
  "capture_sharpening": 0.4,
  "capture_sharpening_threshold": 0.002
}
```

`demosaic` 可设为 `rcd`、`amaze`、`igv`、`dcb` 或 `lmmse`。Capture
Sharpening 在相机 RGB 转换到线性 DWG 后、DaVinci Intermediate OETF 前增强中性亮度细节；
默认力度为 `0.4`，设为 `0.0` 可关闭；阈值同样在线性 DWG 域。

可直接使用仓库中的 `config/compare-amaze.json`、`config/compare-dcb.json`、
`config/compare-lmmse.json` 和 `config/compare-capture-sharpening.json`。

`cpu_threads` 是整个进程的 CPU 线程预算，`max_parallel_frames` 是同时在途的帧数上限；
两者为 `0` 时自动选择。自动模式至少为每帧保留两个线程、最多同时处理八帧，并使用
不超过当前可用物理内存四分之一的保守预算。低内存机器可显式设置：

```json
{
  "schema_version": 1,
  "cpu_threads": 4,
  "max_parallel_frames": 1
}
```

GPU 配置的 `backend` 可为 `cpu`、`auto` 或 `vulkan`。`auto` 会先完成 device、
真实尺寸 frame context/encoder 和小型 shader→encoder smoke test；失败时把具体原因写入
sidecar 并按 `fallback` 回到 CPU。`vulkan` 为强制模式，任何 preflight/runtime 错误都会
终止当前 partial 文件，不会在同一个 MOV 中途切换 CPU。`async_depth` 同时控制 GPU slots
及有界队列的基准深度；当前已验证的精度模式为 `fp32`，`fp16` 会明确拒绝。

GPU sidecar 中 `gpu_resident=true` 严格表示 encoder-compatible YUV frame 没有 CPU
upload/readback；`rgb_upload_bytes` 单独报告仍由 CPU RAW/颜色前端产生的 FP32 RGB staging
流量，不能把它理解为完整 RAW pipeline 已零 PCIe copy。

`--compare-fused` 会在指定帧上同时运行解析参考路径和 LUT/融合路径，并报告全部
10-bit Y/Cb/Cr 样本的差异数量与最大码值误差。

## Resolve 导入

v0.1 的 MOV 写入：

- range: video/legal
- YCbCr matrix: BT.2020 non-constant luminance
- primaries/transfer: unspecified

QuickTime 标签不能完整表达 DaVinci Wide Gamut / DaVinci Intermediate。请在
DaVinci Resolve 中手动把 Input Color Space 指定为
`DaVinci Wide Gamut / DaVinci Intermediate`。完整 profile、矩阵、白点、配置、
fallback 和耗时写在 `output.mov.json`。

## 当前边界

- 只实现首个 DWG/DaVinci Intermediate profile。
- ProRes packing matrix 与 chroma siting 仍需通过 Resolve chart 实测冻结；sidecar
  明确标记当前策略。
- `validate` 使用官方 decoder 生成可复现 RAW hash；自定义/GPU decoder 尚未加入，
  因此现在不存在“官方 vs GPU bit-exact”结论。
- 转码器不做 RAW、色度或时域降噪，噪声处理留给后期调色；坏点、镜头阴影、
  几何校正、其他 Log profile 和 GUI 不在 v0.1 范围。
- Vulkan 路径目前从 CPU-produced TargetLog RGB 开始；RAW decode、校准、demosaic、
  camera color、sharpening 和 DI encode 尚未迁移到 GPU。FFmpeg 自带 ProRes DCT shader
  的 GPU-assisted race diagnostic 仍是生产发布 gate，详见 GPU Phase 验证文档。
- `mcraw_sample/` 和输出文件不会提交到版本控制。

架构依据见 [docs/MCRAW_ProRes_RouteB_Architecture_Spec_CN.md](docs/MCRAW_ProRes_RouteB_Architecture_Spec_CN.md)，
冻结决策见 [docs/adr/README.md](docs/adr/README.md)，Vulkan 实测结果、兼容性矩阵和
未关闭的发布门槛见 [docs/GPU_PHASE8_PRODUCTION.md](docs/GPU_PHASE8_PRODUCTION.md)，
下一阶段的正式路线与当前 Stage 0 基线合约分别见
[docs/GPU_PIPELINE_FORMAL_ACTION_GUIDE.md](docs/GPU_PIPELINE_FORMAL_ACTION_GUIDE.md) 和
[docs/GPU_STAGE0_BASELINE.md](docs/GPU_STAGE0_BASELINE.md)。
