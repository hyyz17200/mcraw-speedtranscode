# Vulkan ProRes GPU Pipeline Audit

状态：待审阅，不包含 GPU 实现代码  
审计日期：2026-07-14  
范围：当前 CPU reference、FFmpeg/MOV 输出、线程与所有权、Vulkan 接入边界、Phase 1～3 文件级计划

## 1. 结论

当前 CPU pipeline 已具备作为 GPU 迁移参考的必要条件：数据类型边界清楚，颜色数学有解析参考，融合路径与参考路径的 10-bit 输出最大差异为 1 LSB，真实 MCRAW 样本可复现，CPU 多帧执行和 ProRes mux 都有界。

GPU 后端不应改写 `CpuPipeline` 或直接替换 `FfmpegWriter`。建议先把现有 FFmpeg 编码/mux 行为抽成保持公开接口不变的 CPU adapter，再增加独立 Vulkan backend。最终只保留一个 Vulkan logical device，选择 **FFmpeg 创建并拥有 device，应用从 `AVVulkanDeviceContext` 借用同一 instance/device/queue**。这最符合本项目目前“没有既有 Vulkan runtime”的事实，也避免后期跨 device 复制。

当前硬阻塞只有一个：仓库链接的 FFmpeg 8.1.2 虽然源码包含 `prores_ks_vulkan`，但 vcpkg manifest 没有启用 `vulkan` feature，因此当前项目库中：

```text
CONFIG_VULKAN=0
CONFIG_PRORES_KS_VULKAN_ENCODER=0
```

系统 FFmpeg 8.1.2、RTX 3060 和 Vulkan SDK 已通过两帧 upload-bridge smoke test，证明 `yuv422p10le -> AV_PIX_FMT_VULKAN -> prores_ks_vulkan -> MOV` 在本机可用。

## 2. 已验证基线

### 2.1 开发环境

| 项目 | 当前状态 |
|---|---|
| OS | Windows 10/11 开发环境 |
| GPU | NVIDIA GeForce RTX 3060, 12 GiB |
| NVIDIA driver | 576.02 |
| GPU Vulkan API | 1.4.303 |
| Vulkan SDK | 1.4.350.0 |
| Validation layer | `VK_LAYER_KHRONOS_validation` 可用 |
| Shader compiler | `glslc` / shaderc 2026.2 可用 |
| 系统 FFmpeg | 8.1.2 full build，`prores_ks_vulkan` 可用 |
| 项目 FFmpeg source | vcpkg 固定 8.1.2 |
| 项目 FFmpeg binary | 当前未启用 Vulkan，需要重建 |
| 编译器 | Visual Studio Build Tools 2022，MSVC x64 preset |

系统 FFmpeg smoke test 使用 128x64、2 帧、ProRes HQ、`async_depth=2`，输出可被 ffprobe 完整读取，结果为 `apch`、`yuv422p10le`、video range、2 帧。

### 2.2 CPU reference

当前验证结果：

- 18/18 单元测试通过；
- 固定样本：`mcraw_sample/260710_142121_VIDEO_49mm.mcraw`；
- 样本尺寸：4096x3072，240 帧；
- 首帧 RAW FNV-1a 64：`4534363536704555902`；
- 首帧压缩 payload：11,222,720 bytes；
- 首帧融合/解析参考比较：25,165,824 个 10-bit samples，600 个不同，最大差异 1 LSB；
- 已记录完整 CPU 输出：ProRes 422 HQ、240 帧、PCM 48 kHz stereo、FFmpeg 全流解码通过；
- 当前仓库的 `test-output/` 不保留大型 MOV，因此正式开始实现前应生成一个小型、可提交的 reference manifest，包含命令、Git commit、配置、RAW hash、关键帧 plane hash、ffprobe JSON 和输出 hash。大型 MOV 可保留为本机测试资产，不必提交 Git。

## 3. 当前 CPU pipeline 的实际数据流

```text
MCRAW file
  |
  +-- independent frame index validation
  +-- MotionCam container/frame metadata
  +-- compressed type 6/7 payload
  v
motioncam::Decoder (one leased instance per concurrent frame)
  v
RawMosaicU16                         uint16 CFA mosaic
  v
black/white calibration             FP64 setup, FP32 output, preserve <0 and >1
  v
RawDemosaicF32                      planar-less CFA buffer in 0..65535 working scale
  v
librtprocess demosaic               RCD default; AMaZE/IGV/DCB/LMMSE optional
  v
CameraRgbF32                        3 planar FP32 channels, still scaled by 65535
  v
per-frame camera color solution     FP64 CameraNeutral/matrix setup
  v
fused production pack
  +-- camera RGB -> linear DWG
  +-- exposure offset
  +-- optional neutral capture sharpening
  +-- DaVinci Intermediate OETF LUT
  +-- encoded RGB -> BT.2020 NCL Y'CbCr
  +-- 5-tap left-sited 4:2:2 downsample
  +-- deterministic dither
  +-- 10-bit video-range clamp/quantize
  v
Yuv422P10                           3 CPU uint16 vectors
  v
FfmpegWriter
  +-- AVFrame / AV_PIX_FMT_YUV422P10LE
  +-- N independent prores_ks contexts
  +-- sequence-ordered AVPacket collection
  +-- PCM S16LE audio
  +-- av_interleaved_write_frame
  v
output.partial.mov
  v
flush -> trailer -> close -> atomic rename
  v
output.mov + sidecar JSON
```

## 4. 每阶段格式、所有权和 GPU 插入点

| 阶段 | 输入 | 输出 | 当前所有权 | 并发 | GPU 迁移位置 |
|---|---|---|---|---|---|
| MCRAW index/read | file offsets | compressed payload/metadata | `McrawReader::Impl` 保存索引；payload 独立 vector | reader 共享 | 保持 CPU；磁盘 I/O 不搬 GPU |
| Official decode | MCRAW compression 6/7 | `RawMosaicU16` | decoder lease + moved vector | 每在途帧独立 decoder | 第一版保持 CPU；后续 Vulkan decode 必须逐像素匹配 |
| Calibration | U16 CFA | `RawDemosaicF32` | 新 FP32 vector | OpenMP rows | Shader A；适合与未来 unpack 合并，但先独立验证 |
| Demosaic | FP32 CFA, 0..65535 | `CameraRgbF32` | 三个新 FP32 planes | librtprocess/OpenMP | Shader B；RCD 为首个 GPU parity 目标 |
| Color solution | per-frame metadata | FP64 matrices/white point | 小型 value object | 每帧 CPU | 保持 CPU setup，将最终矩阵作为 uniform 上传 |
| Fused color/pack | Camera RGB FP32 | `Yuv422P10` | 新 Y/Cb/Cr vectors | OpenMP row partitions | 先拆成可验证 shaders，最终融合需 profiler 证明 |
| CPU ProRes | CPU planar 10-bit | packets | AVBufferRef 持有 moved vectors | 多 codec contexts | 保留为 reference/fallback |
| Vulkan ProRes | Vulkan 10-bit 4:2:2 | packets | AVFrame/AVBufferRef 持有 AVVkFrame | `async_depth` | Phase 3 先 upload；Phase 5 直接 shader write |
| MOV/audio | packets + PCM | MOV | muxer/context RAII | mux 写入串行 | 保持 CPU；压缩 packet 回 CPU 合法 |

4096x3072 单帧主要 CPU buffer 规模：

| Buffer | 约 bytes |
|---|---:|
| U16 CFA | 25,165,824 |
| FP32 calibrated CFA | 50,331,648 |
| 3-plane FP32 RGB | 150,994,944 |
| planar 10-bit YUV stored as U16 | 50,331,648 |

当前自动模式允许 8 帧计算并发，因此 upload bridge 只用于集成验证；如果长期保留，会同时增加 system RAM、VRAM 和 PCIe 压力。

## 5. 颜色变换的冻结语义

GPU 输出必须匹配以下 CPU 行为，不能在 GPU 后端中“顺便修正”。如需改变，必须作为独立、版本化的产品决策。

### 5.1 RAW calibration

每个 CFA 位置独立使用：

```text
normalized = (sample - black[cfa_position]) /
             (white[cfa_position] - black[cfa_position])
```

不在此阶段 clamp；负值和 super-white 均保留。生产路径随后乘以 65535 进入 librtprocess 工作域。

### 5.2 Demosaic

- CFA：RGGB/BGGR/GRBG/GBRG；
- 默认 RCD；
- librtprocess 接口输入和输出为 FP32 0..65535 工作域；
- 当前生产路径跳过 demosaic 后的全帧 `/65535`，该 scale 折叠进后续矩阵。

### 5.3 Camera RGB 到 DWG

- `CameraNeutral -> xy` 迭代，最多 50 次，收敛阈值 `1e-10`；
- dual illuminant 按 reciprocal CCT 插值；
- 有 ForwardMatrix：使用 ForwardMatrix 路径；
- 无 ForwardMatrix：camera-to-XYZ-at-white 后做 Bradford 到 D50；
- XYZ D50 经 Bradford 到 D65，再使用固定矩阵转 DaVinci Wide Gamut；
- setup 和矩阵运算为 FP64，像素输出为 FP32；
- exposure 为 `exp2(exposure_offset_stops)`。

### 5.4 Capture sharpening

- 在线性 DWG 域；
- 使用目标线性 RGB 的 BT.2020 luma 权重形成中性 detail；
- 上下左右 4-neighbour detail；
- threshold 后将相同 delta 加到 R/G/B，避免主动引入 chroma；
- 默认 amount 0.4，threshold 0.002。

### 5.5 DaVinci Intermediate

- 解析 OETF 是真值；
- 生产 LUT 每 segment 65,536 entries；
- linear cut `0.00262409`；
- 默认 negative policy 为 `preserve_by_curve`，负数经过线性 toe；
- 非有限数必须报错，不能静默继续。

### 5.6 RGB 到 Y'CbCr 和量化

- matrix coefficients：BT.2020 non-constant luminance；
- `Kr=0.2627`, `Kb=0.0593`, `Kg=0.6780`；
- luma code：`64 + 876 * Y'`，clamp 64..940；
- chroma code：`512 + 896 * Cb/Cr`，clamp 64..960；
- quality chroma filter：`[-1, 4, 10, 4, -1] / 16`；
- 在偶数 x/left position 采样，边缘 clamp；
- deterministic noise 范围 `[-0.5, 0.5)`，按 frame/plane/sample hash；
- 正 code value 使用加 0.5 后截断完成 rounding；
- odd width 明确拒绝。

### 5.7 当前 metadata 事实

当前 CPU writer 写入：

```text
range       = MPEG/video
matrix      = BT.2020 NCL
primaries   = unspecified
transfer    = unspecified
profile     = ProRes 422 HQ
pixel format= yuv422p10le
```

算法采用 left-sited chroma，但 `AVCodecContext/AVFrame` 当前没有显式设置 `chroma_sample_location`。sidecar 要求 NLE 手动指定 DaVinci Wide Gamut / DaVinci Intermediate，而且 ADR 明确把 BT.2020 packing/chroma siting 标为等待 Resolve chart 验证的 provisional 行为。

GPU parity 阶段必须先复制这个 metadata 行为。若决定补写 `AVCHROMA_LOC_LEFT` 或增加 primaries/TRC 标记，应同时评估 CPU/GPU 两个后端并版本化，不能只改 GPU。

## 6. 当前 frame ownership

1. `McrawReader` 持有不可变索引、container metadata 和 decoder pool。
2. 每个异步 frame task 租用一个 `motioncam::Decoder`；lease 析构时归还 pool。
3. official decoder 的 vector move 到 `RawMosaicU16`。
4. calibration、demosaic 和 pack 各自分配并返回拥有型 vector；阶段结束后上游 buffer 自动析构。
5. `ProcessedFrame` 拥有 timestamp、metadata、color solution 和最终 YUV planes。
6. `write_video` 把三个 YUV vector move 到堆上，并用自定义 free callback 包进三个 `AVBufferRef`。
7. queued `AVFrame*` 在 `avcodec_send_frame` 后释放；FFmpeg 如需异步使用必须自行持有引用。
8. worker 取得 `AVPacket`，按 sequence 保存；mux 完成后释放 packet。
9. `FfmpegWriter` 析构按 RAII join workers、释放 queued frames/packets/codecs/muxer。

GPU 路径必须延续“拥有型 frame + RAII”的规则。裸 `VkImage` 不能独立于 `AVFrame`，slot 只有在 encoder 不再持有 frame reference 后才能回收。

## 7. 当前线程、队列和 backpressure

### 7.1 Frame compute

- CLI 使用 `std::async(std::launch::async)` 启动 frame task；
- `pending` future deque 受 `parallel_frames` 限制，默认最多 8；
- 每帧内部使用 OpenMP，默认总 CPU budget 16 时为 2 threads/frame；
- 按 future deque 前端有序消费，因此不会乱序提交，但存在 head-of-line wait；
- `McrawReader` decoder pool 支持并发 frame decode。

### 7.2 CPU encoding

- 默认最多 16 个独立 `prores_ks` contexts，每 context 1 slice thread；
- encode job queue 以 `contexts + 2` 限制 jobs in flight；
- packet 可乱序完成，但 `completed` map 只按 submission sequence mux；
- audio encode/mux 在调用线程同步执行；
- 所有 mux write 由 mutex 串行化。

### 7.3 对 GPU scheduler 的影响

现有 CPU encoder 的关键假设是“intra-only encoder 每次 send 立即返回至少一个 packet”。`prores_ks_vulkan` 暴露 `AV_CODEC_CAP_DELAY` 和 `async_depth`，不能复用这个假设，也不应为满足它而每帧 drain/wait。

GPU scheduler 需要独立的有界状态机：

```text
CPU read/decode queue
  -> GPU processing slots
  -> Vulkan encoder submission
  -> packet queue
  -> MOV mux
```

Phase 3 upload bridge 可先复用 CPU frame compute，但必须使用正确的 send/drain/flush loop。Phase 6 再将 queue/backpressure 扩展为完整异步 pipeline。

## 8. FFmpeg 接入位置

当前 FFmpeg 全部集中在 `src/output/ffmpeg_writer.cpp`，同时负责：

- ProRes codec discovery/configuration；
- video `AVFrame` 包装；
- 多 context CPU encode scheduling；
- packet ordering；
- PCM encode；
- MOV context/header/interleave/trailer；
- FFmpeg error translation；
- cleanup。

这是 GPU 接入前必须建立的 seam。建议保留 `FfmpegWriter` 作为 CPU-compatible façade，使现有 CLI 调用和 CPU tests 不变；内部逐步抽出：

```text
Ffmpeg RAII + errors
MovMuxer
CpuProResEncoder
VulkanProResEncoder
```

CPU encoder 的多 context ordering 逻辑留在 CPU adapter；Vulkan encoder 使用单 logical device、单 encoder context 起步，并通过 `async_depth` 实现内部并行。

## 9. Vulkan device owner 方案

### 9.1 决策

选择 **FFmpeg-owned Vulkan device**：

1. 用 `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VULKAN, selector, options)` 创建唯一 device context；
2. 从 `AVHWDeviceContext.hwctx` 取得 `AVVulkanDeviceContext`；
3. 应用的 compute pipelines 使用其中的 `inst`、`phys_dev`、`act_dev`、queue family 信息；
4. 所有 encoder input frames 来自该 device 的 `AVHWFramesContext`；
5. Vulkan/FFmpeg device context 由一个 RAII owner 控制，最后释放。

理由：

- 项目当前没有需要保留的自有 Vulkan runtime；
- 系统 smoke test 已证明 FFmpeg device creation 可用；
- 减少应用自建 device 再填充 `AVVulkanDeviceContext` 时遗漏 extension、queue 或 loader callback 的风险；
- 最终应用 shaders 与 encoder 仍在同一个 logical device 上，不产生跨 device copy。

### 9.2 Device selection

必须记录并允许选择：

- application-visible stable GPU id；
- Vulkan device UUID；
- vendor ID / device ID；
- device name；
- driver name/version；
- 实际 compute queue family。

配置可以接受 index 方便用户使用，但不能把枚举 index 当作永久身份。默认优先 discrete GPU，拒绝 software/basic renderer；force-GPU 模式失败即报错，auto 模式记录原因后回退 CPU。

## 10. 零拷贝 handoff 方案

### 10.1 Phase 3 bridge

```text
Yuv422P10 CPU vectors
  -> software AVFrame (AV_PIX_FMT_YUV422P10LE)
  -> av_hwframe_get_buffer(Vulkan frame)
  -> av_hwframe_transfer_data
  -> AV_PIX_FMT_VULKAN
  -> prores_ks_vulkan
```

这只验证 encoder、format、timestamp、packet、mux 和 cleanup，不作为性能结论。

### 10.2 最终 GPU-resident handoff

```text
av_hwframe_get_buffer()
  -> AVFrame(format=AV_PIX_FMT_VULKAN,
             hw_frames_ctx.sw_format=AV_PIX_FMT_YUV422P10LE)
  -> AVVkFrame::img[]
  -> application compute writes Y/Cb/Cr images
  -> update layout/access/queue ownership
  -> wait+signal AVVkFrame timeline semaphores
  -> avcodec_send_frame()
```

FFmpeg 8.1.2 的 `AVVkFrame` 已提供每 image 的：

- `img[]` / `mem[]`；
- `layout[]` / `access[]`；
- `queue_family[]`；
- timeline `sem[]` / `sem_value[]`。

其合约要求每次提交等待当前 `sem_value`，并以递增值 signal。应用不能绕过这些字段或用每帧 `vkQueueWaitIdle()` 掩盖错误。

优先从 FFmpeg frames pool 分配，不在第一版实现 application-owned image import。运行时必须查询 RTX 3060 对 `AV_PIX_FMT_YUV422P10LE` 对应 optimal-tiling image/fallback plane formats的 storage/transfer 支持，不能硬编码 VkFormat 数量或 plane layout。

## 11. 实测 bottleneck 与 GPU 优先级

当前 8 帧、16 CPU threads、8 frames in flight、2 threads/frame 的 compute-only 快照：

| 阶段 | mean ms/frame |
|---|---:|
| official RAW decode + metadata | 61.05 |
| black/white calibration | 52.91 |
| demosaic | 411.14 |
| color solution | 0.04 |
| fused DWG/DI/sharpen/YUV | 565.17 |
| wall throughput | 6.64 fps |

该短测仅用于定位，不替代正式 benchmark。它说明 GPU 工作优先级应为：

1. 融合颜色/DI/锐化/RGB-to-YUV；
2. demosaic；
3. calibration/RAW normalization；
4. RAW compression 6/7 decode；
5. 只按 profiler 结果继续融合或调整 queue。

历史完整 CPU output 约 2.52 fps，低于 compute-only，说明 CPU `prores_ks` 和 mux/write 也是重要端到端瓶颈。Phase 3 必须分别报告 upload、GPU encode 和 write 时间。

## 12. 已识别风险

### High

1. **项目 FFmpeg Vulkan 未启用。** 版本号正确但 build feature 错误；必须以项目链接库的 `avcodec_find_encoder_by_name` 测试为准。
2. **CPU writer 的 packet-immediate 假设不适用于 Vulkan encoder。** GPU 后端需要独立 send/drain/flush state machine。
3. **颜色 metadata 尚未完全产品化。** primaries/TRC unspecified、chroma location 未写入且 ADR 标 provisional；GPU 先严格 parity，任何修正必须 CPU/GPU 一起版本化。
4. **同步和生命周期。** `AVVkFrame` timeline semaphore、layout、access、queue family 任一处理错误都可能产生随机旧帧/花帧，不能用 global idle 掩盖。

### Medium

5. **当前 FFmpeg writer 同时承担 encoder 和 mux。** 需要窄范围抽象，但重构必须以 CPU bitstream/decoded-frame/metadata regression 保护。
6. **当前 telemetry 不测真实 encode time。** `prores_submit_wait` 只含提交和 backpressure；GPU 前必须补足 encode/mux/queue/VRAM/PCIe 指标。
7. **当前无 cancellation/device-lost 状态机。** CLI 异常能留下 `.partial.mov`，但 GPU runtime failure 需要显式停止队列和标记原因。
8. **参考大文件未保留在仓库。** 需要小型 manifest 和关键帧 artifacts，避免只依赖文档中的历史数字。
9. **多帧 4K 内存压力。** upload bridge 会临时同时保留 CPU YUV 与 Vulkan frame，pool/queue 必须有界。

### Low / operational

10. Vulkan loader 可见 OBS、Bandicam、Steam 等 implicit layers；validation/performance run 应禁用 overlays，并记录 active layers。
11. 当前 `CMake` 命令来自 Nuitka cache，虽然版本满足要求，但正式可复现脚本应解析固定工具链或 VS/CMake 安装，不依赖偶然 PATH。

## 13. Phase 1～3 文件级修改计划

### 13.0 前置：backend seam 和可复现依赖

目标：不改变 CPU 对外行为，建立 CPU/GPU 并存入口。

修改：

- `vcpkg.json`
  - FFmpeg features 增加 `vulkan`；
  - 显式加入项目直接使用的 Vulkan headers/loader dependency；
  - 保持当前 builtin baseline，不追踪浮动 master。
- `CMakeLists.txt`
  - 增加 `MCRAW_ENABLE_VULKAN`，默认开发构建可开、能力缺失时可明确关闭；
  - 分离 FFmpeg common、CPU encoder、Vulkan targets；
  - Debug 才启用 validation/debug utils，Release 不依赖 SDK layers。
- `include/mcraw/core/config.hpp`, `src/core/config.cpp`, `config/schema-v1.json`, `config/default.json`
  - 增加 `backend = auto|cpu|vulkan`；
  - 增加 GPU selector、`async_depth`、fallback、precision；
  - CPU 默认和既有 config 语义保持不变。
- `include/mcraw/output/video_encoder.hpp`（新增）
  - `IVideoEncoder` 的 send/drain/flush/capability contract；
  - 不假定一帧对应即时 packet。
- `include/mcraw/output/video_frame.hpp`（新增）
  - 明确 CPU YUV frame 与 Vulkan frame 的 tagged variant；
  - 公共 metadata 含 width/height/PTS/time base/range/matrix/primaries/TRC/chroma location。
- `src/output/ffmpeg_raii.*`（新增）
  - AVFrame/AVPacket/AVBufferRef/codec/context RAII 和统一 error string。
- `src/output/cpu_prores_encoder.*`（新增）
  - 从现有 writer 抽出 CPU encoder adapter；
  - 保留多 context/sequence 行为。
- `src/output/mov_muxer.*`（新增）
  - packet/timebase/interleave/trailer/PCM；
  - 只接受 compressed packet，不依赖 frame storage。
- `src/output/ffmpeg_writer.cpp`
  - 保留 façade 和既有构造/调用语义；改为组合 CPU encoder + muxer；
  - 每一步用现有 CPU E2E output regression 验证。
- `src/cli/main.cpp`
  - runtime capability report 和 backend selection；
  - auto fallback 与 force-GPU 错误语义；
  - `list-capabilities` 打印不可用原因。
- `tests/test_backend_selection.cpp`, `tests/test_ffmpeg_cpu_regression.cpp`（新增）
  - auto/force/fallback；
  - CPU frame count、PTS、profile、metadata、decoded checksum 不回归。

验收：当前 18 tests 加新 tests 全过；CPU sample 输出行为不变；禁用 Vulkan 时仍能独立构建/运行 CPU backend。

### 13.1 Phase 1：Vulkan runtime/capability

新增：

- `include/mcraw/vulkan/vulkan_capabilities.hpp`
- `include/mcraw/vulkan/vulkan_runtime.hpp`
- `src/vulkan/vulkan_capabilities.cpp`
- `src/vulkan/vulkan_runtime.cpp`
- `src/vulkan/vulkan_debug.cpp`
- `src/vulkan/vulkan_telemetry.cpp`
- `tests/test_vulkan_runtime.cpp`

职责：

- 枚举 physical devices、UUID、PCI/vendor/device ID、driver；
- 稳定 device selection；
- 创建/销毁 FFmpeg-owned Vulkan device context；
- 验证 compute queue；
- 安装 debug messenger/object names；
- 结构化记录 active layers/extensions；
- 连续 init/destroy、错误注入、无设备 fallback。

验收：

- RTX 3060 被稳定选择；
- 1000 次 init/destroy 无 validation error/resource growth；
- 指定不存在 GPU 时 auto 回退、force 模式明确失败；
- CPU tests 不受影响。

### 13.2 Phase 2：FFmpeg Vulkan device/frames context

新增：

- `include/mcraw/output/ffmpeg_vulkan_context.hpp`
- `src/output/ffmpeg_vulkan_context.cpp`
- `tests/test_ffmpeg_vulkan_context.cpp`
- `tests/test_ffmpeg_vulkan_capability.cpp`

职责：

- 用项目链接库检查 `prores_ks_vulkan`；
- 打印/固定当前 private options mapping；
- 查询 `av_hwdevice_get_hwframe_constraints`；
- 创建 `AVHWFramesContext`：
  - `format = AV_PIX_FMT_VULKAN`；
  - 首选 `sw_format = AV_PIX_FMT_YUV422P10LE`；
  - width/height 使用样本和小型测试尺寸；
  - pool size 显式有界；
- 分配 AVVkFrame，记录实际 VkFormat/image count/usage/layout；
- 完成单帧 software -> Vulkan -> software round-trip，仅用于桥接验证。

验收：

- 项目库的 capability probe 通过；
- RTX 3060 支持所需 sw format 和 storage/transfer usage；
- allocation/transfer/free validation clean；
- unsupported format 返回可诊断原因并回退 CPU。

### 13.3 Phase 3：CPU upload bridge + Vulkan ProRes

新增：

- `include/mcraw/output/vulkan_prores_encoder.hpp`
- `src/output/vulkan_prores_encoder.cpp`
- `src/output/vulkan_output_backend.cpp`
- `tests/test_vulkan_prores_smoke.cpp`
- `tests/test_vulkan_prores_e2e.cpp`

修改：

- `src/cli/main.cpp`
  - 接入 GPU backend selection；
  - 输出 backend、GPU、driver、FFmpeg/libav versions、async depth、fallback reason。
- `src/output/sidecar.cpp`
  - 增加 pipeline/backend/FFmpeg/Vulkan/device/transfer counters；
- `scripts/validate-samples.ps1`
  - 增加 CPU 与 upload-bridge GPU 输出的 ffprobe、frame count、PTS、decode 和 failure cleanup 检查。

编码器规则：

- 一个 `prores_ks_vulkan` context 起步；
- profile HQ，alpha disabled；
- `async_depth` 从 1 验证，再测试 2/4/8；
- 正确处理 send `EAGAIN`、receive loop、delayed packets 和 null-frame flush；
- mux 只接收 packet；
- partial 文件只有 flush、trailer、close、基础 ffprobe 验证成功后才能 rename；
- device failure 不允许在同一个 MOV 中切换 CPU。

验收：

- 10 秒样本完整输出；
- frame count、PTS/duration、profile、audio sync、metadata 与 CPU reference 一致；
- FFmpeg/ffprobe/Resolve 可读；
- validation 0 error；
- 循环 100 次无资源增长；
- telemetry 明确标记 `gpu_resident=false`, `upload_frames=N`, `readback_frames=0`；
- 不把该阶段速度作为最终 GPU pipeline 结论。

## 14. 后续阶段的边界

Phase 4 才开始把 color/DI/RGB-to-YUV 移到 Vulkan；Phase 5 才允许报告 `gpu_resident=true`。该 invariant 必须进入代码和测试：

```text
gpu_resident == true
=> upload_frames == 0
=> readback_frames == 0
```

Phase 4 shader golden tests 应先逐 pass 对 CPU FP64/解析参考，再决定是否融合。第一版使用 FP32 precise mode；FP16 必须作为单独 fast mode，在最大误差、RMSE、percentile、异常坐标和最终 10-bit LSB 阈值通过后才能启用。

## 15. 审计批准门槛

开始实现前建议确认以下设计决策：

1. CPU pipeline 继续作为默认和 fallback，公开行为冻结；
2. FFmpeg 拥有唯一 Vulkan device，应用借用同一 device；
3. Phase 3 明确是 upload bridge，不宣称最终加速；
4. GPU parity 先保持当前 unspecified primaries/TRC 和现有 chroma metadata 行为；
5. 首个 GPU processing 目标按实测热点选择 fused color/DI/YUV，之后再迁移 RCD demosaic；
6. 任何输出 metadata 改进作为 CPU/GPU 共同行为的版本化变更处理。

批准后，实施从 13.0 的 backend seam 与 vcpkg Vulkan build 开始，不直接跳到 shader 或零拷贝。
