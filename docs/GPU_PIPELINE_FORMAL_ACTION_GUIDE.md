# GPU Pipeline 下一阶段正式行动指南

日期：2026-07-14  
性质：实施与验收总纲  
适用范围：当前 Phase 0–8 已完成的 Vulkan ProRes 后端之后，继续把 GPU
pipeline 的入口从 TargetLog FP32 RGB 向 Camera RGB、U16 RAW mosaic，最终向
MCRAW compressed payload 前移。

本文综合以下现状文档，并把已有分析转换为可执行的阶段、交付物、验收门槛和停止条件：

- `GPU_PIPELINE_SUMMARY_AND_NEXT_STEPS.md`
- `GPU_PHASE1_VALIDATION.md` ～ `GPU_PHASE8_PRODUCTION.md`
- `GPU_PIPELINE_AUDIT.md`
- `VULKAN_PRORES_GPU_PIPELINE_GUIDE.md`
- `implementation-status.md`

本文不改变现有 CPU reference、Vulkan opt-in 策略或发布门槛。后续实现如与本文冲突，
应先更新本文或记录 ADR，不能在实现中静默改变目标。

---

## 1. 当前基线与正式结论

### 1.1 已经完成的能力

当前 Vulkan 路径已经完成：

```text
CPU official RAW decode
  -> CPU calibration
  -> CPU RCD demosaic
  -> CPU Camera RGB / DWG / sharpening / DI
  -> FP32 TargetLog RGB staging upload
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
- 有界 RGB job queue、GPU slots、packet queue 和独立 mux worker；
- normal path 无 `vkQueueWaitIdle()` / `vkDeviceWaitIdle()`；
- backpressure、取消、异常传播、device lost 和 partial 文件清理；
- `auto` 全尺寸 preflight/fallback 与强制 Vulkan 失败语义；
- 240 帧 4096×3072 ProRes HQ + PCM 完整输出和软件全流解码。

### 1.2 当前性能事实

在已记录的 RTX 3060 / NVIDIA 576.02 样本上：

| 项目 | 当前结果 |
|---|---:|
| CPU 历史完整转换 | 约 2.52 fps |
| 当前 Vulkan 完整转换 | 约 6.95 fps |
| 端到端提升 | 约 2.76× |
| GPU queue peak | 5 / 10 |
| packet queue peak | 1 / 16 |
| backpressure | 0 |
| RGB staging | 约 151 MB/frame |
| 240 帧 RGB staging 总量 | 36,238,786,560 bytes |

主要 CPU 热点：

| 阶段 | Phase 8 mean |
|---|---:|
| Camera RGB → DWG → sharpening → DI RGB | 511.45 ms/frame |
| RCD demosaic | 400.32 ms/frame |
| official RAW decode + metadata | 56.21 ms/frame |
| black/white calibration | 49.31 ms/frame |

阶段计时会因多帧并行重叠，不能直接相加推算最终 FPS；但 queue、backpressure 和
`prores_submit_wait` 数据共同证明：**当前 GPU/encoder 在等待 CPU producer，首要问题
不是继续微调现有 RGB-to-YUV shader 或 mux queue。**

### 1.3 zero-copy 的准确口径

当前已经实现的是：

```text
Vulkan RGB-to-YUV -> FFmpeg AVVkFrame -> Vulkan ProRes
```

这段没有未压缩 YUV upload/readback，因此当前 telemetry 中
`gpu_resident=true`, `direct_frames=N`, `upload_frames=0`, `readback_frames=0`
在既定定义下成立。

当前尚未实现全 RAW pipeline GPU resident：CPU 仍生产并上传三张 FP32 RGB plane。
下一阶段的实际目标不是字面上的“零传输”，而是：

> 每帧只向离散 GPU 上传一次尽可能小的源数据；所有大尺寸未压缩中间图像留在 VRAM；
> 编码后只让压缩 packet 回到 CPU mux。

---

## 2. 下一阶段的正式目标

### 2.1 主目标

按以下顺序将 Vulkan pipeline 入口前移：

```text
TargetLog FP32 RGB（当前）
  -> Camera RGB FP32
  -> calibrated / U16 RAW mosaic
  -> compressed MCRAW payload（仅在 profiler 证明需要时）
```

### 2.2 性能目标

项目开始实施前应正式确认一个最低目标和一个扩展目标。建议：

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
- 只有 drain、trailer、close、reopen validation 成功后才发布最终文件；
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
   - compressed input upload；
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

## Stage 4：评估 GPU MCRAW compression 6/7 decode

### 是否启动的决策门槛

Stage 2/3 完成后重新 profile。只有满足以下任一条件才正式投入 GPU decoder：

- CPU official decode 成为端到端 top-2 热点；
- CPU decode 无法稳定供给目标 24/30 fps；
- U16 upload/CPU memory traffic 成为明显瓶颈；
- batch/多流场景下 CPU decode 限制扩展性。

否则保留 official CPU decoder：约 56–61 ms/frame 的阶段可被多帧流水部分掩盖，GPU
decoder 的复杂度和验证成本可能不值得。

### 目标数据流

```text
CPU file read/index
  -> compressed payload upload (~11 MB/frame on current sample)
  -> Vulkan compression 6/7 decode
  -> Vulkan calibration/RCD/color/DI/YUV/ProRes
  -> compressed ProRes packet to CPU mux
```

### 不可放宽的门槛

- official MotionCam decoder 继续是真值；
- RAW U16 输出必须 bit-exact，不能用最终 ProRes 相似度替代；
- payload 长度、边界、损坏输入和 unsupported compression 必须安全失败；
- decoder 失败不能产生伪装完整的最终 MOV。

RAW 解压是整数语义和源数据正确性边界，不应作为“可接受少量画质误差”的优化阶段。

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
| Transfer | payload/U16/RGB upload bytes 与 GB/s、readback bytes |
| Memory | system RAM peak、VRAM peak、slot/pool count |
| Queues | capacity、max depth、wait count/time |
| Output | fps、wall time、packet bytes、disk MB/s、frame/PTS/audio validation |
| Quality | stage error、final YUV error、decoded comparison、人工结论 |

统一规则：

1. 同一 executable/config/input 做 matched A/B；
2. 先 warm-up，至少三次完整正式运行；
3. 报告 median 和 min/max 或标准差；
4. 短 smoke 只验证正确性，不用于承诺最终性能；
5. stage timer 有并行重叠时，不能把 mean 相加当 wall time；
6. 优先依据 GPU timestamp 和 queue idle 判断 shader/producer/encoder 瓶颈；
7. 没有 profiler 证据时，不优先做 descriptor churn、queue 数量或 mux buffer 微调；
8. 每阶段设置 rollback point，性能未改善且复杂度显著上升时保留上一个稳定切分点。

---

## 6. 发布 gates：与性能开发并行但不能跳过

以下问题不阻止实验性 Stage 1/2 开发，但阻止 Vulkan backend 成为默认生产路径：

1. pinned FFmpeg ProRes DCT shader 的 GPU-assisted validation race：解决、升级验证或正式
   记录 waiver；
2. DaVinci Resolve、Adobe Premiere 和至少一个桌面播放器的 decode/seek/color/duration/
   audio sync 实测；
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

### Batch E：重新决定是否做 GPU RAW decoder

- 用 Stage 2/3 完整结果判断 compression 6/7 decoder ROI；
- 若启动，坚持 U16 bit-exact；若不启动，记录 CPU decode 已满足 producer 需求的证据。

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

建议每个任务保持“一项新 GPU 语义 + 一套 golden + 一份 benchmark”。不要在同一任务中同时
引入新 stage、FP16、算法近似和 pass fusion，否则出现误差或性能回退时无法归因。

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
- [ ] Stage 2/3 完成后重新评估 GPU MCRAW decoder ROI；
- [ ] 性能开发期间持续推进 NLE/硬件/长时间发布 gates。

第一项实际工程任务应当是 **Stage 0 基线与 profiler 合约**，而不是直接开始写 FP16 或
融合 shader。第一项处理像素的工程任务应当是 **Stage 1 Camera RGB→DWG→sharpening→DI
的 FP32 Vulkan precise 路径**。
