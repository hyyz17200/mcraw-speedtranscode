# GPU Pipeline 下一阶段正式行动指南

日期：2026-07-14；最新修订：2026-07-15
性质：实施与验收总纲  
适用范围：Stage 0～3 / Batch A～D 已完成、GPU pipeline 已前移到 U16 RAW mosaic 入口后，
清理确认的用户可见开销，验证 Vulkan ProRes async/queue serialization 候选，并在 U16 RAW
入口冻结 GPU pipeline 边界；MCRAW compressed payload 保持 CPU 解码。

本文综合以下现状文档，并把已有分析转换为可执行的阶段、交付物、验收门槛和停止条件：

- `GPU_PIPELINE_SUMMARY_AND_NEXT_STEPS.md`
- `GPU_PHASE1_VALIDATION.md` ～ `GPU_PHASE8_PRODUCTION.md`
- `GPU_PIPELINE_AUDIT.md`
- `VULKAN_PRORES_GPU_PIPELINE_GUIDE.md`
- `GPU_STAGE3F_E2E_BENCHMARK.md`
- `PRORES_KS_VULKAN_ENCODER_BENCHMARK_2026-07-15.md`
- `GPU_PIPELINE_SERIALIZATION_ANALYSIS_2026-07-15.md`
- `test-output/full-file-parallel-benchmark/benchmark-report.json`
- `implementation-status.md`

本文不改变现有 CPU reference、Vulkan opt-in 策略或发布门槛。后续实现如与本文冲突，
应先更新本文或记录 ADR，不能在实现中静默改变目标。

---

## 1. 当前基线与正式结论

### 1.1 已经完成的能力

当前 Vulkan 路径已经完成：

```text
CPU official RAW decode
  -> U16 RAW mosaic upload
  -> Vulkan calibration / RCD demosaic
  -> Vulkan Camera RGB / DWG / sharpening / DI
  -> Vulkan RGB-to-YUV 4:2:2 10-bit
  -> FFmpeg-owned AVVkFrame
  -> prores_ks_vulkan
  -> compressed packet
  -> CPU MOV/audio mux
```

已经验证的工程基础包括：

- FFmpeg-owned 单一 Vulkan logical device；
- 应用 compute 和 `prores_ks_vulkan` 共用该 device；
- shader 直接写 FFmpeg Vulkan frame pool；
- timeline semaphore 和正确的 `AVVkFrame` ownership；
- 有界 frame job queue、GPU slots、packet queue 和独立 mux worker；
- normal path 无 `vkQueueWaitIdle()` / `vkDeviceWaitIdle()`；
- backpressure、取消、异常传播、device lost 和 partial 文件清理；
- `auto` 全尺寸 preflight/fallback 与强制 Vulkan 失败语义；
- 240 帧 4096×3072 ProRes HQ + PCM 完整输出和软件全流解码。

### 1.2 当前性能事实

在 2026-07-15 的 RTX 3060 / NVIDIA 610.62、4096×3072 ProRes 422 HQ 基线上：

| 项目 | 结果 |
|---|---:|
| 240 帧 precise 完整转换 | 34.776 fps |
| 240 帧 fast 完整转换 | 36.857 fps |
| GPU processing-only microbenchmark | 96.052 fps / 10.41 ms/frame |
| GPU processing + Vulkan ProRes microbenchmark | 35.880 fps / 27.87 ms/frame |
| encoder-only，1 个进程 | 45.16 fps / 22.14 ms/frame |
| encoder-only，8 个进程 aggregate | 56.61 fps / 17.66 ms/frame |
| 完整应用，1/2/4 个并行进程 aggregate | 32.75 / 30.02 / 28.63 fps |

processing-only 的 10.41 ms 与最佳 encoder-only aggregate 的 17.66 ms 相加，预测
35.62 fps；实际 combined microbenchmark 为 35.88 fps。两个 benchmark 的输入内容和
upload 路径并非完全相同，因此该等式不能证明内部没有任何 bubble，但它明确说明：**现有
证据没有显示额外的大幅 steady-state pipeline 损耗。当前主要成本是 image processing 与
ProRes encoding 在同一 GPU 上执行的有效工作，而不是重复未压缩数据复制、mux、queue lock
或 CPU decode starvation。**

`encoder_send` 的约 26.88 ms 是线程阻塞位置，不是纯 encoder shader 时间。它可能同时包含
输入帧 semaphore 尚未完成的 processing、ProRes GPU execution、packet transfer 和 FFmpeg
内部 packet production。禁止把该 wall timer 直接解释为独立 encoder cost，也禁止用
`nvidia-smi` 的设备级 utilization 乘 wall time 推导所谓 GPU-busy shader time。

### 1.3 zero-copy 的准确口径

当前已经实现的是：

```text
CPU U16 RAW -> 一次 U16 upload -> Vulkan 全图像链
  -> FFmpeg AVVkFrame -> Vulkan ProRes -> compressed packet download
```

生产路径没有 FP16/FP32 RGB upload、未压缩 YUV upload 或 pixel/YUV readback，因此 telemetry 中
`gpu_resident=true`, `direct_frames=N`, `upload_frames=0`, `readback_frames=0`
在既定定义下成立。

这不是字面上的零传输：每帧仍有一次约 25.2 MB 的 U16 RAW host-to-device upload，编码后
compressed bitstream 仍必须回到 CPU mux。这两次边界传输是当前架构的必要成本；在没有
matched profiler 证据前，不把它们归类为重复 copy。正式口径为：

> 每帧只向离散 GPU 上传一次尽可能小的源数据；所有大尺寸未压缩中间图像留在 VRAM；
> 编码后只让压缩 packet 回到 CPU mux。

---

## 2. 下一阶段的正式目标

### 2.1 主目标

已经按以下顺序完成 Vulkan pipeline 入口前移：

```text
TargetLog FP32 RGB（历史入口）
  -> Camera RGB FP32
  -> U16 RAW mosaic（当前及最终 GPU pipeline 入口）
```

项目不再以 GPU MCRAW decoder 为目标。compression 6/7 解码继续在 CPU 完成，Batch E
负责把 official truth、调度、内存和必要的 CPU 快速路径做完整；后续 profiler 结果不再自动
触发 GPU decoder 工作，若未来重新提出该方向，必须另立总纲或 ADR。

### 2.2 性能目标

项目已经确认一个最低目标和一个扩展目标：

- 最低产品目标：4096×3072 ProRes HQ 稳定达到源素材实时帧率；
- 首个明确基准：≥24 fps；
- 扩展目标：≥30 fps；
- 不以短帧 smoke 的瞬时 FPS 代替完整样本结果；
- 每阶段至少同时报告 end-to-end、各 producer stage、GPU timestamp、CPU/GPU
  utilization、PCIe upload、VRAM peak 和磁盘写入。

24/30 fps 是行动目标，不是对 RTX 3060 的预先性能承诺。若硬件 compute、显存带宽或
`prores_ks_vulkan` 在完整 GPU producer 下成为新瓶颈，应以 profiler 更新目标或方案。

### 2.3 不变约束

- CPU pipeline 保持 reference、默认和 fallback，禁止为 GPU 优化重写其稳定语义；
- 同一 MOV 中禁止运行时从 Vulkan 切换 CPU encoder；
- 只有 drain、trailer、close 和轻量 reopen/metadata validation 成功后才发布最终文件；完整
  packet-by-packet 扫描保留为显式 verify/release/test gate，不再默认计入正常 conversion E2E；
- 精度、demosaic 算法、metadata 或 chroma siting 的行为变化必须显式版本化；
- 每个新 GPU stage 必须可独立 golden test、性能测试和故障注入；
- 未完成发布 gates 前，Vulkan 保持 opt-in。

---

## 3. 精度策略：Precise 与 Fast 分轨

性能优化和语义验证不能混成一个不可分辨的 backend。建议正式建立两条模式：

### 3.1 `fp32/precise`

用途：GPU production reference、回归基线和逐阶段迁移。

- shader 图像和关键计算使用 FP32；
- 每帧颜色解算继续由 CPU FP64 完成，只上传最终矩阵/参数；
- 保持现有 negative policy、sharpening、chroma filter、rounding、clamp 和 dither；
- 最终量化前/量化后按阶段比较；
- 最终 Y/Cb/Cr 与 CPU reference 最大差异保持 ≤1 LSB；
- 非有限数和非法 metadata 继续明确报错。

### 3.2 `mixed/fast`（新增前必须先完成验证设计）

用途：在 precise pipeline 正确后，以显式画质预算换吞吐和带宽。

建议候选策略：

- RGB/intermediate image 使用 FP16；
- 矩阵参数、关键累加、DI 暗部和最终 YUV quantization 保持 FP32；
- 允许 FMA、不同运算顺序、非 bit-exact dither；
- DI 可评估 texture LUT + interpolation 或 shader analytic evaluation；
- 不能让 fast mode 静默替代 precise mode。

建议以以下数值作为**初始实验预算**，在固定 corpus 测量后再冻结正式阈值：

| 指标 | Precise | Fast 候选 |
|---|---:|---:|
| 最终 10-bit max error | ≤1 LSB | ≤8 LSB |
| 最终 10-bit P99 error | ≤1 LSB | ≤2 LSB |
| 最终 10-bit RMSE | 记录 | ≤1.0 LSB |

这些阈值不能替代图像检查。必须额外覆盖暗部、super-white、负值 toe、饱和颜色、细线、
斜边、摩尔纹和高频 chroma。若 clipping 边界导致少量离群点，应记录坐标和原因，不能
只扩大 max-error 阈值掩盖问题。

### 3.3 demosaic 单独验收

demosaic 不能只通过最终 ProRes PSNR 验收。必须同时比较：

- 线性 Camera RGB；
- CFA 边界和四种 Bayer pattern；
- 黑场、白场、坏点邻域和图像边缘；
- 高频斜线、彩色摩尔纹和 zipper artifact；
- max/RMSE/P50/P95/P99 及异常像素坐标；
- 最终 DWG/DI/YUV 和解码 ProRes 的影响。

如果 GPU RCD 为近似实现，必须以新名称/模式暴露，不能声称与 librtprocess RCD 相同。

---

## 4. 分阶段实施路线

## Stage 0：冻结可复现基线和 profiler 合约

### 目标

在改变 pipeline 切分点前，建立所有后续阶段共同使用的事实基线。

### 工作项

1. 固定当前 commit、FFmpeg build、驱动、配置和样本 hash。
2. 建立至少包含以下内容的固定 corpus：
   - 当前 4096×3072、240 帧真实样本；
   - 首帧、末帧和固定中间帧；
   - 四种 CFA pattern 的小型 synthetic/golden；
   - 暗部、super-white、负值、饱和色和高频边缘图案。
3. 保存逐阶段 hash/statistics：RAW、calibrated mosaic、Camera RGB、TargetLinear、
   TargetLog、YUV planes、packet/frame count、PTS 和 ffprobe JSON。
4. 为 Vulkan stages 加 GPU timestamp query；CPU wall timer 不能冒充 GPU execution time。
5. telemetry 明确区分：
   - compressed input read bytes；
   - U16 RAW upload；
   - FP16/FP32 RGB upload；
   - GPU image-to-image bytes 不宣称为 PCIe bytes；
   - compressed packet download/mux bytes。
6. 记录完整转换的 CPU%、GPU compute utilization、VRAM peak、PCIe、disk throughput。

### 验收门槛

- 同一构建重复运行的 frame count、PTS、RAW/YUV golden 和 telemetry invariant 稳定；
- benchmark 至少一次 warm-up、三次正式运行，报告 median 和 spread；
- validation layer 普通验证无应用错误；
- 现有 Phase 8 输出行为不回归。

### 停止条件

如果基线重复波动足以掩盖预期优化幅度，先修正 benchmark 或 telemetry，不进入 Stage 1。

---

## Stage 1：Camera RGB 之后全 GPU

### 目标

消除当前约 511 ms/frame 的 CPU TargetLog producer，使上传切分点从 TargetLog RGB 前移到
Camera RGB。

### 目标数据流

```text
CPU decode/calibration/RCD
  -> Camera RGB FP32 staging
  -> Vulkan camera matrix + exposure
  -> Vulkan neutral capture sharpening
  -> Vulkan DaVinci Intermediate
  -> Vulkan RGB-to-YUV 4:2:2 10-bit
  -> existing AVVkFrame / prores_ks_vulkan
```

### 实现原则

- CPU 保留 CameraNeutral、dual illuminant、ForwardMatrix、Bradford 和 DWG matrix 的 FP64
  setup；只把最终 matrix、exposure 和 policy 参数作为 uniform/push data 上传；
- 第一版每个逻辑 stage 独立 golden test；正确后再由 profiler 决定 pass fusion；
- sharpening 使用 tile/shared memory 时必须保持邻域语义和边界规则；
- DI 首版以 FP32 precise 为基线，不在同一提交中同时引入 FP16；
- 直接复用现有 FFmpeg frames、timeline semaphore、slot 和 encoder handoff。

### 交付物

- Vulkan color/exposure pass；
- Vulkan sharpening pass；
- Vulkan DI pass；
- Camera RGB staging writer；
- stage-level golden tests 和完整 E2E regression；
- 新旧路径 matched benchmark 与 GPU timestamp breakdown；
- sidecar 增加实际 pipeline entry 和 precision 标记。

### 验收门槛

- precise 模式最终 YUV ≤1 LSB；
- 所有现有音频、PTS、MOV cleanup、fallback 和 device-lost tests 继续通过；
- 不增加 CPU readback；
- GPU queue 应比当前更持续获得工作；
- 完整样本端到端性能必须有可重复改善。建议 go/no-go 下限为 ≥20%，否则先 profile，
  不继续盲目融合 shader。

### 阶段决策

- 若颜色链成为 GPU 热点：先分析 occupancy、带宽、pass 间 layout 和 fusion；
- 若 demosaic 明确成为绝对主瓶颈：冻结 Stage 1 正确版本，进入 Stage 2；
- 若 PCIe Camera RGB upload 成为热点：Stage 2 优先级进一步提高。

---

## Stage 2：U16 RAW upload + GPU calibration + GPU demosaic

### 目标

把每帧上传量从约 151 MB FP32 RGB 降为约 25.2 MB U16 CFA，并消除 CPU calibration 和
RCD demosaic 热点。

### 目标数据流

```text
CPU official MCRAW decode
  -> U16 RAW mosaic staging upload
  -> Vulkan black/white calibration
  -> Vulkan RCD demosaic
  -> Stage 1 GPU color/sharpen/DI/YUV
  -> Vulkan ProRes
```

### 子阶段

1. **2A：U16 upload 与 calibration shader**
   - 四 CFA position 独立 black/white；
   - 不提前 clamp negative/super-white；
   - 先输出可读回测试 image，仅在测试路径 readback。
2. **2B：GPU RCD precise prototype**
   - 先正确，再 tile/shared-memory 优化；
   - 四种 Bayer pattern 和边缘处理独立验证；
   - 不与 calibration fusion 同时首次落地。
3. **2C：GPU-resident 串联与 fusion**
   - production path 禁止中间 readback；
   - profiler 证明收益后再融合 calibration/RCD 或后续 color pass；
   - transfer queue 与 compute overlap 只有在实际 queue family 和 profiler 支持时采用。

### 交付物

- U16 RAW Vulkan input type/ownership；
- calibration compute shader；
- GPU RCD precise implementation；
- stage readback test harness（仅测试）；
- demosaic quality corpus/report；
- U16 upload bytes、GPU stage timestamp、VRAM peak 和 E2E benchmark。

### 验收门槛

- production telemetry 中 FP32 RGB upload 必须为 0；
- production path 不允许 calibrated/Camera RGB readback；
- U16 RAW upload byte accounting 精确；
- precise demosaic 达到正式批准的容差，且无结构性 artifact；
- 最终 YUV、MOV、音频和错误处理继续满足 Stage 1 标准；
- 完整样本相对 Stage 1 有可重复性能收益。

### 停止条件

- 如果 GPU RCD 质量无法达到 precise 门槛，保留 CPU RCD 路径并将近似算法限制在 fast
  mode；不能降低 precise 名义标准；
- 如果 Vulkan ProRes 与完整 GPU image pipeline 开始争用 compute，必须重新 profile
  queue overlap、async depth 和 shader/encoder占比，再决定优化方向。

---

## Stage 3：混合精度与 fast mode

### 前置条件

- Stage 1/2 的 FP32 precise pipeline 已正确、稳定且有完整 golden；
- 已测得 GPU stage 的带宽、occupancy、VRAM 和 compute 分布；
- 能证明 FP16/fast 优化针对真实瓶颈，而不是只减少理论字节数。

### 工作项

按一次只改变一个变量的顺序评估：

1. FP16 intermediate storage，FP32 accumulation；
2. FP16 color/sharpen 部分计算；
3. texture DI LUT / analytic DI；
4. 非 bit-exact GPU dither；
5. precise RCD 与 fast demosaic 的独立选择。

### 验收门槛

- 每个变量都有 A/B output、数值报告、视觉 corpus 和 performance delta；
- 单项若端到端收益低于噪声或引入不可接受 artifact，则撤销该项；
- preset/sidecar 必须明确记录 precision、demosaic implementation、dither 和 DI mode；
- fast 输出不能与 precise 输出使用无法区分的配置身份。

---

## Stage 3G / Batch D.1：GPU pipeline 结构性清理与受控实验

### 正式诊断

2026-07-15 serialization review 确认两个代码事实：

1. vendored FFmpeg 的 `prores_ks_vulkan` compute exec pool 被硬编码为 1，并将用户请求的
   `async_depth` 覆盖为实际 pool size 1；当前 sidecar 记录的是 requested depth，不是 effective
   depth；
2. 应用 processing command 和该单 context encoder 当前都使用第一个 compute family 的 queue
   index 0，因此两者的 GPU command 在同一 Vulkan queue 上按序执行。

这些是需要修正或实验的结构性限制，但当前 benchmark **没有证明**它们造成可回收的 40～56
fps 大幅缺口。多个 Vulkan queue 仍共享同一 GPU 的 SM、cache 和 memory bandwidth；第二个
queue 只提供并发调度机会，不等于第二个硬件 encoder。任何收益必须由相同真实帧、相同进程、
相同配置的 matched A/B 证明。

### D.1-A：删除已确认的无意义 wall-time 开销

1. 正常转换不再在 E2E timer 内无条件 `av_read_frame()` 扫描刚写完的整份 MOV。现有约
   1.40 GB / 240 帧输出的只读复测约为 0.67～0.81 秒，占 6.51 秒完整转换约 10～12%；
2. 保留轻量 reopen、stream/codec/color metadata、trailer 和已有 writer 内部 packet count/PTS
   invariant；完整 packet scan 通过显式 verify 模式或 release/test gate 执行；
3. telemetry 分离 `startup_preflight`、`conversion_core`、`output_validation` 和用户可见
   `process_wall`，禁止用不同 timer boundary 的 FPS 互相比较；
4. 消除 capability probe 与真实 writer 对 full-resolution Vulkan runtime/frame pool/encoder 的
   重复构造。forced Vulkan 可由真实 writer initialization 负责失败；auto fallback 采用轻量
   probe、资源复用或在提交正式输出前捕获真实 initialization failure。

### D.1-B：修正 effective async depth，并把收益视为实验结果

1. 以可持续的 vcpkg patch 修正 `proresenc_kostya_vulkan.c`：compute exec pool 使用受设备
   queue/resource 限制的 requested depth，不再静默固定为 1；
2. sidecar 同时记录 requested/effective depth、compute pool size、queue family/index 分配；
3. 使用完整真实帧单进程 workload 测 depth 1/2/4/8，报告 combined E2E、processing GPU
   timestamps、`encoder_send/receive`、queue idle、packet transfer、VRAM 和输出正确性；
4. 不以“`encoder_send` 进入 low single digits”或“E2E 必须达到 40 fps”作为预设验收值。
   只接受超出 benchmark spread、没有质量/稳定性回归且资源增长合理的 depth。

仅创建多个 `VulkanProResEncoder` 并在当前单一 Vulkan encoder thread 中 round-robin 不会产生
并发：每次 blocking `send()` 返回后才可能调用下一个实例。若未来评估多 context app-only
方案，必须明确增加并发 workers、packet reorder、共享 device synchronization 和 VRAM 预算；
它不是 FFmpeg pool 修正的等价低风险替代品。

### D.1-C：有证据后才实验 queue separation

只有 D.1-B 证明 effective depth 增加后仍存在可重叠的 queue idle/bubble，才进行 queue
separation：

- 为 processing 与 encoder 明确分配/保留不同 queue index，不能只让 app 改到 index 1，
  同时又让 FFmpeg exec pool round-robin 占用所有 queue；
- queue lock/unlock 必须使用实际 family/index，timeline semaphore 的跨 queue ownership 和
  wait/signal 值必须继续正确；
- 单独报告 async-only、queue-only、async+queue 三组 matched A/B；
- 50～56 fps 只能作为探索上界，不能写成预期结果或验收门槛。

### D.1-D：RAW staging、slot depth 和其他次级候选

- `raw_upload` 当前只要求 `HOST_VISIBLE | HOST_COHERENT`；先记录实际 memory type/heap flags，
  再判断 shader 是否确实直接读取 system-visible memory；
- device-local RAW staging 不会消除 25.2 MB PCIe transfer，只可能把它移到 copy engine 并与
  compute 重叠。必须以完整 E2E 改善验收，不能只以 `raw_calibration` timestamp 下降验收；
- 当前 slot wait、frame allocation、queue-lock wait、submit 和 mux 都不是主瓶颈。只有修正
  async 后 telemetry 出现 slot backpressure，才提高 resident slot/queue depth；
- 不再增加完整应用并行进程。现有 1/2/4 process aggregate 已从 32.75 降至 30.02/28.63 fps，
  同时 VRAM 从约 3.3 GB 增至 6.0/11.2 GB。

### 验收与停止条件

- 每一步一次只改变一个变量，使用相同 executable/config/input 做 warm-up + 至少三次正式 A/B；
- 保持 `direct_frames == frame_count`、RGB/YUV upload/readback 为 0、最终 YUV/ProRes/PTS/audio
  和取消/partial cleanup 不回归；
- 已确认的 validation/preflight 清理以用户可见 wall time 和正确性验收；async/queue/staging
  以 matched conversion-core 与 process-wall 同时改善验收；
- 若 async depth、queue separation 或 staging 未超出噪声，正式记录 no-go，并接受 shared GPU
  useful work 是该硬件上的 steady-state 上限；不得继续用更多进程、slot 或未经证实的复制
  重构追逐理论并行。

---

## Stage 4 / Batch E：完成 CPU MCRAW compression 6/7 decoder

### 正式边界与目标

Batch E 是 CPU decoder 的完整实施批次，不再是 GPU decoder 的前置评估。最终生产数据流为：

```text
CPU file read/index + metadata
  -> CPU compression 6/7 decode
  -> contiguous U16 RAW mosaic
  -> existing Vulkan calibration/RCD/color/DI/YUV/ProRes
  -> compressed ProRes packet to CPU mux
```

Batch E 的目标是：固定 compression 6/7 的 official truth；消除已知 RAW buffer 过度分配；
以持久、有界的帧级 worker pool 取代 per-frame `std::async`；在统一 CPU 线程预算内建立稳定
容量；只有 matched benchmark 证明仍有价值时，才引入仓库内的 bit-exact CPU 快速 decoder。
official decoder 必须始终保留为 golden/reference 和可选择的 fallback。

### 格式边界：compression 6 与 7 必须分轨

- 当前固定的 `release/0.2` commit `06bf1a8` 只接受 compression 7，不能作为
  compression 6 truth source；第一步必须更新并固定真正同时覆盖 6/7 的 immutable commit；
- compression 7 使用 64-sample block、独立 bits/refs metadata 和 4-row interleave；
- compression 6 是独立 legacy bitstream，具有不同 block、padding、offset 和逐行交织语义；
- compression 7 的 block-offset 表、4-row band 或 SIMD kernel 不得未经独立设计直接复用于
  compression 6；两种格式分别建立 golden corpus、错误输入测试、benchmark 和 telemetry；

### 项目决策：compression 6 验收豁免（2026-07-16）

项目决定不再等待或要求真实 compression 6 素材，放弃本项目内对 compression 6 的素材级
golden、损坏输入、容量和完整流水测试验收。该决定是测试覆盖豁免，不是格式不支持声明，也
不改变已冻结的 official upstream decoder。

运行时策略保持为：遇到 metadata 标记为 compression 6 的 MCRAW 时，继续尝试调用 official
legacy decoder 完成转码；同时必须向控制台输出明确警告，说明 compression 6 未经过本项目
真实素材验证，输出不具备本项目 compression 7 同等级的验证保证。不得静默宣称 compression
6 已通过 bit-exact 或生产验收。本文只记录策略与验收边界，本次不修改代码实现。
- official decoder 更新后先重新审计其 SIMD、分配和输出 API。旧 `06bf1a8` 的标量热点数据
  只能作为历史背景，不能直接证明新自研 decoder 的收益。

### E-A：冻结 official truth 与输入语义

1. 固定覆盖 compression 6/7 的 official MotionCam decoder commit，记录 upstream diff、许可证、
   build flags 和 source hash；
2. 吸收 RAW output buffer 类型/长度修复：4096×3072 U16 RAW 的有效容量应为 24 MiB/frame，
   不得再以字节数 resize `std::vector<uint16_t>` 形成 48 MiB capacity；
3. 明确 `BUFFER` payload、metadata、width/height、encoded width/height、compression type 和
   output span 的 API 合约，禁止靠调用方 workaround 隐藏 decoder 尺寸错误；
4. compression 6/7 各选真实首/中/末帧并保存 payload hash、metadata 和 official U16 hash；
5. 对 unsupported compression、截断 payload、非法 header/offset/bit width、尺寸乘法溢出和
   output-too-small 建立安全失败测试。

### E-B：持久、有界的帧级调度与内存

1. 将 `src/cli/main.cpp` 的 per-frame `std::async` 替换为转换实例私有的持久 worker pool；
2. queue 和 in-flight frame 数必须有界，保持输入顺序交付、取消、首错传播、drain、partial
   output cleanup 和 decoder instance pool 语义；
3. 每个并发 decoder instance 复用 compressed scratch、metadata scratch 和 U16 output capacity，
   hot path 不得每帧创建线程或无条件重新分配；
4. 建立统一 CPU 预算：`frame_workers × decode_threads_per_frame` 不得绕过总线程上限。
   完整流水默认优先帧级并行，帧内 decode threads 初始为 1；高帧内并行仅用于单帧低延迟
   或 matched benchmark 证明其端到端更优的配置；
5. 生产初始候选为 6 个持久 frame workers，但必须按目标硬件、总线程预算、工作集和下游
   backpressure 自动收敛；不得把 16-worker decoder-only 峰值直接设为默认。

### E-C：重新建立 decoder 基线与容量分档

2026-07-15 的预评估可作为实验设计依据，但不是新实现的验收结果。该 Ryzen 7 3700X、
4096×3072 compression 7、826 帧 warm 样本结果为：

| Persistent workers | Median decode throughput | Run range | CPU core-equivalent | P95 loadFrame | Peak working set |
|---:|---:|---:|---:|---:|---:|
| 1 | 43.77 fps | single run | 1.00 | 20.42 ms | 68 MiB |
| 2 | 77.25 fps | single run | 2.00 | 24.60 ms | 120 MiB |
| 4 | 111.02 fps | 108.59-111.48 | 3.95 | 35.42 ms | 235 MiB |
| 6 | 121.82 fps | 119.83-123.81 | 5.96 | 48.47 ms | 351 MiB |
| 8 | 126.10 fps | 124.67-127.13 | 7.89 | 62.32 ms | 467 MiB |
| 12 | 130.74 fps | 130.05-131.29 | 11.65 | 90.90 ms | 697 MiB |
| 16 | 136.66 fps | 135.59-138.15 | 13.33 | 135.35 ms | 927 MiB |

更新 official decoder 和 worker pool 后，必须分别测量：

- 内存 payload → U16 的纯 decoder-only；
- 真实文件读取、metadata JSON parse、decode 和输出 buffer 的 `loadFrame`；
- 完整 precise/fast Vulkan pipeline；
- compression 6 与 7；
- frame workers 1/2/4/6/8，以及受统一预算约束的帧内线程组合。

每组至少 warm-up 后三次长样本正式运行，报告 P50/P90/P95/P99、MP/s、CPU
core-equivalent、working set、compressed input GB/s、producer wait、queue depth、downstream
backpressure 和端到端 FPS。继续报告 90/120/130 fps 的容量与资源成本，但这些是容量分档，
不是要求所有 8 核机器必须达到的产品承诺。4096×2160 的 2 ms/frame 仅可作为探索性
stretch result，不得代替项目 4096×3072 corpus 的正式门槛。

### E-D：compression 7 仓库内快速 decoder（条件实施）

只有 E-C 证明更新后的 official decoder 仍值得优化，才按以下顺序实施新模块；不得原地修改
FetchContent source：

1. **M0：benchmark、golden、synthetic encoder 与 bit-width histogram。** 先稳定复现
   official 输出和真实性能；
2. **M1：scratch 复用与直写 U16。** 删除 `p0..p3 -> row vectors -> memcpy` 中间流量，
   保持单线程和原有 unpack 语义；
3. **M2：block offset 表与完整输入验证。** bits metadata 解出后用 64-bit 前缀和构建绝对
   offset，验证最终范围，再允许 block/band 独立调度；
4. **M3：4-row band 帧内并行。** 使用 OpenMP 或已有持久执行器，按 band grain 调优；生产
   默认是否大于 1 thread 由统一预算下的完整流水 benchmark 决定；
5. **M4：独立 ISA translation units。** scalar/SSE4.1/AVX2 分开编译并运行时检测；AVX2
   不得施加到整个二进制。按真实 bit-width histogram 逐个增加 kernel，单独测试 lane
   interleave、reference add、尾部裁剪和 `vzeroupper`；
6. **M5：opt-in 集成、fuzz 和 matched A/B。** 新路径先可选择启用，official 路径保留；
   达到本节所有正确性、稳定性和性能 gates 后才讨论默认切换。

建议模块边界为仓库自有 `raw_decode` API：输入 immutable payload、明确的 compression type、
dimensions、可写 U16 span、options 和外部复用 scratch；返回结构化错误码与 metadata/offset/
payload/total timings。禁止异常作为正常错误分支、hot-loop allocation、未对齐/端序假设或让
整个 `mcraw_core` 依赖 AVX2。

### E-E：compression 6 优化策略

compression 6 首先只接入更新后的 official legacy decoder 和 bit-exact tests。只有真实
compression 6 corpus 证明其为容量或延迟热点，才建立独立 milestone；其 block offset、行级
并行、SIMD 和尾部方案必须从 legacy format 重新推导。没有真实 corpus、official golden 和
matched benchmark 时，不实现或宣称 compression 6 fast decoder。

### E-F：验收、默认路径与停止条件

不可放宽的正确性门槛：

- compression 6/7 的每个实现、ISA 和线程组合都与对应 official decoder U16 逐像素 bit-exact；
- 输出与 worker 数、帧内线程数和调度顺序无关；
- payload 长度、header、offset、尺寸、padding、encoded width 和 output span 全部边界验证；
- 损坏输入、取消和 decoder 异常不能产生伪装完整的 MOV；
- CPU/GPU backend 的 metadata、PTS、音频、fallback、cleanup 和 sidecar 行为不回归。

性能 go/no-go：单项优化必须在 matched 长样本中超出噪声，并满足至少一项：decoder-only
中位数改善 ≥15%；在同一吞吐分档下降低至少一个 frame worker；或显著降低工作集/P95 且
端到端不回退。若 M1–M4 未达到该门槛，保留已验证的较简单版本，不为追求理论 SIMD 宽度
继续增加复杂度。

新 decoder 成为默认前还必须通过 compression 6/7 完整 corpus、scalar/SSE/AVX2、
1/2/4/6/8 workers、取消/损坏输入、长时间和完整 precise/fast Vulkan A/B。即使快速路径成为
默认，official decoder 仍保留为 reference/诊断路径。Batch E 完成后，GPU pipeline 边界正式
冻结在 U16 RAW upload；不再保留 GPU decoder 后续步骤。

RAW 解压是整数语义和源数据正确性边界，不属于允许画质误差的 fast mode。

---

## 5. 性能测量与 go/no-go 规则

每个 stage 的报告使用统一表格：

| 类别 | 必须报告 |
|---|---|
| Build | commit、Release/Debug、FFmpeg commit/config、shader hash |
| Hardware | CPU、RAM、GPU、VRAM、driver、Vulkan API |
| Input | 文件 hash、尺寸、帧数、音频、配置 |
| CPU | total/process utilization、各 CPU stage mean/P50/P95 |
| GPU | 各 pass timestamp、queue busy/idle、encoder time、utilization |
| Transfer | compressed input read bytes、U16/RGB upload bytes 与 GB/s、readback bytes |
| Memory | system RAM peak、VRAM peak、slot/pool count |
| Queues | capacity、max depth、wait count/time |
| Output | conversion-core/process wall、validation wall、fps、packet bytes、disk MB/s、frame/PTS/audio validation |
| Quality | stage error、final YUV error、decoded comparison、人工结论 |

统一规则：

1. 同一 executable/config/input 做 matched A/B；
2. 先 warm-up，至少三次完整正式运行；
3. 报告 median 和 min/max 或标准差；
4. 短 smoke 只验证正确性，不用于承诺最终性能；
5. stage timer 有并行重叠时，不能把 mean 相加当 wall time；
6. 优先依据 GPU timestamp 和 queue idle 判断 shader/producer/encoder 瓶颈；
7. 没有 profiler 证据时，不优先做 descriptor churn、queue 数量或 mux buffer 微调；
8. `encoder_send` 等 API wall timer 只表示阻塞归属，不自动等于该 stage 的独占执行时间；
9. 禁止用设备级 sampled GPU utilization × wall time 推导 shader GPU-busy time；
10. isolated benchmark 与 combined benchmark 的输入内容、upload、进程数和 timer boundary 必须
    一致或明确披露差异，不能把跨 workload 推导值当作验收事实；
11. 分别报告 startup/preflight、conversion core、output validation 和 process wall；
12. 每阶段设置 rollback point，性能未改善且复杂度显著上升时保留上一个稳定切分点。

---

## 6. 发布 gates：与性能开发并行但不能跳过

以下问题不阻止实验性 Stage 1/2 开发，但阻止 Vulkan backend 成为默认生产路径：

1. [x] pinned FFmpeg ProRes DCT shader 的 GPU-assisted validation race：已在当前驱动与
   validation layer 复现，并以 `GPU_BATCH_F_VALIDATION_RACE_WAIVER.md` 记录受限 waiver；
2. DaVinci Resolve、Adobe Premiere 和至少一个桌面播放器的 decode/seek/color/duration/
   audio sync 实测；其中 FFmpeg/VLC 已通过，Resolve 因外部脚本禁用未通过，Premiere 未安装，
   详见 `GPU_BATCH_F_COMPATIBILITY.md`；
3. 明确并验证 chroma siting，以及 primaries/TRC metadata 产品决策；
4. AMD、Intel Vulkan 硬件和第二代 NVIDIA driver 覆盖；
5. 一小时真实素材转换；
6. 多文件 batch、重复启动、取消、device lost 和资源增长测试；
7. 24 fps 最低目标是否作为正式产品门槛的书面确认。

metadata/chroma 决策必须同时作用于 CPU 与 GPU backend，不能只修 Vulkan 输出导致 reference
分叉。

---

## 7. 建议的执行批次

### Batch A：先建立事实基线

- 完成 Stage 0 corpus、GPU timestamp 和统一 benchmark 报告；
- 同时记录并确认 precise/fast 的产品语义；
- 输出：baseline manifest、benchmark report、quality budget 决策。

### Batch B：迁移最大热点

- 实施 Stage 1 的 GPU color/sharpen/DI；
- 先分 pass golden，再 profile 和选择性 fusion；
- 输出：Camera RGB entry 的 precise Vulkan pipeline。

### Batch C：减少 6× 上传并消除 demosaic 热点

- 实施 Stage 2A calibration；
- 实施 Stage 2B GPU RCD；
- 串联 Stage 1，生产路径不 readback；
- 输出：U16 RAW entry 的 precise Vulkan pipeline。

### Batch D：基于 profiler 做性能模式

- 逐项实验 FP16、DI、dither 和 fast demosaic；
- 只合入具有可测端到端收益并通过质量预算的项；
- 输出：可明确识别的 precise 与 fast presets。

### Batch D.1：清理确认开销并验证 GPU serialization 假设

- 先把完整 MOV packet scan 从正常 E2E 移到显式 verify/release gate，并消除重复 Vulkan
  full-resolution preflight；
- 再修正 FFmpeg effective async depth 和 telemetry，以真实帧单进程 depth matrix 验证收益；
- 只有 async 证据成立后才实验显式 queue reservation/separation；只有 memory-type 与 transfer
  timeline 证据成立后才实验 device-local RAW staging；
- 输出：分离 timer boundary 的 matched benchmark、requested/effective depth 报告，以及每个
  serialization/staging 候选的 go/no-go 结论；不预先承诺 40～56 fps。

### Batch E：完成 CPU decoder

- 更新并固定真正覆盖 compression 6/7 的 official truth source，修复 RAW buffer 过度分配；
- 用持久、有界 worker pool 替代 per-frame `std::async`，建立统一 CPU 线程预算；
- compression 6/7 分轨建立 bit-exact golden、安全测试和 matched benchmark；
- 仅在新 official baseline 证明有收益时实施 compression 7 直写、offset、帧内并行和独立
  ISA 快速路径；compression 6 只有在真实 corpus 证明需要时才另做 legacy 优化；
- 输出：90/120/130 fps 分档下的 CPU decoder capacity、资源成本、默认路径决策，以及冻结在
  U16 RAW upload 的最终 GPU pipeline 边界。

### Batch F：关闭生产发布 gates

- 多软件、多硬件、长时间、batch 和 validation race；
- 达到明确产品性能阈值后，才讨论是否把 `backend=auto` 或 Vulkan 设为更积极的默认。

---

## 8. 每次实施任务给开发代理的固定要求

后续每个任务都应明确提供：

1. **唯一阶段边界**：本次只迁移哪个输入/输出；
2. **CPU reference**：对应的函数、数据类型和 frozen semantics；
3. **golden corpus**：使用哪些固定帧和 synthetic case；
4. **允许误差**：bit-exact、≤1 LSB 或 fast budget，不允许使用“视觉差不多”；
5. **ownership/sync**：buffer/image/AVFrame 生命周期和 semaphore 合约；
6. **telemetry**：本次必须新增或保持的 counter/timestamp；
7. **failure semantics**：fallback、forced Vulkan、device lost、partial cleanup；
8. **benchmark**：matched A/B 条件和 go/no-go 阈值；
9. **非目标**：列出本次不顺带修改的 CPU、metadata、算法或配置行为；
10. **完成定义**：测试、文档、样本验证和性能报告全部满足后才算完成。

GPU 阶段建议每个任务保持“一项新 GPU 语义 + 一套 golden + 一份 benchmark”，不要在同一
任务中同时引入新 stage、FP16、算法近似和 pass fusion。Batch E 对应的最小变更单位改为
“一项 decoder/truth/调度语义 + bit-exact golden + matched benchmark”，同样禁止把 worker
pool、格式迁移、帧内并行和新 ISA kernel 首次落在同一提交中。

---

## 9. 下一步立即执行清单

正式行动建议从以下顺序开始：

- [x] 批准本文作为下一阶段实施总纲；
- [x] 确认最低性能目标是 24 fps，30 fps 为扩展目标；
- [x] 确认 precise 保持 ≤1 LSB，fast 使用独立 preset/sidecar 身份；
- [x] 执行 Stage 0：冻结 baseline manifest、quality corpus 和 GPU timestamp；
- [x] 为 Stage 1 写独立技术设计，冻结 Camera RGB input format、shader pass、uniform 和
      golden boundary；
- [x] 实施并验收 Stage 1，不同时启用 FP16；Stage 1G 中位数 `13.791 fps`，相对
      重建 Stage 0 为 `+100.943%`，通过 `+20%` gate；
- [x] 用 Stage 1 profiler 决定 pass fusion 与 Stage 2 优先级；回退根因是 CPU finite
      全帧重复扫描及 pack/encoder 串行，而非 shader fusion；修正后瓶颈回到 CPU producer；
- [x] 实施 Stage 2A/2B，把生产上传切换到 U16 RAW；Stage 2E 中位数
      `37.747 fps`，相对 Stage 1G `+173.710%`，final YUV `<=1 LSB`；
- [x] precise 完成后才开始 mixed/fast A/B；Batch D 最终 precise/fast
      中位数为 `34.776/36.857 fps`；合入 FP16 intermediate storage 与
      analytic DI，移除未作为公开模式的 balanced preset，拒绝 dither-off 和
      质量不合格的 bilinear demosaic；
- [x] Batch D.1-A：从正常 E2E 移除完整 MOV packet scan，分离 startup/conversion/validation/
      process timers，并消除重复 full-resolution Vulkan preflight；
- [x] Batch D.1-B：修正并报告 FFmpeg requested/effective async depth，使用真实帧单进程
      depth 1/2/4/8 做 matched A/B；未证明收益前不实施 queue separation；
- [x] Batch D.1-C/D：只在前置 telemetry 证明可回收 idle 或 PCIe/heap 问题后，分别实验
      queue reservation 与 device-local RAW staging；未超出噪声则记录 no-go；
- [x] Batch E-A：official truth 更新到同时覆盖 compression 6/7 的 immutable commit，吸收
      RAW buffer 过度分配修复，并确认现有 compression 7 首/中/末帧 U16 hash 不变；
- [x] Batch E-B/E-C：实现持久、有界 frame worker pool，完成更新后 compression 7 的
      decoder-only、loadFrame 与完整 precise/fast Vulkan 1/2/4/6/8 worker matrix；
- [x] Batch E-D/E-E go/no-go：更新后的 official compression 7 decoder 已超过
      90/120/130 fps 容量分档且不是完整流水瓶颈，自研 fast decoder 为 no-go；无真实
      compression 6 corpus，不启动 legacy fast decoder；
- [x] Batch E-F compression 6 corpus gate：按 2026-07-16 项目决策放弃素材级 compression 6
      测试验收，记录为显式 waiver；运行时继续尝试 official legacy decoder，并要求控制台
      警告未验证状态；GPU pipeline 边界保持冻结在 U16 RAW upload；
- [ ] 性能开发期间持续推进 NLE/硬件/长时间发布 gates。

Stage 0～3 / Batch A～D、Batch D.1-A～D 以及 Batch E-A～E-F 已完成；D.1-C/D 的
queue/staging 候选与 E-D/E-E fast decoder 均记录 no-go。compression 6 素材级验收按
2026-07-16 决策显式豁免；该格式继续走 official legacy decoder，并以控制台警告标记未验证
状态，不得将其描述为已完成本项目级 golden/安全/容量验收。
