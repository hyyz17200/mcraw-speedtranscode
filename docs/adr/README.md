# v0.1 Architecture Decision Record

状态：2026-07-13 已冻结，用于 Phase 0～2。

| ADR | 决策 | 后果与验证 |
|---|---|---|
| 001 License posture | 整体采用 GPLv3-or-later | 允许直接静态/动态链接 GPLv3 librtprocess；分发必须履行 GPL |
| 002 Official decoder | MotionCam decoder `release/0.2` 是 compression 6/7 CPU RAW 真值 | 自定义索引只暴露压缩 payload；RAW 解压必须调用官方实现 |
| 003 GPU API | 后续主 GPU API 为 Vulkan；v0.1 不包含 GPU | 任何未来 GPU 解压必须逐像素 bit-exact |
| 004 Color anchor | 相机 profile 与输出 profile 之间使用 XYZ D50 | ForwardMatrix 与无 ForwardMatrix 路径分开测试 |
| 005 Dual illuminant | CameraNeutral→xy 迭代；矩阵按 inverse CCT 插值 | FP64、50 次上限、`1e-10` xy 收敛阈值 |
| 006 Demosaic ABI | 高质量 demosaic 通过明确 enum/函数边界调用 librtprocess | RCD 默认；AMaZE/IGV/DCB/LMMSE 可替换；不附加独立 FCS |
| 007 Log | 解析公式是真值；生产路径由公式生成每转换实例 LUT | v0.1 只冻结 DaVinci Intermediate；最终 10-bit 输出与参考最大差异 1 LSB |
| 008 ProRes | 使用 FFmpeg libavcodec/libavformat `prores_ks` | 不实现 ProRes 码流；输入为 yuv422p10le |
| 009 Packing | video range、BT.2020 NCL、left siting、quality 5-tap | primaries/TRC 标 unspecified；Resolve 实测后再冻结产品默认 |
| 010 Timing | 默认保留逐帧源时间关系 | 以纳秒源时钟为事实来源，转换到 90 kHz MOV video time base |
| 011 Negative values | 默认 `preserve_by_curve` | DI linear toe 保留负值；只在 YUV 量化边界裁剪 |
| 012 Determinism | CPU FP32 pixels、FP64 setup/reference、确定性 dither | 默认禁用隐藏近似；每阶段独立计时 |
| 013 FCS removal | 不实现独立伪色抑制 | 当前全帧中值实现收益不足且占原耗时约 60%；依赖 demosaic 本身质量 |
| 014 CPU execution | 总线程预算 + RAM 感知的有界多帧并行 | 默认 `0=auto`；有序收集后再按源时间戳 mux，避免乱序输出 |
| 015 Detail processing boundary | 线性 DWG Capture Sharpening 默认 `0.4`；转码器不做降噪 | 锐化补偿采集与解码链路的细节损失；噪声处理留给后期调色 |
| 016 FFmpeg ProRes GPU-AV waiver | 对 pinned FFmpeg 8.1.2 `dct.glsl` 的同 invocation shared-memory race 诊断给予受限发布豁免 | 仅覆盖已记录的 143/167 行诊断；core VUID、应用 shader 诊断或输出失败不在豁免内；升级 FFmpeg/validation layer 时必须复测 |

## MotionCam 缺失 illuminant 的兼容规则

官方 decoder 示例把 `ColorMatrix1` 写为 DNG illuminant 21（D65），把
`ColorMatrix2` 写为 illuminant 17（Standard Light A）。当 MCRAW 同时包含矩阵但
缺少 illuminant 字段时，v0.1 使用这一官方兼容约定，并在 `inspect`、日志与
sidecar 中明确记录 warning；不会静默伪造。
