# GPU Pipeline 构建总结与性能瓶颈分析

日期：2026-07-14
性质：分析总结，不含代码变更。供后续 agent 参考的现状快照。
数据来源：`GPU_PIPELINE_AUDIT.md`、`GPU_PHASE1~8` 各验证报告、`implementation-status.md`。
人工验收：导出文件已由用户手工比对，与 CPU 版本无可见误差（与文档记录的 ≤1 LSB / PSNR ~88 dB 一致）。

## 1. 构建概况（Phase 0–8）

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0–2 | backend seam（`IVideoEncoder` 抽象、CPU encoder/muxer 拆分）；vcpkg 重建启用 Vulkan 的 FFmpeg 8.1.2；FFmpeg-owned 单一 Vulkan device，应用借用同一 device | 完成 |
| Phase 3 | CPU YUV 上传桥 + `prores_ks_vulkan`，验证编码/封装/清理 | 完成 |
| Phase 4 | 应用自有 compute shader：RGB→BT.2020 NCL YCbCr 4:2:2 10-bit 打包；golden test 对 CPU 参考 ≤1 LSB，dither 逐位一致；FP16 显式禁用 | 完成 |
| Phase 5–6 | GPU-resident 直通：shader 直写 `AVVkFrame`，timeline semaphore 同步；有界异步流水线（RGB job 队列 → GPU slots → 编码 → packet 队列 → 独立 mux 线程），无 normal-path `vkQueueWaitIdle`，带取消/背压/遥测 | 完成 |
| Phase 7 | 按实测优化 CPU 侧 TargetLog RGB 生产路径：1357 → 547 ms/帧（-59.7%），端到端 2.306 → 3.618 fps | 完成 |
| Phase 8 | 产品化：`backend=auto` 全量 preflight 后可回退 CPU；`backend=vulkan` 失败即终止、不在同一 MOV 内切换编码器；`device_lost` 错误类别 | 完成（发布 gate 未全关） |

**实测结果（240 帧 4096×3072 真实样本，RTX 3060）**：

- CPU 端到端：2.52 fps → GPU 路径：**6.95 fps（约 2.76×）**；
- `gpu_resident=true`，`direct_frames=240`，`upload_frames=0`，`readback_frames=0`；
- FFmpeg 全流解码通过，音画时长与 CPU 输出一致。

## 2. 瓶颈定位：GPU 在挨饿，瓶颈在 CPU

Phase 6/8 遥测：GPU slot 队列峰值 6/10，packet 队列峰值 1/16，**背压等待为 0**。Vulkan shader 和编码器从未成为限制因素，一直在等 CPU 喂数据。

目前真正搬上 GPU 的只有最后两步（RGB→YUV 打包 + ProRes 编码）。每帧主要开销仍全部在 CPU：

| 阶段（每帧均值） | 位置 | 耗时 |
|---|---|---:|
| MCRAW RAW 解压 + 元数据 | CPU | ~61 ms |
| 黑白场校准 | CPU | ~53 ms |
| RCD demosaic（librtprocess） | CPU | ~411–475 ms |
| Camera→DWG + 曝光 + 锐化 + DI 编码（TargetLog RGB） | CPU | ~547 ms（Phase 7 优化后） |
| RGB→YUV 打包 + ProRes 编码 | **GPU** | 非瓶颈 |

2.52 → 6.95 fps 的提升主要来自甩掉 CPU `prores_ks` 编码和 YUV 打包；demosaic 与 TargetLog 两大热点（合计约 1 秒/帧）仍在 CPU 上，因此端到端提升幅度受限。

### 精度一致性约束（≤1 LSB parity）是否是主因？

不是主因，但有真实代价：

1. 全程 FP32、FP16 因无误差预算被显式禁止（Phase 4）；
2. 必须逐位复现 CPU 的确定性 dither hash 和 65,536 项/段的 DI LUT；
3. 为保 parity，切分点选在"CPU 算到 TargetLog RGB、GPU 只做打包"——**这个保守切分本身才是最大的性能约束**，而不是精度本身。

## 3. 若可放弃部分精度，收益排序

1. **对 GPU 版 demosaic / 色彩链放宽验收标准**（收益最大）：不要求与 librtprocess RCD 逐像素一致，改用容差型验收（最大误差 / RMSE / percentile / 最终 10-bit LSB 阈值）。审计第 14 节已预留 FP32 precise + FP16 fast 双轨路线。
2. **FP16 / 混合精度**：矩阵乘、锐化、chroma 滤波用 FP16 减半带宽与寄存器压力；DI 为 log 曲线、暗部敏感，累加建议保 FP32。
3. **DI 曲线在 shader 内解析求值**替代巨型 LUT：GPU 超越函数便宜，可能更快且不损失精度（此项实际无精度代价）。
4. **放弃 dither 的逐位复现**：任意高质量 GPU 噪声即可，视觉无差别。
5. chroma 5-tap quality 滤波降级为 fast：收益极小，不建议。

注意：这些让步的价值在于**降低把重活搬上 GPU 的实现/验证门槛**，而不是让现有 CPU 代码变快。

## 4. 完全 GPU 化 / zero-copy 现状与改进路径

### 现状

- **GPU→编码器一侧 zero-copy 已实现**：shader 直写 `AVVkFrame`，timeline semaphore 同步，无 YUV 上传/回读。
- **CPU→GPU 一侧远未实现**：每帧需上传 3 个 FP32 平面 **约 151 MB** 的 TargetLog RGB（8 帧即 1.2 GB，见 Phase 6 遥测 `rgb_upload_bytes=1,207,959,552`）。
- `gpu_resident=true` 的严格含义仅是"无未压缩 YUV 上传/回读"，并非全流程 GPU 常驻。

### 改进路径（与审计第 11 节优先级一致）

1. **TargetLog 色彩链上 GPU**：Camera→DWG 矩阵、曝光、锐化、DI 编码移为 Vulkan compute pass，与现有打包 shader 融合或串联；矩阵/白点仍由 CPU FP64 计算后作 uniform 上传。直接消掉 ~547 ms/帧的最大热点。
2. **GPU RCD demosaic（+ 合并 calibration/unpack）**：完成后上传物从 151 MB/帧 FP32 RGB 降为 **25 MB/帧 U16 CFA（约 6×）**。此后 CPU 只剩 RAW 解压（~61 ms/帧，理论上限 ~16 fps，可流水掩盖）和 mux。
3. **（可选）MCRAW compression 6/7 GPU 解压**：上传量再降至 ~11 MB/帧压缩 payload；实现成本高，按 profiler 结果决定，多线程 CPU 解码可能已够用。
4. **配套**：中间结果全程留在 VRAM image 间流转，不回 host；staging 上传用 dedicated transfer queue 与 compute 重叠。

**预期量级**：完成第 1、2 步后，RTX 3060 上 4K ProRes HQ 达到实时（24–30 fps）是现实目标（Phase 8 已明确当前吞吐低于实时且剩余阶段即上述 CPU 环节）。

## 5. 遗留发布门槛（改动 shader 前应先关注）

来自 `GPU_PHASE8_PRODUCTION.md` / `GPU_PHASE3_VALIDATION.md`：

- pinned FFmpeg ProRes DCT shader 的 GPU-assisted validation race 未解决或未正式豁免；
- DaVinci Resolve / Premiere / 桌面播放器兼容性、chroma siting 人工验收未完成；
- AMD / Intel 硬件与第二代 NVIDIA 驱动覆盖缺失；
- 一小时真实转换 / 批量资源增长测试未跑（现有 30,000 帧合成测试仅覆盖 1,000 逻辑秒）；
- 项目级性能阈值尚未议定。

在这些 gate 关闭前，Vulkan 后端保持 opt-in，CPU 后端为生产默认与回退。
