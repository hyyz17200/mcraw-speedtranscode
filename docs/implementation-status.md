# v0.1 Implementation Status

## 已实现源码边界

- CMake/vcpkg/MSVC 2022 构建骨架
- MCRAW 独立索引和压缩 payload 读取
- MotionCam 官方 CPU 解压适配
- 元数据标准化和来源可见 warning
- RAW 黑白场、RCD/AMaZE/IGV/DCB/LMMSE；RCD 保持默认
- 默认力度 `0.4` 的线性 DWG Capture Sharpening；转码器不做降噪
- 双矩阵、ForwardMatrix、Bradford、DWG、DaVinci Intermediate
- 每转换实例 DI LUT、融合 Camera→DWG→DI→YCbCr、quality 4:2:2、dither、legal range 量化
- OpenMP 行并行、CPU/RAM 感知的有界多帧并行和有序 mux
- FFmpeg ProRes/MOV/PCM/时间戳和 sidecar
- 七个 CLI 子命令和单元测试

## Windows 10 / MSVC 2022 验收结果

- MSVC 2022 Release：主库、CLI、测试程序构建通过
- 单元测试：18/18 通过
- `mcraw_sample/`：1 个样本完成 inspect、首末帧 RAW 解压与 hash
- 样本音频：48 kHz、双声道、377 块及源时间戳全部读取
- CPU compute benchmark：自动 16 线程/8 在途帧约 3.7～4.0 fps，平均整机 CPU 约 78.6%
- 相比原始约 0.26 fps 参考实现，compute throughput 提升约 15 倍
- 首末 4K 帧各比较 25,165,824 个 10-bit 样本；融合路径最大差异 1 LSB，
  差异比例分别约 0.00056% 和 0.00051%
- 2 帧 ProRes 422 HQ + PCM MOV round-trip 通过
- 优化后完整 240 帧 ProRes 422 HQ + PCM MOV 转换及 FFmpeg 全流解码通过：
  95.251 s、2.52 fps；原始实现 931.2 s，端到端加速约 9.8 倍
- ffprobe：`apch`、`yuv422p10le`、解码帧为 video range、BT.2020 NCL、
  90 kHz 视频时基、48 kHz PCM；240 帧视频 7.990744 s，音频 8.031021 s
- 优化后完整输出：1,402,859,919 字节，无残留 partial 文件；sidecar 记录 240 帧、
  377 个音频块，音频相对视频尾端 +40.288409 ms
- 优化前后完整 PCM SHA-256 均为
  `04c5eb506259a5eb2f956226ce627cc4b3e773faf8c9d67d645fed5d8468736a`

## 2026-07-14 质量选项对比

同一 4096×3072 样本、16 帧、Release、16 CPU 线程、8 帧在途、每帧 2 线程，
compute-only 测量：

| 配置 | 吞吐 fps | 相对 RCD 吞吐 | demosaic 平均耗时 |
|---|---:|---:|---:|
| RCD（两轮平均） | 4.477 | 基准 | 405.0 ms |
| AMaZE | 3.097 | -30.8% | 1219.0 ms |
| DCB（两轮平均，2 次迭代、enhance 关闭） | 3.493 | -22.0% | 835.2 ms |
| LMMSE（两轮平均，2 次迭代） | 3.086 | -31.1% | 1159.2 ms |

在 RCD 上单独打开 Capture Sharpening `0.25`，测得 3.955 fps，较同轮 RCD
下降约 11.1%。多帧并行下各阶段计时有重叠，吞吐差是评估总体开销的主要指标。

完整 240 帧对比输出均通过 FFmpeg 全流解码：AMaZE 为 2.217 fps，RCD +
Capture Sharpening `0.25` 为 2.655 fps。两者均为 4096×3072 ProRes 422 HQ
`yuv422p10le`，保留 48 kHz 双声道 PCM。

## 仍需人工外部验收

- DaVinci Resolve chart、chroma siting 与手动 Input Color Space 工作流实测
- 完整 240 帧输出的人工播放/画面检查（自动解码和流参数检查已通过）

## 2026-07-14 GPU 下一阶段 Stage 0

- 正式采用 24 fps 最低目标、30 fps 扩展目标和 `fp32/precise` 最终 YUV
  最大差异不超过 1 LSB 的行动口径。
- 已建立固定 corpus 合约：真实 4096×3072 / 240 帧 compression 7 样本的
  首帧、中间帧和末帧，以及四 CFA、校准范围、饱和色和高频图案定义。
- 新增与生产 `CpuPipeline` 相同边界的 calibrated mosaic、Camera RGB、
  sharpened TargetLinear、TargetLog 和 YUV extract stages。
- Vulkan RGB→YUV 已使用 timestamp query；CPU fence wall time 不再冒充 GPU
  execution time。sidecar/CLI 输出样本数、total/mean/P50/P95/P99/min/max。
- 初次重复 capture 的 21 个 artifact hash 全部稳定。
- 初次强制 Vulkan 完整基准完成一次 warm-up 和三次正式运行：中位数
  7.133 fps，范围 7.098–7.412 fps；每次 240 个 GPU timestamp，RGB→YUV
  GPU mean 的 run median 为 13.007 ms/frame；device-global GPU 平均利用率约
  18.3%，job queue peak 7、packet queue peak 1、backpressure 0。
- Release build、43 项 CTest、GPU-assisted RGB→YUV golden、30 帧 CPU/Vulkan
  全流解码、auto fallback、forced Vulkan cleanup 全部通过。

Stage 0 已提交为 rollback point `622070c`。提交后 capture manifest 指向该 commit、
记录 `dirty=false`，并保持相同 executable/config/corpus 与 21 项 artifact hashes。
Stage 1 的 Camera RGB 输入、shader passes、uniform、golden、同步及 benchmark 合约见
`GPU_STAGE1_CAMERA_RGB_TECHNICAL_DESIGN.md`。

## 2026-07-15 GPU Stage 1A foundation

- `CpuPipelineOutput` 取代隐式 boolean producer seam，明确区分 CPU packed YUV、
  TargetLog RGB 和 Camera RGB；当前 CLI 仍固定使用已验收的 Stage 0 TargetLog 路径。
- 新增独立的 `VulkanCameraPipelineResources`：每 slot 三张 host-visible Camera RGB
  upload buffer、两组三平面 device-local FP32 ping-pong buffer、独立 command buffer 和
  fence；继续复用 FFmpeg-owned device、compute queue 及 queue lock。
- test-only readback 必须显式启用；round-trip 通过 transfer barrier 验证 upload →
  device-local intermediate → readback 的逐 bit 一致性，production 配置不会分配 readback。
- writer telemetry/sidecar 开始记录实际 `pipeline.entry`、precision、demosaic/color
  solution location，并分别统计 TargetLog 与 Camera RGB FP32 upload bytes。
- 本批次没有 color/sharpen/DI shader，不改变 Stage 0 输出像素或 fallback 行为。

## 2026-07-15 GPU Stage 1B color/exposure

- 新增独立 `camera_to_dwg.comp.glsl` FP32 pass；CPU 继续完成 FP64 color solution，
  GPU 只接收最终 3x3 row-major matrix 和独立 exposure scale。
- 64-byte push-constant ABI 已用 C++ size/offset assertions 冻结；每 slot 使用三张
  Camera input 和 ping A 三张 TargetLinear output 的六 binding descriptor set。
- test-only readback 使用 compute→transfer→host 明确 barriers；production 仍不分配
  readback buffer。
- synthetic golden max/RMSE 为 `2.38419e-7 / 1.87853e-8`；真实 4096×3072
  Stage 0 首帧为 `2.38419e-7 / 1.57234e-8`，均通过 `2e-5 / 1e-6` 门槛。
- color pass 已有独立 GPU timestamp summary；validation-enabled 4K 单样本为
  `11.9165 ms`，仅作为 shader 验证数据，不作为性能承诺。
- 本批次仍不切换 production writer；详细报告见
  `GPU_STAGE1B_COLOR_VALIDATION.md`。

## 2026-07-15 GPU Stage 1C sharpening

- 新增独立 `sharpen_target_linear.comp.glsl` FP32 pass，从 device-local ping A
  读取并写入 ping B，不做原地更新、clamp、FP16 或 pass fusion。
- 保持 CPU 的 BT.2020 luma、四邻域 cross blur、边缘坐标钳制、soft threshold
  和同量 RGB delta 语义；16-byte push-constant ABI 已冻结。
- color→sharpen 使用 compute write/read barrier；仅 test route 再把 ping B
  barrier 到 transfer readback，production 仍无中间图像回读。
- synthetic edge/threshold/negative golden max/RMSE 为
  `5.96046e-8 / 2.00631e-9`；真实 4096×3072 Stage 0 首帧为
  `2.38419e-7 / 1.94043e-8`，均通过 `3e-5 / 2e-6` 门槛。
- sharpening pass 已有独立 GPU timestamp summary；validation-enabled 4K
  单样本为 `4.3264 ms`，仅作为 shader 验证数据，不作为性能承诺。
- 本批次仍不切换 production writer；详细报告见
  `GPU_STAGE1C_SHARPENING_VALIDATION.md`。

## 2026-07-15 GPU Stage 1D DaVinci Intermediate

- 新增独立 `davinci_intermediate.comp.glsl` FP32 precise pass，从 sharpened
  ping B 读取并在明确 barrier 后复用 ping A 写入 TargetLog。
- 现有两段各 65,536-entry FP32 LUT 通过 staging 一次性上传到 524,288-byte
  pipeline-owned device-local buffer，由所有 slot 共享；高于 100 的值仍走 analytic DI。
- `preserve_by_curve`、`clamp_zero`、`error` 策略 ABI 已冻结；每 slot 独立
  4-byte status word 检测 error-policy negative 和 shader-created non-finite，
  只在 fence signal 后读取并以 `processing_failed` 终止。
- LUT boundary synthetic golden max/RMSE 为 `1.19209e-7 / 2.98023e-8`；
  真实 4096×3072 Stage 0 首帧为 `5.96046e-8 / 1.17004e-8`，均通过
  `3e-5 / 2e-6` 门槛；1x1、重复确定性和两类 fault injection 也已通过。
- DI pass 已有独立 GPU timestamp summary；validation-enabled 4K 单样本为
  `3.48774 ms`，仅作为 shader 验证数据，不作为性能承诺。
- 本批次仍不切换 production writer；详细报告见
  `GPU_STAGE1D_DAVINCI_INTERMEDIATE_VALIDATION.md`。

## 2026-07-15 GPU Stage 1E resident chain

- production Vulkan route 已切换到 `camera_rgb_f32`：单一 slot command buffer
  串联 color/exposure、sharpening、DI、RGB-to-YUV，并直接写 encoder-owned
  `AVVkFrame`；Stage 0 TargetLog overload 仅保留为 rollback point。
- Camera RGB upload 逐帧精确计数；production TargetLog upload、TargetLinear/
  TargetLog/YUV pixel readback 均为 0。每帧只额外读取 4-byte control status。
- ping-pong buffer reuse、status reset/read、AVVkFrame layout/access 与 timeline
  semaphore 均有显式 barrier/ownership；validation 发现的宽泛 source stage 已收窄为
  实际 compute shader stage，复测为 0 个应用 validation error。
- synthetic resident final Y/Cb/Cr 均为 0 LSB；negative-policy fault injection、
  decodable MOV、partial cleanup、auto fallback 和 forced invalid-device 均通过。
- 30 帧 4096x3072 E2E 中 Camera RGB upload 为 4,529,848,320 bytes、TargetLog
  upload 为 0、四个 GPU pass 各 30 个 timestamp；CPU/Vulkan 全流解码与音频/PTS
  检查通过。详细报告见 `GPU_STAGE1E_RESIDENT_CHAIN_VALIDATION.md`。
