# MCRAW → 高品质 ProRes 转码器：路线 B 架构与实现规范

> 文档用途：作为 VS Code / Codex 开展项目时的总体设计依据。  
> 当前阶段：Phase 0～2 CPU 路径已实现并完成并行优化；GPU 阶段尚未开始。  
> 暂定项目名：`mcraw-transcoder`。

---

## 1. 项目目标

开发一个专门处理 MotionCam `.mcraw` 的高品质、高性能转码器：

1. 直接解析 MCRAW 容器和逐帧元数据。
2. CPU 参考解码必须与 MotionCam 官方 decoder 位级一致。
3. 后续提供 Vulkan GPU MCRAW 解压路径。
4. 提供多种高品质 Bayer 去马赛克算法：
   - AMaZE
   - RCD
   - IGV
   - 可扩展其他算法
5. 黑电平、白电平、镜头校正、坏点修复、RAW 降噪等节点可配置。
6. 严格实现 DNG 双矩阵、双光源色温适配，不沿用不明确的简化插值。
7. 精确实现常见场景线性 Log/色域组合，包括：
   - F-Log / BT.2020
   - S-Log2 / S-Gamut
   - S-Log3 / S-Gamut3
   - S-Log3 / S-Gamut3.Cine
   - DaVinci Intermediate / DaVinci Wide Gamut
   - ARRI LogC3 / ARRI Wide Gamut 3
8. 使用 FFmpeg/libavcodec 编码 ProRes，不自行实现 ProRes 编码器。
9. 保留音频、时间戳和必要元数据。
11. 所有高开销步骤都可被独立测量、关闭或替换。

---

## 2. 明确的非目标

第一阶段不做：

- 非线性“美化”或自动风格化。
- 显示变换、HDR tone mapping、Rec.709 成片 Look。
- AI 去马赛克或 AI 降噪。
- 自行实现 ProRes 码流。
- 依赖 DNG 中间文件。
- 把整个 vkdt 或 RawTherapee 框架嵌入项目。
- 一开始就追求所有算法 GPU 化。
- GUI 优先；先完成稳定 CLI 和库接口。

项目应输出“可进一步调色的数字负片”，而不是自动完成风格化成片。

---

## 3. 核心设计原则

### 3.1 正确性优先于融合优化

每个阶段先建立独立、可验证的参考实现，再考虑：

- kernel 融合；
- FP16；
- fast-math；
- 近似曲线；
- LUT 替代解析公式；
- GPU 专用优化。

默认质量模式不得使用未经误差量化的近似算法。

### 3.2 格式解析与图像处理分离

MCRAW 容器、帧索引、音频、JSON 元数据属于 I/O 层。

RAW 解压、校正、去马赛克、颜色变换、Log 编码属于处理层。

ProRes、MOV、音频复用属于输出层。

三层不能互相依赖内部实现细节。

### 3.3 节点化，而不是主流程堆条件分支

每个处理功能都应是一个具有明确输入和输出格式的节点。节点具备：

- 名称与版本；
- 输入、输出像素类型；
- CPU/GPU 后端能力；
- 是否原地处理；
- 边界/halo 需求；
- 是否支持分块；
- 是否确定性；
- 预计显存和内存；
- 计时统计；
- 可序列化参数；
- 校验规则。

配置文件只是构建节点图，不应存在隐藏行为。

### 3.4 参考路径与高速路径并存

保留两条路径：

- **Reference**：CPU、FP32/FP64、无 fast-math，便于验证。
- **Accelerated**：Vulkan、异步流水线、可选受控近似。

高速路径必须能与参考路径逐阶段比较。

---

## 4. 推荐技术栈

### 4.1 主语言与构建

- C++20
- CMake
- Windows 11 和 Linux 为首要平台
- Vulkan 1.2/1.3 作为 GPU 后端
- SPIR-V shader
- FFmpeg/libavcodec/libavformat/libswresample
- JSON 配置及元数据解析
- 单元测试框架：Catch2 或 GoogleTest
- 基准测试：Google Benchmark 或自建稳定 benchmark runner

### 4.2 外部组件

| 组件 | 用途 | 原则 |
|---|---|---|
| MotionCam 官方 `motioncam-decoder` | 容器、索引、元数据、CPU 参考解码 | 作为兼容性基准 |
| FFmpeg | ProRes、MOV、PCM/音频复用 | 不重写编码与封装 |
| Vulkan | GPU RAW 解压及图像处理 | 主加速后端 |
| librtprocess | AMaZE/RCD/IGV CPU 参考实现 | 受 GPLv3 影响，需先决定许可证 |
| OpenColorIO Config ACES / CLF | 精确颜色变换的验证来源 | 默认不要求作为运行时依赖 |
| Adobe DNG SDK/规范 | 双矩阵颜色模型的规范依据 | 用于测试与行为对照 |

---

## 5. 许可证先决决策

`motioncam-decoder` 是 Apache-2.0；FFmpeg 的许可取决于具体构建；`librtprocess` 整体为 GPLv3 或 GPLv3+；vkdt 仓库存在不同许可文件，必须逐文件确认。

### 推荐二选一

#### 方案 L1：整个应用采用 GPLv3

优点：

- 可以直接链接 librtprocess。
- 可以较自由地复用 GPL 兼容代码。
- 最快获得成熟的 AMaZE、RCD、IGV。

缺点：

- 分发时整个组合程序必须履行 GPL 义务。
- 对未来闭源或商业集成限制较大。

#### 方案 L2：核心保持宽松许可

要求：

- 不直接复制或链接 GPL 去马赛克代码。
- 根据公开论文和算法描述重新实现。
- OpenColorIO/ACES CLF 可作为测试参考，但仍需检查具体文件许可和版权头。
- vkdt 代码只能在逐文件许可确认后使用。

**重要：** 动态插件或独立进程并不自动规避 GPL 义务。许可证结构应在写大量代码前决定；必要时寻求专业法律意见。

### 实际开发建议

若目标首先是自用和开源，采用 GPLv3 最省时间。  
若未来可能闭源分发，则从第一天就选择宽松核心，并把 librtprocess 仅用于本地对照测试，不进入发布构建。

---

## 6. 总体架构

```text
Application / CLI
│
├── mcraw_io
│   ├── container reader
│   ├── frame index
│   ├── metadata normalization
│   ├── compressed frame reader
│   └── audio reader
│
├── raw_reference
│   └── official CPU decompression adapter
│
├── processing_graph
│   ├── raw calibration nodes
│   ├── demosaic plugins
│   ├── color science
│   ├── target gamut / transfer function
│   └── YCbCr / chroma subsampling
│
├── backend_cpu
│   ├── scalar reference
│   ├── SIMD/OpenMP
│   └── optional librtprocess
│
├── backend_vulkan
│   ├── compressed RAW unpack
│   ├── GPU correction nodes
│   ├── GPU demosaic implementations
│   ├── color transforms
│   └── output packing
│
├── media_output
│   ├── ProRes encoder
│   ├── MOV muxer
│   ├── audio
│   └── sidecar metadata
│
└── validation
    ├── frame compare
    ├── transfer-function tests
    ├── matrix tests
    ├── codec round-trip
    └── stage benchmark
```

---

## 7. 模块职责

### 7.1 `mcraw_io`

职责：

- 打开和验证 MCRAW。
- 读取容器版本。
- 建立帧、音频和元数据索引。
- 提供线程安全的随机读取。
- 返回压缩帧字节，而不是强制 CPU 解压。
- 保留原始 JSON，另生成标准化强类型结构。
- 兼容旧字段拼写，例如历史上的 `sensorArrangment`。
- 不负责颜色处理。

若官方 decoder API 只能直接返回已解压帧，应增加薄适配层或最小 fork，让 GPU 路径能取得原始压缩 payload。官方 CPU 解码器仍保留为参考。

### 7.2 `raw_reference`

职责：

- 调用官方 CPU 解压。
- 对 compression type 6/7 等格式提供基准。
- 输出未做黑电平处理的原始 U16 Bayer。
- 用于自动验证 GPU 解压结果。

### 7.3 `processing_graph`

职责：

- 根据配置构建有向无环图。
- 验证节点顺序是否合法。
- 自动插入必要的格式转换。
- 估算内存、显存和队列深度。
- 收集每个节点的耗时、吞吐和错误。
- 允许从中间节点导出测试帧。

### 7.4 `backend_cpu`

职责：

- 正确性参考。
- 高品质 CPU 去马赛克。
- 无 GPU 或 GPU 不支持时的完整回退。
- 可用 SIMD/OpenMP，但必须保持确定性模式。

### 7.5 `backend_vulkan`

职责：

- 压缩数据上传。
- GPU MCRAW 解压。
- 可选 RAW 节点。
- 后续逐步实现 GPU 去马赛克。
- 色彩矩阵、Log、YUV 和量化。
- 使用持久映射 staging ring 和多帧异步流水线。

### 7.6 `media_output`

职责：

- 接收 RGB log 或 YUV422P10。
- 使用 FFmpeg `prores_ks` 或经验证的 ProRes encoder。
- MOV 封装。
- 音频对齐。
- 写入可表达的色彩元数据。
- 生成完整 sidecar JSON。

---

## 8. 像素数据类型和阶段

应使用明确类型，禁止使用含糊的“float image”。

| 类型 | 含义 |
|---|---|
| `CompressedFrame` | MCRAW 原始压缩帧 |
| `RawMosaicU16` | 解压后的传感器码值，未减黑 |
| `RawMosaicF32` | 已转 float 的 Bayer，可包含负值和超白 |
| `RawNormalizedF32` | 按 CFA 通道完成黑电平和白场尺度归一化 |
| `CameraRGBF32` | 去马赛克后的相机原生线性 RGB |
| `XYZD50F32` | 依照 DNG 模型得到的场景线性 XYZ D50 |
| `TargetLinearRGBF32` | 目标色域中的场景线性 RGB |
| `TargetLogRGBF32` | 精确 OETF 后的目标 Log RGB |
| `YUV444F32` | 非线性 R'G'B' 转换所得 Y'CbCr |
| `YUV422P10` | 滤波、色度抽样和量化后的 10-bit planar 422 |

### 精度原则

- RAW、去马赛克、颜色和 Log 默认 FP32。
- 矩阵生成、白点求解、参考曲线和测试向量使用 FP64。
- FP16 只能作为单独的 `fast` 模式，并必须给出误差报告。
- 中间阶段不要提前裁剪到 `[0,1]`。
- 保留负值与超白，直到具体输出策略要求限制。

---

## 9. 推荐处理顺序

```text
MCRAW compressed frame
→ lossless RAW decompression
→ CFA/crop phase resolution
→ black-level subtraction
→ white-level normalization
→ optional static/dynamic bad-pixel correction
→ optional lens shading / flat-field correction
→ optional RAW-domain spatial denoise
→ optional highlight reconstruction preparation
→ high-quality demosaic
→ optional post-demosaic chromatic aberration correction
→ white balance + DNG dual-illuminant camera transform
→ XYZ D50
→ target-white chromatic adaptation
→ target linear gamut
→ optional exposure offset
→ exact Log OETF
→ RGB-to-YCbCr packing transform
→ high-quality 4:2:2 chroma filtering
→ dither and 10-bit quantization
→ ProRes encoding
→ MOV/audio mux
```

### “镜头校正”必须拆分

它不是一个单独节点：

1. **镜头阴影/平场校正**：RAW mosaic 域，去马赛克之前。
2. **横向色差**：通常 RGB 域或专用 RAW 算法。
3. **几何畸变**：RGB 域，去马赛克以后。
4. **裁切/缩放**：在 CFA 阶段需特别注意 Bayer 相位；一般最终 RGB 域更安全。

---

## 10. 黑电平和白电平的处理规则

用户界面可以把黑白场显示为“可选节点”，但从颜色科学上它们不是普通画质特效。

### 黑电平

默认按文件元数据应用：

- 支持 1 个、2 个或 4 个 CFA 通道黑电平。
- 根据实际 CFA 布局映射，不假设固定 RGGB。
- 减黑后允许负值。
- 不在此阶段裁剪。
- 支持手动覆盖和诊断关闭。

### 白电平

白电平用于把传感器码值转为相对场景线性比例：

\[
R_\mathrm{norm} = \frac{R_\mathrm{raw}-B_c}{W_c-B_c}
\]

其中 \(c\) 是 CFA 颜色位置。

### 三种模式

- `metadata`：默认，使用 MCRAW 元数据。
- `manual`：用户指定。
- `bypass`：只用于诊断或 RAW 导出。

正常 Log/色彩输出若选择 `bypass`，程序应警告，最好要求 `force`，因为此时曝光尺度、白平衡和 Log 映射缺乏可靠物理含义。

### 高光

- 不要在白电平处自动硬裁切。
- 白电平上方若存在有效超白，应保留。
- 饱和检测与高光重建使用单独掩码。
- 每通道饱和阈值可独立配置。

---

## 11. CFA 和裁切相位

MCRAW 可能记录 RGGB、BGGR、GRBG 或 GBRG。

必须满足：

- 原始 CFA 排列强类型化。
- 任何奇数像素 crop 都会改变 CFA 相位。
- 去马赛克插件接收的是“当前图像原点对应的 CFA”，不是只读原始容器值。
- 分块去马赛克时，每个 tile 的局部 CFA 相位由全局坐标计算。
- 不支持的传感器排列应明确报错，不能猜测。

第一阶段只承诺常规 2×2 Bayer。Quad Bayer、X-Trans 或特殊排列应作为未来能力，不应误用 Bayer 算法。

---

## 12. 去马赛克子系统

### 12.1 插件接口概念

每个 demosaicer 应声明：

- 算法 ID 和版本。
- 支持 CFA。
- CPU、Vulkan 或其他后端。
- 输入范围要求。
- 是否要求先裁剪负值。
- 边界 halo 宽度。
- 最小 tile 尺寸。
- 是否支持独立 tile。
- 是否内部多线程。
- 是否确定性。
- 临时内存需求。
- 输出通道布局。
- 可调参数和推荐范围。

### 12.2 首批算法

#### AMaZE

定位：最高质量候选之一，重点测试细节、边缘和假色。

注意：

- 分支和邻域复杂，GPU 移植工作量大。
- 第一版可先用成熟 CPU 实现建立黄金参考。
- 不应预设它在所有素材上都优于其他算法。

#### RCD

定位：质量与性能平衡候选。

注意：

- 适合作为首个 GPU 高品质移植对象。
- 仍需针对细密纹理、饱和边缘和高噪场景测试。

#### IGV

定位：另一种高品质候选，重点评估噪声和伪色表现。

注意：

- 不将算法名称等同于固定场景优势。
- 最终默认值应由测试集决定，不凭经验命名为“最好”。

### 12.3 可附带的低成本算法

- Bilinear：只用于预览、调试和性能下限。
- BayerFast/VNG4：可选快速模式。
- 不允许把 Bilinear 作为默认高质量模式。

### 12.4 推荐预设

预设必须只是公开参数组合：

| 预设 | 建议 |
|---|---|
| `preview` | 快速去马赛克、关闭高开销修复 |
| `balanced` | RCD，关闭高开销后处理 |
| `quality` | AMaZE，使用算法自身的高质量重建 |
| `alternate` | IGV，用于用户比较 |
| `reference` | CPU、FP32、单一确定版本 |

默认算法在完成样片和 chart 测试前标记为“暂定”。

### 12.5 分块与视频并行

高品质算法可能需要较大邻域，因此：

- 插件报告 halo。
- tile 内部使用完整 halo，最终只写中心区域。
- 边界策略必须与整帧参考完全一致。
- 先实现整帧正确版本，再实现 tile。
- 视频天然适合帧级并行；避免算法内部 OpenMP 与外部多帧线程池发生过度订阅。
- 提供线程预算器，统一分配 I/O、demosaic 和 encoder 线程。

---

## 13. 伪色抑制（已移除）

CPU 首版曾实现独立全帧伪色抑制，但正式样本 profile 显示其约占总耗时 60%，
而画质收益不足以支持这一成本。产品路径不再提供 FCS 节点、等级或配置项；伪色表现
由所选 demosaic 算法自身负责。若未来重新评估，必须以新的 ADR、独立质量样片和性能
预算重新进入架构，不能恢复旧的全帧中值实现。

---

## 14. 坏点修复

作为可选 RAW 节点，位于去马赛克之前。

### 两种来源

1. **静态坏点表**
   - 来自用户校准文件或元数据。
   - 最低开销，推荐优先使用。
2. **动态检测**
   - 与同色 CFA 邻居比较。
   - 阈值考虑局部梯度和噪声模型。
   - 区分 hot pixel、dead pixel 和真实高亮小点。

### 视频注意事项

- 动态检测阈值要有时间稳定性，避免坏点 mask 帧间跳变。
- 可缓存稳定检测结果。
- 插值只使用同色 CFA 邻域，防止提前混色。
- 提供坏点 mask 输出和计数统计。

---

## 15. RAW 降噪

### 第一阶段范围

仅做可选的空间 RAW 降噪，不立刻做运动补偿时域降噪。

### 输入模型

优先读取 MCRAW 可用噪声参数；若不存在：

- 用户提供 shot/read noise 参数；
- 或使用设备、ISO/gain 的校准数据库；
- 否则退化为手动强度，不假装是物理噪声模型。

常见方差模型：

\[
\sigma^2(x)=a x+b
\]

其中 \(a\) 表示光子噪声相关项，\(b\) 表示读出噪声相关项。

### 处理规则

- 在 Bayer mosaic 域。
- 仅比较同 CFA 颜色样本，或使用明确的联合模型。
- 保留边缘。
- 降噪强度可为 0。
- 不默认启用强降噪。
- 输出前后噪声统计。

### 后续阶段

可增加多帧时域降噪，但必须独立子项目化：

- 亚像素运动估计；
- 遮挡判断；
- rolling shutter 考虑；
- 运动区域退化为空间降噪；
- 防止拖影和细节“粘连”；
- 时间窗口和延迟管理。

---

## 16. 镜头阴影与几何校正

### 镜头阴影/平场

- 位于 RAW 域。
- 对 CFA 位置分别施加 gain。
- 若 MCRAW 有逐帧或容器级 shading map，先确认字段语义和网格布局，再启用。
- 不得因为字段名相似就猜测。
- 支持外部校准网格。
- 网格插值应可选双线性或更高质量方法。
- 大增益区域应记录噪声放大。

### 几何畸变

- 在 RGB 域。
- 可通过外部 lens profile 系统实现。
- 初期可以不做，接口先预留。
- 处理后需要定义输出尺寸、裁切与边缘填充策略。

### 横向色差

- 可单独放在 RGB 域。
- 若使用 RAW 专用 CA 修正，应作为另一实现，并有测试证明不会破坏 CFA 细节。

---

## 17. MCRAW 元数据标准化

官方示例可读取：

- `width`
- `height`
- 每帧 `asShotNeutral`
- `blackLevel`
- `whiteLevel`
- `sensorArrangement` 或历史拼写
- `colorMatrix1`
- `colorMatrix2`
- `forwardMatrix1`
- `forwardMatrix2`

其他分支或工具还可能出现：

- `calibrationMatrix1`
- `calibrationMatrix2`
- `colorIlluminant1`
- `colorIlluminant2`
- 噪声模型
- lens shading
- crop/active area
- baseline exposure
- per-frame exposure、ISO/gain、时间戳

### 处理要求

1. 保留原始 JSON。
2. 生成 `NormalizedCameraMetadata`。
3. 记录每个标准化字段来源：
   - container
   - frame
   - external override
   - default
4. 检查矩阵：
   - 元素数量；
   - 行列定义；
   - 是否有限数；
   - 行列式/条件数；
   - 是否全零；
   - 是否两个矩阵误填为相同；
   - 是否存在转置风险。
5. 缺失字段不得静默伪造。
6. `inspect` 输出完整诊断。
7. 为不同 MotionCam 版本建立样本库。

---

## 18. 白平衡与双矩阵颜色模型

这部分以 Adobe DNG 1.7.1 的 camera color model 为规范，而不是照搬 vkdt 的具体行为。

### 18.1 基本定义

对每个校准光源有：

- ColorMatrix \(CM_i\)
- CameraCalibration \(CC_i\)，缺失时为单位矩阵
- ForwardMatrix \(FM_i\)，可能缺失
- CalibrationIlluminant \(I_i\)
- AnalogBalance \(AB\)，缺失时为单位对角矩阵

逐帧白平衡通常来自 `asShotNeutral`，记为 `CameraNeutral`。

### 18.2 双矩阵插值规则

DNG 1.2 及以后规定：

- 根据用户白平衡和两个校准光源的相关色温计算权重。
- 在**倒数相关色温**上做线性插值。
- 白平衡超出两个校准光源范围时，夹到最近一个矩阵。
- 分别插值 CM、CC、FM；不能只插值最终 Camera→RGB 矩阵。
- 不应简单根据“相机设置的 Kelvin 数”直接插值，而应从白平衡 neutral 求得 xy/CCT。

权重可概念化为：

\[
w =
\frac{1/T_\mathrm{WB}-1/T_1}
     {1/T_2-1/T_1}
\]

之后夹到 `[0,1]`。矩阵顺序必须按实际低/高色温整理，不能假设 Matrix1 一定是低色温。

### 18.3 CameraNeutral → xy 的迭代

因为插值矩阵依赖白点，而白点又由 CameraNeutral 和矩阵决定，需要迭代：

1. 用一个合理 xy 作为初值。
2. 由当前 xy/CCT 得到双矩阵权重。
3. 插值得到 CM、CC、FM。
4. 计算：
   \[
   XYZtoCamera = AB \cdot CC \cdot CM
   \]
5. 求：
   \[
   XYZ = (XYZtoCamera)^{-1} \cdot CameraNeutral
   \]
6. XYZ 转 xy。
7. 迭代到收敛。

要求：

- FP64。
- 最大迭代次数。
- 明确收敛容差。
- 记录失败原因。
- 矩阵非方阵时使用稳定 pseudo-inverse。
- 对低条件数、异常 neutral 做错误处理。

### 18.4 有 ForwardMatrix 的路径

遵循 DNG：

\[
ReferenceNeutral=(AB\cdot CC)^{-1}\cdot CameraNeutral
\]

\[
D=\mathrm{Invert}(\mathrm{Diagonal}(ReferenceNeutral))
\]

\[
CameraToXYZ_{D50}=FM\cdot D\cdot(AB\cdot CC)^{-1}
\]

ForwardMatrix 已包含 profile 设计者的 D50 映射意图，因此不要再额外做一遍错误的校准光源→D50 适配。

### 18.5 无 ForwardMatrix 的路径

1. 求逆：
   \[
   CameraToXYZ=(AB\cdot CC\cdot CM)^{-1}
   \]
2. 用线性 Bradford 将选定白点适配到 D50：
   \[
   CameraToXYZ_{D50}=CA\cdot CameraToXYZ
   \]

### 18.6 白平衡与曝光必须分离

- 白平衡改变三通道比例。
- 曝光偏移是场景线性统一乘数。
- 不要把绿色归一化、曝光补偿和白平衡混成同一个不可解释的 gain。
- 日志输出中记录：
  - 原始 `asShotNeutral`
  - 求得 xy、CCT、Duv（若计算）
  - 矩阵插值权重
  - 最终相机→XYZ D50 矩阵
  - 曝光偏移

### 18.7 手动白平衡

支持：

- `as-shot`
- CCT + tint
- xy
- 直接 CameraNeutral
- 直接 RGB gains（高级/诊断）

CCT+tint 必须先转换为 xy，再进入统一 DNG 求解路径。不能为不同输入方式维护多套颜色逻辑。

---

## 19. 颜色转换主路径

建议把 `XYZ D50` 作为相机 profile 与输出色域之间的规范中间锚点：

```text
Camera RGB linear
→ DNG CameraToXYZ_D50
→ XYZ D50
→ chromatic adaptation to target white
→ target linear RGB
→ exact target Log OETF
```

大多数目标 Log 色域使用 D65，因此需要从 D50 到 D65 的明确 chromatic adaptation。建议：

- 默认线性 Bradford。
- 适配方法记录在元数据中。
- 不允许隐式将 D50 XYZ 直接乘 D65 RGB matrix。
- 目标 RGB matrix 使用官方公布矩阵；若只有 primaries，则由 FP64 计算并固定测试向量。

对于没有后续编辑节点的纯转码，不必强行绕经 ACEScg。  
若未来加入 RGB 域高级处理，可提供 ACEScg 或其他工作空间，但不能改变基础颜色转换的定义。

---

## 20. “Log 格式”必须定义为色域 + OETF

用户选择项不能只叫 `S-Log3`，而应是明确 profile：

- `FLog_BT2020`
- `SLog2_SGamut`
- `SLog3_SGamut3`
- `SLog3_SGamut3Cine`
- `DaVinciIntermediate_DWG`
- `LogC3_AWG3_EI800`

每个 profile 固定：

1. 目标 RGB primaries。
2. 白点。
3. XYZ↔RGB 矩阵。
4. OETF/EOTF 精确公式。
5. 负值策略。
6. scene-linear 基准。
7. 18% 灰映射。
8. RGB→YCbCr matrix。
9. packing range。
10. QuickTime/FFmpeg 色彩标签策略。
11. sidecar 名称和版本。
12. 官方来源和校验向量。

---

## 21. 精确 Log 实现规则

### 21.1 通用规则

- 使用解析分段公式，不使用 spline 拟合。
- 不用粗 LUT 作为参考实现。
- 可有高精度 LUT 加速模式，但必须与解析公式比较并限制误差。
- CPU 参考使用 FP64。
- GPU 默认 FP32，禁用会改变结果的激进 fast-math。
- 每条曲线都实现 forward 和 inverse。
- 断点两侧做连续性测试。
- 对负输入、NaN、Inf 和极高超白明确处理。
- 生成密集测试网格与官方/ACES CLF 对比。
- 记录公式版本和来源文档版本。

### 21.2 F-Log / BT.2020

使用 Fujifilm 官方分段公式和常数：

- `a=0.555556`
- `b=0.009468`
- `c=0.344676`
- `d=0.790453`
- `e=8.735631`
- `f=0.092864`
- linear cut `0.00089`
- log-domain cut `0.100537775223865`

官方参考：

- 0% 反射约 10-bit 95
- 18% 反射约 470
- 90% 反射约 705
- gamut 为 BT.2020/D65
- 官方资料称 F-Log 使用 full range

实现时把“Log 信号归一化”和“最终 ProRes YUV range packing”分开，不能因为 F-Log 原始相机记录是 full range，就直接假设任意 MOV/ProRes 打包行为。

### 21.3 S-Log3

使用 Sony 官方公式：

- 分段点：scene linear `0.01125`
- 18% 灰应映射到 10-bit code 420
- 0% 黑参考 code 95
- 90% 参考 code 598
- 公式不随 EI 改变

必须支持：

- S-Gamut3
- S-Gamut3.Cine

二者共用 S-Log3 OETF，但 gamut matrix 不同。

### 21.4 S-Log2

S-Log2 必须单独实现并绑定 S-Gamut。

实施规则：

- 不从图表拟合。
- 不从 S-Log3 反推。
- 采用 Sony 官方技术文档、官方 LUT 或经 ACES/OCIO 采用的明确 reference transform。
- 在拿到可追溯的公式/transform 前，profile 标记为 `experimental`，不可声称 bit-exact。
- 至少校验 Sony 参考点：
  - 0% 黑 code 90
  - 18% 灰 code 347
  - 90% 白 code 582
- 记录 S-Log2 版本和对哪类 Sony 实现兼容。

### 21.5 DaVinci Intermediate / DWG

使用 Blackmagic 官方参数：

- `DI_A = 0.0075`
- `DI_B = 7.0`
- `DI_C = 0.07329248`
- `DI_M = 10.44426855`
- `DI_LIN_CUT = 0.00262409`
- `DI_LOG_CUT = 0.02740668`

参考：

- scene-linear 0.18 → 0.336043
- scene-linear 1.0 → 0.513837
- scene-linear 100 → 1.0

使用官方 DWG primaries、白点和公布矩阵，不采用近似 spline。

### 21.6 ARRI LogC3 / AWG3

LogC3 不是单一固定曲线。

其参数取决于：

1. linear domain 是 sensor signal 还是 exposure value；
2. EI。

对于从 MCRAW 场景线性数据生成数字负片，默认采用 **exposure-value** 路径。

设计：

- `logc3_domain = exposure_value | sensor_signal`
- `logc3_ei = 160...1600`，必要时扩展更高 EI 参数
- 默认 `EI800`
- 参数表直接来自 ARRI 官方规范，禁止手抄后无测试
- 18% 灰应映射到约 0.391，即 10-bit 400/1023
- 输出 profile 名称必须包含 EI，例如 `LogC3_AWG3_EI800`

ARRI 明确说明不同 EI 曲线不同；仅为了兼容而固定 EI800可以作为默认，但不能隐藏这一事实。

### 21.7 负值策略

全局提供：

- `preserve_by_curve`：使用曲线官方线性 toe，可编码则保留。
- `clamp_zero`：进入 OETF 前裁到 0。
- `soft_floor`：可配置平滑压缩负值。
- `error`：发现负值即停止，用于验证。

默认按曲线规范。  
不得为了避免 `log()` 非法而无条件静默 clamp。

---

## 22. RGB → YCbCr 与 ProRes 422

这一步必须与 Log OETF 分离。

### 22.1 输出 profile 还需定义 packing profile

例如：

```text
color encoding:
    SLog3_SGamut3Cine

packing:
    ProRes422HQ
    10-bit
    video or full range
    YCbCr matrix identifier
    chroma siting
    chroma filter
```

### 22.2 避免的错误

- 把 Log RGB 的 0–1 直接当作 Y 平面码值。
- 未定义 RGB→YCbCr matrix。
- 把 full-range Log 曲线定义与 MOV YUV range 混为一谈。
- 先做 4:2:2 再做 Log。
- 在场景线性 RGB 上直接做标准 Y'CbCr。
- 使用 nearest/box 作为高质量色度下采样。
- 写错或遗漏色彩标签，却依赖 NLE 自动识别。

### 22.3 4:2:2 下采样

- 在非线性 R'G'B' 转换成 Y'CbCr 后进行。
- 使用明确的低通滤波器。
- 定义水平 chroma siting。
- 处理画面边界。
- 独立测试饱和彩色细线和高频图案。
- 提供 `fast` 与 `quality` 两种滤波器，但默认 `quality`。

### 22.4 量化

- FP32/FP64 到 10-bit 前增加可控 dither。
- 默认使用确定性、固定 seed 或基于帧/坐标的可重复噪声。
- 避免视频帧间固定条纹或随机闪烁。
- 对 legal/full range 分别测试端点。
- 统计裁剪像素比例。

### 22.5 元数据现实限制

QuickTime/ProRes 的标准色彩标签不能完整表达所有“相机 Log + 宽色域”组合。

因此：

1. 尽可能写正确的 `colr`/nclc/nclx 标签。
2. 无法精确表达时使用 `unspecified`，而不是写一个错误标准。
3. 始终写 sidecar JSON。
4. 文件名或 metadata 明确 profile。
5. 在 DaVinci Resolve 中进行导入识别测试。
6. 文档说明需要手动指定 Input Color Space 的情况。

不要假设一个 `color_trc` 标签就足以表示完整摄影机色彩空间。

---

## 23. ProRes 与 MOV 输出

### 编码

首选 FFmpeg `prores_ks`，支持：

- ProRes 422 Proxy
- ProRes 422 LT
- ProRes 422
- ProRes 422 HQ

内部输入：

- `yuv422p10le`

未来可加：

- ProRes 4444
- ProRes 4444 XQ
- 16-bit TIFF/EXR 测试输出

处理架构不应在颜色节点前假定最终一定是 422，以便未来扩展 4444。

### 时间

MCRAW 帧时间戳为事实来源。

提供：

- `source_timestamps`：保留实际时间。
- `cfr`：按指定帧率重建，明确处理缺帧。
- `drop/duplicate/error` 策略。

默认先尝试保留源时间关系，但需验证 NLE 对 MOV 时间基的兼容性。

### 音频

- 保留采样率、声道数和时间戳。
- 输出 PCM。
- 不以第一帧简单假定音频起点。
- 统计 A/V 起点差和结束差。
- 提供静音补齐或裁切策略。
- 输出同步报告。

---

## 24. 性能架构

### 24.1 目标流水线

```text
async read / prefetch
    ↓
compressed frame ring
    ↓
Vulkan RAW unpack
    ↓
RAW processing
    ↓
demosaic
    ↓
color + log
    ↓
RGB/YUV + 4:2:2
    ↓
CPU ProRes encoding
    ↓
async mux/write
```

在稳态时：

- GPU 处理帧 N+1；
- CPU 编码帧 N；
- I/O 读取帧 N+2；
- 磁盘写入更早的 packet。

### 24.2 I/O

- 使用 `pread`、memory mapping 或独立句柄，避免共享 seek 锁。
- 根据 frame index 直接随机读取。
- 支持预取。
- 压缩帧缓冲使用有限 ring，防止内存无限增长。
- 记录读取、等待和 page fault 时间。

### 24.3 Vulkan

- 持久映射 staging buffer。
- timeline semaphore。
- 多帧 in-flight。
- 尽量让压缩数据直接进入 GPU 可访问缓冲。
- 避免解压出完整 CPU Bayer 后再次上传。
- 每个节点独立 timestamp query。
- shader 使用明确精度。
- 正确性阶段不启用 `fast-math`。
- 先保留独立 kernel，后续依据 profiler 决定融合。

### 24.4 CPU 高品质去马赛克的现实问题

若 GPU 完成 RAW 解压而 AMaZE/RCD/IGV 仍在 CPU，会发生 GPU→CPU 读回。

因此分阶段：

1. **正确性阶段**：CPU 解压 + CPU demosaic。
2. **GPU 解压验证阶段**：GPU 解压后读回，只用于测试。
3. **混合生产阶段**：评估 host-visible buffer 和读回是否仍有收益。
4. **完整 GPU 阶段**：至少将常用 RCD 移植到 Vulkan。
5. AMaZE/IGV 可继续作为较慢 CPU“最高质量”选项，直到 GPU 版本验证完成。

不要为了避免读回而过早移植复杂算法；错误 GPU demosaic 的代价大于暂时较慢。

### 24.5 编码瓶颈

GPU 图像处理加速后，`prores_ks` 很可能成为瓶颈。因此 benchmark 必须分别测：

- MCRAW I/O
- CPU 解压
- GPU 解压
- 黑白场
- 坏点
- RAW NR
- demosaic
- color/matrix
- Log
- RGB→YUV
- chroma filter
- ProRes
- mux/write

只报告端到端 FPS 不足以指导优化。

---

## 25. 配置系统

建议 JSON 或 YAML；内部有版本号和 schema 校验。

### 顶层概念

- input
- timing
- raw calibration
- bad pixel
- lens shading
- denoise
- demosaic
- white balance
- camera profile
- target color encoding
- output packing
- codec
- audio
- performance
- diagnostics

### 原则

- 所有默认值都能在 `print-effective-config` 中显示。
- preset 展开后就是普通配置。
- 配置写入 sidecar。
- 非法组合在运行前失败。
- 输入元数据和 override 的优先级固定且可见。
- 配置 schema 带版本，未来可迁移。

---

## 26. CLI 功能规划

### `inspect`

输出：

- MCRAW/container 版本
- 帧数、时间戳、帧率统计
- 分辨率、CFA
- compression type
- black/white levels
- matrices
- illuminants
- as-shot neutral 范围
- 音频
- 可选元数据
- 字段来源和异常

### `convert`

执行转码。

### `extract-frame`

输出指定阶段：

- compressed payload
- raw U16
- normalized Bayer
- linear RGB
- XYZ D50
- target linear RGB
- target Log RGB
- YUV

### `validate`

- CPU 官方解压 vs 自定义 CPU/GPU
- 矩阵和曲线
- 选定帧逐像素比较
- 输出差异热图和统计

### `benchmark`

- warm-up
- 每阶段时间
- P50/P95/P99
- 吞吐 FPS
- CPU/GPU 利用率
- 内存/显存峰值
- 队列等待比例

### `list-capabilities`

列出：

- 可用 demosaic 算法
- CPU/GPU backend
- 可用 Log profile
- FFmpeg encoder
- 支持 CFA
- build license/features

---

## 27. 错误处理与回退

### 必须停止的错误

- 帧索引越界或损坏。
- 压缩 payload 长度不一致。
- GPU 解压与参考算法不一致。
- CFA 未知。
- 矩阵维度错误、NaN/Inf、不可接受的条件数。
- 双矩阵有矩阵但无可解释 illuminant。
- 目标 profile 缺失必要 gamut/OETF 定义。
- 编码器返回损坏 packet。
- 音视频时间戳倒退。

### 可警告回退

- 缺少 ForwardMatrix：使用 ColorMatrix + Bradford。
- 缺少 CameraCalibration：单位矩阵。
- 缺少 AnalogBalance：单位矩阵。
- 仅一个 ColorMatrix：单光源模式。
- 缺少噪声模型：关闭物理 RAW NR。
- 缺少镜头 shading 数据：关闭该节点。
- GPU 不可用：CPU backend。
- 某 demosaic 无 GPU 实现：CPU 或用户指定 fallback。

所有回退写入日志和 sidecar，不能静默。

---

## 28. 测试体系

### 28.1 MCRAW 解压

- compression 6/7 等实际版本样本。
- 所有 CFA。
- 不同宽度、非典型 stride。
- 首帧、末帧、随机帧。
- CPU 官方结果和自定义结果必须 bit-exact。
- GPU 结果必须 bit-exact。
- 异常/截断文件应安全失败。

### 28.2 元数据

- 历史字段拼写。
- 缺失矩阵。
- 单矩阵/双矩阵。
- ForwardMatrix 有/无。
- 每帧白平衡变化。
- 异常 matrix。
- 元数据读取结果保存为 golden JSON。

### 28.3 去马赛克

建立素材集：

- Siemens star
- zone plate
- 黑白细线
- 斜线与文字
- 织物
- 叶片
- 屋顶瓦片
- 远处栅栏
- 饱和红蓝 LED
- 肤色
- 低照高噪
- 单通道剪切高光
- 坏点
- 图像边界

比较：

- AMaZE
- RCD
- IGV
- RawTherapee/librtprocess 参考
- DaVinci CinemaDNG 结果

指标不能只用 PSNR；还需观察假色、拉链、摩尔纹、时间闪烁和边缘形状。

### 28.4 双矩阵颜色

单元测试：

- 已知 identity profile。
- 已知合成 ColorMatrix。
- 校准光源端点权重应精确为 0/1。
- 中间色温按 inverse CCT。
- 超范围 clamp。
- CameraNeutral→xy 迭代收敛。
- 有/无 ForwardMatrix 两条路径。
- neutral 变换到 D50 后应中性。
- 与 Adobe DNG SDK 或受信参考实现比较。
- 由相同 MCRAW 生成 DNG，在 Adobe/Resolve 中做参考比较。

### 28.5 Log 曲线

对每条曲线：

- forward/inverse round-trip。
- 断点前、断点、断点后。
- 0、18%、90%、1.0 和多个超白。
- 负值。
- 密集 FP64 网格。
- GPU FP32 vs CPU FP64。
- 官方 reference code value。
- ACES CLF/OCIO reference 对比。
- 目标：在量化前误差足够低，使已知 10-bit 点不超过 0.5 code value；参考点四舍五入结果精确一致。

### 28.6 YUV/ProRes

- 灰阶 ramp。
- 饱和 RGB patch。
- 单像素/双像素彩色线。
- full/video range。
- chroma siting。
- 码值端点。
- FFmpeg decode round-trip。
- DaVinci Resolve 导入。
- 检查 waveform、RGB parade、metadata 和手动 input color space。
- ProRes profile/bit depth/tag 验证。

### 28.7 视频稳定性

- 静止场景检查 demosaic 伪色与时间闪烁。
- 坏点 mask 稳定性。
- RAW NR 稳定性。
- 帧时间戳和音频同步。
- 长片内存泄漏。
- 中止和恢复。
- 输出损坏处理。

---

## 29. 性能验收方法

每次 benchmark 固定：

- 输入文件 checksum。
- CPU/GPU/驱动版本。
- 编译器和 flags。
- backend。
- 算法配置。
- 线程数。
- 输出盘。
- warm-up。
- 是否写文件。
- 是否启用音频。
- 分辨率和帧数。

报告：

| 阶段 | ms/frame | FPS 等价 | CPU% | GPU% | 内存/显存 | 等待 |
|---|---:|---:|---:|---:|---:|---:|

提供三种基准：

1. `compute-only`：不编码、不写盘。
2. `encode-only`：输入预生成 YUV。
3. `end-to-end`：完整 MCRAW→MOV。

---

## 30. 实现阶段建议

### Phase 0：规范冻结和样本

- 建立测试 MCRAW 集。
- 固定官方 decoder commit。
- 固定 DNG、Sony、Fujifilm、Blackmagic、ARRI 规范版本。
- 决定项目许可证。
- 建立 golden metadata 和 raw frame。
- 写 architecture decision records。

### Phase 1：I/O 和参考解压

- `inspect`
- frame/audio index
- CPU official decode adapter
- raw U16 导出
- 时间戳和音频读取
- 异常文件测试

验收：所有样本可稳定解析，官方 raw 可复现。

### Phase 2：CPU 高质量正确路径

- 黑/白场
- CFA/crop
- RCD
- AMaZE
- IGV
- DNG 双矩阵
- XYZ D50
- 首个精确输出 profile：建议 DWG/DaVinci Intermediate
- ProRes 422 HQ
- sidecar

验收：单帧颜色、曲线和 ProRes round-trip 正确。

### Phase 3：完整 Log profile

- F-Log
- S-Log3 两种 gamut
- LogC3 EI 参数
- S-Log2 reference 确认后加入
- 颜色/packing metadata
- Resolve 对照项目

### Phase 4：Vulkan MCRAW 解压

- 参考 vkdt 算法结构
- compressed payload 直达 GPU
- prefix/offset
- decode shader
- bit-exact 自动验证
- 多帧 ring

验收：所有测试帧与官方 CPU 完全一致。

### Phase 5：GPU 颜色和输出准备

- black/white
- matrix
- chromatic adaptation
- exact OETF
- RGB→YUV
- quality 422
- dither/quantization
- 异步 read/compute/encode/write

### Phase 6：可选 RAW 修复

- 坏点
- lens shading
- RAW spatial NR
- highlight reconstruction
- CA/geometry 接口

每项独立 benchmark 和质量回归。

### Phase 7：GPU 高品质 demosaic

优先顺序建议：

1. RCD
2. IGV 或 AMaZE，依据前期 profile 结果决定

GPU 算法必须逐像素或按明确容差与 CPU reference 比较。

### Phase 8：产品化

- 稳定 preset
- 配置 schema 迁移
- 崩溃恢复
- 日志
- 批处理
- GUI 作为 CLI/library 的薄前端

---

## 31. Codex 工作拆分原则

不要让 Codex 一次生成“完整转码器”。每个任务应具有：

- 一个模块边界。
- 一个输入/输出契约。
- 一个对应测试。
- 一个完成标准。
- 不跨越多个尚未验证的颜色阶段。

### 建议任务粒度

1. 建项目骨架和依赖锁定。
2. 封装 MotionCam decoder。
3. 输出标准化 metadata。
4. 做 CPU raw golden test。
5. 定义像素类型和 frame buffer。
6. 实现 black/white reference node。
7. 适配单个 demosaic。
8. 加另外两个 demosaic。
9. 建 CPU 线程预算和有界多帧执行接口。
10. 实现 DNG neutral→xy 迭代。
11. 实现双矩阵插值。
12. 实现 ForwardMatrix 路径。
13. 实现 XYZ D50→DWG。
14. 实现 DI exact OETF。
15. FFmpeg ProRes 单帧编码。
16. MOV 和时间戳。
17. 音频。
18. 端到端 CPU CLI。
19. Vulkan device/context。
20. GPU compressed frame input。
21. GPU decode bit-exact。
22. GPU color/log。
23. GPU YUV422。
24. pipeline overlap。
25. optional correction nodes。

每次只要求 Codex 修改有限文件，并让测试先失败、后通过。

---

## 32. 架构决策记录建议

在仓库建立 `docs/adr/`：

- ADR-001 License posture
- ADR-002 Official MotionCam decoder role
- ADR-003 Vulkan as primary GPU API
- ADR-004 XYZ D50 as interchange anchor
- ADR-005 DNG dual-illuminant algorithm
- ADR-006 Demosaic plugin ABI
- ADR-007 Exact analytic Log functions
- ADR-008 ProRes via FFmpeg
- ADR-009 YUV range and chroma siting
- ADR-010 Timestamp policy
- ADR-011 Negative linear value policy
- ADR-012 CPU/GPU determinism and tolerance

每个 ADR 包含：背景、选择、替代方案、后果、测试方式。

---

## 33. 首版默认值建议

这些默认值可在测试后更改：

| 项目 | 暂定默认 |
|---|---|
| black/white | metadata |
| bad pixel | off，静态 map 有效时可 on |
| lens shading | off，除非可靠 metadata/profile |
| RAW NR | off |
| demosaic | RCD（暂定，等待测试） |
| white balance | as-shot |
| color transform | DNG dual-illuminant + ForwardMatrix 优先 |
| target | DWG / DaVinci Intermediate |
| exposure offset | 0 |
| negative policy | preserve_by_curve |
| chroma filter | quality |
| dither | on, deterministic |
| ProRes | 422 HQ |
| timing | source timestamps |
| audio | PCM passthrough/repack |
| backend | auto，reference 可强制 CPU |
| precision | FP32 pixels + FP64 setup/reference |

对输出数字负片，默认关闭降噪和几何校正更中性；黑白场和颜色校准默认开启，因为它们属于正确解释传感器数据的基础步骤。

---

## 34. 仍需在开始前确认的事项

1. 项目采用 GPLv3 还是宽松许可证。
2. 是否允许直接链接 librtprocess。
3. 第一目标平台是否为 Windows + NVIDIA。
4. Vulkan 是否唯一 GPU backend，还是未来加 CUDA。
5. 首版只做 ProRes 422 HQ，还是同时做 Standard/LT。
6. 是否需要 ProRes 4444 作为颜色验证和高质量备选。
7. 对 Log YUV packing 的默认 range 和 matrix 需通过 Resolve 实测最终确定。
8. S-Log2 的权威 reference transform 需要单独收集和固化。
9. MCRAW 中 lens shading、noise model、baseline exposure 等字段需用真实样本审计。
10. MotionCam 不同版本、不同手机、不同镜头的矩阵和元数据兼容性需要样本库。

---

## 35. 关键风险

| 风险 | 应对 |
|---|---|
| 误解 MCRAW 版本差异 | 官方 decoder 基准 + 多版本样本 |
| GPU RAW 解压非 bit-exact | 自动逐帧比较，失败禁止生产输出 |
| 高品质 demosaic 许可冲突 | 开工前决定许可证 |
| 双矩阵实现错误 | 按 DNG 规范 + Adobe/DNG 对照 |
| ForwardMatrix 重复色适配 | 明确两条 DNG 路径 |
| Log 曲线近似 | 解析公式 + 官方测试向量 |
| Log 与 gamut 名称混淆 | profile 必须成对命名 |
| full range 与 ProRes range 混淆 | 分离 encoding 和 packing |
| 4:2:2 造成额外假色 | 高质量滤波 + 4444 测试输出 |
| ProRes 变成瓶颈 | encode-only benchmark |
| CPU demosaic 导致 GPU 读回 | 分阶段实现，优先 GPU RCD |
| 视频帧间闪烁 | 静止序列回归测试 |
| 元数据自动识别不可靠 | sidecar + Resolve 手动 profile 测试 |

---

## 36. 参考资料

### MCRAW

- MotionCam official decoder  
  https://github.com/mirsadm/motioncam-decoder
- MotionCam decoder example showing matrices and AsShotNeutral  
  https://github.com/mirsadm/motioncam-decoder/blob/main/example.cpp
- vkdt  
  https://github.com/hanatos/vkdt

### Demosaic

- librtprocess  
  https://github.com/CarVac/librtprocess
- RawTherapee  
  https://github.com/Beep6581/RawTherapee

### DNG color model

- Adobe DNG Specification 1.7.1.0  
  https://helpx.adobe.com/content/dam/help/en/camera-raw/digital-negative/jcr_content/root/content/flex/items/position/position-par/download_section_733958301/download-1/DNG_Spec_1_7_1_0.pdf

重点章节：

- One, Two, or Three Color Calibrations
- inverse correlated color temperature interpolation
- CameraNeutral to white-balance xy
- Camera to XYZ D50
- ForwardMatrix path

### Log and gamut specifications

- Blackmagic DaVinci Wide Gamut / Intermediate  
  https://documents.blackmagicdesign.com/InformationNotes/DaVinci_Resolve_17_Wide_Gamut_Intermediate.pdf
- Fujifilm F-Log Data Sheet  
  https://dl.fujifilm-x.com/support/lut/F-Log_DataSheet_E_Ver.1.2.pdf
- Sony S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3 Technical Summary  
  https://pro.sony/s3/cms-static-content/uploadfile/06/1237494271406.pdf
- ARRI LogC3 specification/downloads  
  https://www.arri.com/en/learn-help/learn-help-camera-system/technical-downloads
- ARRI ALEXA Log C Curve in VFX  
  https://www.arri.com/resource/blob/31918/66f56e6abb6e5b6553929edf9aa7483e/2017-03-alexa-logc-curve-in-vfx-data.pdf
- OpenColorIO Config ACES reference transforms  
  https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES

---

## 37. 给 Codex 的最高层指令摘要

1. 不要一次实现整个应用。
2. 不允许猜测 MCRAW 字段或颜色公式。
3. 官方 MotionCam CPU decoder 是 RAW 解压真值。
4. Adobe DNG 规范是双矩阵颜色真值。
5. 相机 neutral 求白点需要迭代。
6. 双矩阵使用 inverse CCT 权重。
7. 有 ForwardMatrix 与无 ForwardMatrix 是不同路径。
8. Log 必须使用官方解析公式，不使用 spline。
9. Log 曲线与 RGB gamut 必须成对定义。
10. 色彩 encoding 与 YUV packing 必须分离。
11. 不提前 clamp 负值和超白。
12. 高品质 demosaic 是可插拔模块。
13. 不提供独立 FCS；伪色质量由 demosaic 选择和回归样片负责。
14. 黑白场默认开启；坏点、镜头和 RAW NR 默认可关闭。
15. 任何 GPU 优化都要有 CPU reference 和自动比较。
16. FFmpeg 负责 ProRes 和 MOV。
17. 每个阶段必须有单元测试、黄金数据和 benchmark。
18. 所有 fallback、override 和近似都必须记录到日志与 sidecar。
