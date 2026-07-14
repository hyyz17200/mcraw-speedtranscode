# v0.1 Implementation Status

## 已实现源码边界

- CMake/vcpkg/MSVC 2022 构建骨架
- MCRAW 独立索引和压缩 payload 读取
- MotionCam 官方 CPU 解压适配
- 元数据标准化和来源可见 warning
- RAW 黑白场、RCD/AMaZE/IGV/DCB/LMMSE；RCD 保持默认
- 可选的线性 DWG Capture Sharpening，以及 NoiseProfile 驱动、仅修改 Bayer
  红/蓝色差残差的空间 RAW 色度降噪；两者默认关闭
- 双矩阵、ForwardMatrix、Bradford、DWG、DaVinci Intermediate
- 每转换实例 DI LUT、融合 Camera→DWG→DI→YCbCr、quality 4:2:2、dither、legal range 量化
- OpenMP 行并行、CPU/RAM 感知的有界多帧并行和有序 mux
- FFmpeg ProRes/MOV/PCM/时间戳和 sidecar
- 七个 CLI 子命令和单元测试

## Windows 10 / MSVC 2022 验收结果

- MSVC 2022 Release：主库、CLI、测试程序构建通过
- 单元测试：21/21 通过
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
下降约 11.1%。单独打开 RAW 色度降噪 `1.0`，测得 3.799 fps，较同轮 RCD
下降约 14.6%；新增降噪阶段平均 335.4 ms/帧。多帧并行下各阶段计时有重叠，
吞吐差是评估总体开销的主要指标。

完整 240 帧对比输出均通过 FFmpeg 全流解码：AMaZE 为 2.217 fps，RCD +
Capture Sharpening `0.25` 为 2.655 fps。两者均为 4096×3072 ProRes 422 HQ
`yuv422p10le`，保留 48 kHz 双声道 PCM。

## 仍需人工外部验收

- DaVinci Resolve chart、chroma siting 与手动 Input Color Space 工作流实测
- 完整 240 帧输出的人工播放/画面检查（自动解码和流参数检查已通过）
