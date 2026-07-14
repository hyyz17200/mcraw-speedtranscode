# v0.1 Implementation Status

## 已实现源码边界

- CMake/vcpkg/MSVC 2022 构建骨架
- MCRAW 独立索引和压缩 payload 读取
- MotionCam 官方 CPU 解压适配
- 元数据标准化和来源可见 warning
- RAW 黑白场、RCD/AMaZE/IGV；独立 FCS 已按性能/收益决策移除
- 双矩阵、ForwardMatrix、Bradford、DWG、DaVinci Intermediate
- 每转换实例 DI LUT、融合 Camera→DWG→DI→YCbCr、quality 4:2:2、dither、legal range 量化
- OpenMP 行并行、CPU/RAM 感知的有界多帧并行和有序 mux
- FFmpeg ProRes/MOV/PCM/时间戳和 sidecar
- 七个 CLI 子命令和单元测试

## Windows 10 / MSVC 2022 验收结果

- MSVC 2022 Release：主库、CLI、测试程序构建通过
- 单元测试：16/16 通过
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

## 仍需人工外部验收

- DaVinci Resolve chart、chroma siting 与手动 Input Color Space 工作流实测
- 完整 240 帧输出的人工播放/画面检查（自动解码和流参数检查已通过）
