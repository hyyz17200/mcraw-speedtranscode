# Windows Vulkan ProRes GPU Pipeline Implementation Guide

> 面向 Codex / 开发代理的工程实施指南  
> 目标：在已有稳定 CPU 转码管线旁边，新建一条可验证、可回退、尽可能 GPU-resident 的 Vulkan ProRes 管线。  
> 平台：Windows 10/11，NVIDIA/AMD/Intel Vulkan 驱动，FFmpeg/libav*。  
> 首要编码器：`prores_ks_vulkan`。  
> 原则：**不要破坏现有 CPU pipeline；GPU pipeline 必须作为独立后端逐步落地。**

---

## 0. 给 Codex 的最高优先级指令

1. 先阅读并理解现有 CPU pipeline，再写代码。
2. 禁止原地重写稳定 CPU 路径。
3. 新增抽象层，使 CPU 与 GPU 后端可以并存：
   - CPU 后端继续作为参考实现与自动回退。
   - GPU 后端通过 feature flag / runtime capability probe 启用。
4. GPU pipeline 的最终目标不是“CPU 帧上传后调用 GPU 编码器”，而是：

   ```text
   RAW/MCRAW decode
       -> Vulkan unpack / normalization
       -> Vulkan debayer
       -> Vulkan image processing
       -> Vulkan color transform / Log encoding
       -> Vulkan RGB-to-YUV 4:2:2 10-bit
       -> AV_PIX_FMT_VULKAN frame
       -> prores_ks_vulkan
       -> CPU-visible compressed packet
       -> MOV muxer / file writer
   ```

5. 每个阶段都必须能单独验证：
   - 数值正确性
   - Vulkan synchronization
   - 图像 layout
   - 颜色元数据
   - 内存生命周期
   - 性能
6. 未通过 bitstream、画质和稳定性测试前，不允许将 GPU pipeline 设为默认。
7. 所有失败必须能够：
   - 给出可诊断日志；
   - 安全释放 Vulkan/FFmpeg 资源；
   - 自动或显式回退 CPU pipeline；
   - 不留下损坏或伪装完整的输出文件。

---

## 1. 当前技术前提与版本策略

### 1.1 不要仅依赖 FFmpeg 版本号

`prores_ks_vulkan` 已存在于 FFmpeg 当前主线源码和 Doxygen 中，但不同正式发行包、Windows 第三方构建和分支可能没有启用或尚未包含该编码器。

启动时必须执行运行时能力检查，至少验证：

```bash
ffmpeg -hide_banner -encoders | findstr /I prores
ffmpeg -hide_banner -h encoder=prores_ks_vulkan
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -init_hw_device list
```

代码内必须验证：

```cpp
const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks_vulkan");
if (!codec) {
    // GPU ProRes backend unavailable
}
```

同时检查构建配置和 Vulkan 支持，不要假定系统中任意名为 `ffmpeg.exe` 的程序都满足要求。

### 1.2 推荐依赖策略

短期开发：

- 固定到一个经过验证的 FFmpeg Git commit；
- 保存 commit hash、configure 参数、编译器版本；
- Windows 构建产物与项目一起做可复现版本管理；
- 不跟踪浮动的 `master` 作为生产依赖。

正式发布：

- 使用明确版本或 vendored FFmpeg build；
- 启动日志打印：
  - `av_version_info()`
  - libavcodec/libavutil/libavformat 版本
  - Vulkan API 版本
  - GPU 名称、vendor ID、device ID、driver version
  - 编码器名称和 profile
  - 是否启用了 GPU-resident 路径或 upload bridge 路径

### 1.3 编码器性质

`prores_ks_vulkan` 是 Vulkan Compute 编码，不是 NVENC，也不是 Vulkan Video 固定功能 ProRes 编码。

因此：

- 使用 GPU 通用计算资源；
- 会与 Vulkan debayer、降噪、缩放和色彩转换争用 shader/带宽；
- 不能通过 NVENC 利用率判断其负载；
- 不受 NVENC session 数限制；
- 应使用 Vulkan timestamp queries、GPUView、Nsight Graphics/Systems、RenderDoc 等分析。

---

## 2. 成功标准

GPU pipeline 只有同时满足以下条件才算完成。

### 2.1 功能正确

- 可输出有效 MOV/ProRes 文件；
- Resolve、Premiere、FFmpeg、ffprobe 至少三方可正常读取；
- profile、分辨率、帧率、time base、色彩元数据正确；
- frame count、PTS/DTS、duration 正确；
- 不丢首帧、不丢尾帧；
- flush 正确；
- odd dimensions 或不支持尺寸有明确错误；
- cancel 后文件处理符合既定策略。

### 2.2 画质正确

与 CPU reference pipeline 比较：

- debayer 前后黑白电平一致；
- white balance、CFA 顺序和矩阵一致；
- RGB、YUV range 与 transfer function 一致；
- chroma siting 明确；
- 4:2:2 下采样滤波明确；
- 10-bit 量化、rounding、clamp 和 dithering 明确；
- 不出现系统性通道交换、偏色、legal/full range 错配；
- HDR/Log 数据不被意外裁剪。

### 2.3 性能正确

必须单独报告：

```text
decode fps
GPU processing fps
RGB->YUV fps
encode fps
mux/write fps
end-to-end fps
GPU utilization
VRAM usage
CPU utilization
PCIe upload/download throughput
disk write throughput
pipeline latency
```

理想路径：

- 未压缩图像不回到 system memory；
- CPU 只接收压缩后的 `AVPacket`；
- GPU 队列和编码器存在多帧 in-flight；
- 编码阶段不是单帧提交后立即等待。

### 2.4 稳定性正确

至少完成：

- 10 秒短片循环 100 次；
- 10 分钟素材；
- 1 小时素材；
- 多文件批处理；
- 取消与重启；
- GPU device lost 模拟或实际恢复测试；
- 不同 profile；
- 多分辨率；
- NVIDIA 至少两代驱动测试；
- Debug validation layer 0 error；
- Release 模式无资源增长。

---

## 3. 总体架构

不要让编码器直接依赖 MCRAW reader、GUI 或具体 debayer 实现。

推荐模块边界：

```text
InputReader
  -> RawFrame / RawFrameView

IDecodeBackend
  -> CpuDecodeBackend
  -> VulkanDecodeBackend

IImagePipeline
  -> CpuImagePipeline
  -> VulkanImagePipeline

IFrameConverter
  -> CpuRgbToYuvConverter
  -> VulkanRgbToYuvConverter

IVideoEncoder
  -> CpuProResEncoder
  -> VulkanProResEncoder

IMuxer
  -> MovMuxer

PipelineScheduler
  -> queues
  -> backpressure
  -> cancellation
  -> error propagation
  -> telemetry
```

### 3.1 统一帧描述

不要用一个含大量 nullable 字段的“万能 Frame”。

建议使用 tagged variant：

```cpp
enum class FrameStorage {
    Cpu,
    Vulkan
};

struct FrameMetadata {
    int width;
    int height;
    int64_t pts;
    AVRational timeBase;

    AVColorPrimaries primaries;
    AVColorTransferCharacteristic transfer;
    AVColorSpace matrix;
    AVColorRange range;
    AVChromaLocation chromaLocation;
};

struct CpuVideoFrame {
    FrameMetadata meta;
    AVPixelFormat format;
    AVFramePtr frame;
};

struct VulkanVideoFrame {
    FrameMetadata meta;
    AVPixelFormat format;        // AV_PIX_FMT_VULKAN
    AVFramePtr frame;            // owns/reference-counts AVVkFrame
    AVPixelFormat swFormat;      // logical software representation
};

using VideoFrame = std::variant<CpuVideoFrame, VulkanVideoFrame>;
```

所有权必须明确：

- `AVFrame` 使用 RAII；
- 禁止将裸 `VkImage` 生命周期独立于所属 `AVBufferRef/AVFrame`；
- 编码器异步持有帧期间，生产方不得复用或销毁资源；
- frame pool 必须考虑 `async_depth` 和前后处理 in-flight 数量。

---

## 4. 迁移路线：从稳定 CPU pipeline 到 GPU pipeline

## Phase 0：冻结 CPU reference

在写 GPU 代码之前：

1. 为当前 CPU pipeline 建立固定测试素材；
2. 保存输出：
   - frame count
   - 每帧 PTS
   - ffprobe JSON
   - 若干关键帧的无损图像或 raw buffer
   - waveform/statistics
   - hash
3. 记录当前 CPU 速度和内存占用；
4. 修复 CPU pipeline 中任何尚未定义的颜色行为；
5. 禁止用 GPU 实现“顺便修正”CPU 输出，除非建立新版本化行为。

CPU 输出是迁移过程中的 reference，不一定是绝对真值，但必须可复现。

## Phase 1：只建立 Vulkan runtime

先不编码。

完成：

- Vulkan instance；
- physical device selection；
- logical device；
- compute queue selection；
- command pools；
- descriptor pools；
- pipeline cache；
- synchronization primitives；
- debug messenger；
- device telemetry；
- deterministic teardown。

设备选择必须支持：

- 用户指定 GPU index、PCI ID 或名称；
- 默认选择合适的 discrete GPU；
- 记录实际选择；
- 不要默默选择 Microsoft Basic Render Driver；
- 多 GPU 系统不要依赖枚举顺序永久稳定。

验收：

- 空 pipeline 连续 init/destroy 1000 次；
- validation layer 无错误；
- 多 GPU 切换正确；
- device creation failure 能回退 CPU。

## Phase 2：建立 FFmpeg Vulkan device/context

优先选择一个 Vulkan device owner。

### 推荐方案

让应用拥有 Vulkan device，然后将其安全接入 FFmpeg；或者让 FFmpeg 创建 device，应用从 `AVVulkanDeviceContext` 使用同一设备。二者选一，禁止长期维持两个互不知情的 Vulkan device 再尝试临时复制。

选择标准：

- 如果现有 GPU pipeline 已有成熟 Vulkan runtime：应用应为 owner；
- 如果项目尚无 Vulkan 代码，只想先验证编码器：可先让 FFmpeg 创建 device；
- 最终目标是处理与编码共享同一个 logical device 和兼容的 queue families。

需要理解：

```cpp
AVBufferRef* hw_device_ctx;   // AVHWDeviceContext
AVBufferRef* hw_frames_ctx;   // AVHWFramesContext
```

典型概念流程：

```cpp
av_hwdevice_ctx_create(
    &hw_device_ctx,
    AV_HWDEVICE_TYPE_VULKAN,
    device_selector,
    options,
    0
);

AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);

frames->format = AV_PIX_FMT_VULKAN;
frames->sw_format = chosen_software_format;
frames->width = width;
frames->height = height;
frames->initial_pool_size = pool_size;

av_hwframe_ctx_init(frames_ref);
```

注意：

- 实际支持的 `sw_format` 必须运行时查询和验证；
- 不要从其他硬件编码器示例机械复制 NV12；
- ProRes 422 10-bit 所需逻辑格式必须与编码器实际接受的 Vulkan frame format 匹配；
- 所有错误都输出完整 FFmpeg error string。

验收：

- 可分配 `AV_PIX_FMT_VULKAN` frame；
- 可查询 `AVVkFrame`；
- 可执行一次 software frame -> hardware frame transfer；
- 可安全释放；
- 此阶段即使很慢也只用于验证桥接。

## Phase 3：最小可用 GPU encoder，先走 upload bridge

该阶段不是最终性能方案，只为隔离编码器集成问题。

临时路径：

```text
existing CPU pipeline
  -> CPU 10-bit 4:2:2 frame
  -> av_hwframe_transfer_data / explicit upload
  -> AV_PIX_FMT_VULKAN
  -> prores_ks_vulkan
  -> packet
  -> MOV
```

目的：

- 验证 encoder open；
- 验证 profile/options；
- 验证 hardware frame format；
- 验证 send/receive loop；
- 验证 muxing；
- 验证 output compatibility；
- 建立 GPU 与 CPU ProRes 对比样本。

不要把此阶段的速度当作最终结论。

### 编码器初始化

概念代码：

```cpp
const AVCodec* codec =
    avcodec_find_encoder_by_name("prores_ks_vulkan");

AVCodecContext* enc = avcodec_alloc_context3(codec);

enc->width = width;
enc->height = height;
enc->time_base = time_base;
enc->framerate = frame_rate;
enc->pix_fmt = AV_PIX_FMT_VULKAN;
enc->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);

enc->color_primaries = primaries;
enc->color_trc = transfer;
enc->colorspace = matrix;
enc->color_range = range;
enc->chroma_sample_location = chroma_location;

// Set profile and Vulkan encoder private options via av_opt_set*.
// Every option must be checked against the exact vendored FFmpeg build.

int err = avcodec_open2(enc, codec, &options);
```

不要硬编码未经运行时验证的 private option 名称。初始化时打印：

```bash
ffmpeg -h encoder=prores_ks_vulkan
```

的等价能力信息，或在构建时为当前 FFmpeg commit 固定 option mapping。

### 正确的 send/receive 循环

```cpp
int send_frame(AVFrame* frame) {
    int ret = avcodec_send_frame(enc, frame);

    if (ret == AVERROR(EAGAIN)) {
        drain_packets();
        ret = avcodec_send_frame(enc, frame);
    }

    if (ret < 0) {
        return ret;
    }

    return drain_packets();
}

int drain_packets() {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(enc, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return 0;
        }

        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }

        // rescale timestamps if required
        // write through muxer
        av_packet_free(&pkt);
    }
}
```

flush：

```cpp
avcodec_send_frame(enc, nullptr);
while (avcodec_receive_packet(enc, pkt) >= 0) {
    write_packet(pkt);
}
```

不要假定“一帧输入立即产生一个 packet”。

验收：

- 10 秒测试片完整编码；
- frame count 和 PTS 正确；
- CPU upload 版 GPU 编码器可稳定运行；
- 输出可由多个软件读取；
- validation layer 无错误。

## Phase 4：把 RGB->YUV 4:2:2 10-bit 搬到 Vulkan

这是最关键的颜色正确性阶段。

不要让 ProRes encoder 隐式承担未定义的颜色转换。GPU image pipeline 应明确输出编码器需要的逻辑格式。

### 明确以下数学定义

1. 输入 RGB 工作空间；
2. transfer function；
3. RGB primaries；
4. YCbCr matrix coefficients；
5. full/legal range；
6. 10-bit code value mapping；
7. chroma downsample kernel；
8. chroma sample location；
9. rounding；
10. dithering。

### 推荐 shader 分层

```text
Shader A: RAW normalization / black-white level
Shader B: debayer
Shader C: WB / camera matrix / gamut transform
Shader D: optional denoise / lens correction
Shader E: transfer/log encoding
Shader F: RGB -> YCbCr 4:4:4 intermediate
Shader G: chroma low-pass + 4:2:2 downsample + 10-bit pack
```

可在性能稳定后融合部分 pass，但必须先有独立 reference。

### 4:2:2 下采样

禁止直接丢弃每隔一个 chroma sample。

至少实现有定义的低通滤波，并建立测试图：

- 彩色单像素竖线；
- 红蓝棋盘格；
- zone plate；
- 饱和边缘；
- 灰阶；
- Log gradient。

确保：

- chroma phase 与 metadata 一致；
- 左右边界处理明确；
- odd width 若不支持必须拒绝；
- 不产生越界读写。

### 数值格式

建议同时保留：

```text
GPU precise mode: FP32
GPU fast mode: FP16 where validated
```

验收：

- GPU 生成的 YUV frame 可下载到 CPU；
- 与 CPU reference 做逐像素和感知比较；
- 明确误差阈值；
- 在接入编码器前先验证 raw YUV。

## Phase 5：实现真正 GPU-resident frame handoff

最终不能使用每帧 `av_hwframe_transfer_data()` 上传。

目标：

```text
application Vulkan compute writes VkImage
  -> ownership/layout transition
  -> AVFrame / AVVkFrame references same image
  -> encoder consumes it
```

### 关键要求

1. 同一 Vulkan logical device；
2. image format 与 FFmpeg frame context 兼容；
3. usage flags 满足处理和编码要求；
4. image layout 正确；
5. queue family ownership 正确；
6. synchronization 正确；
7. frame lifetime 覆盖 encoder async use；
8. memory allocation 与 alignment 正确；
9. 不在 encoder 完成前复用 frame；
10. 不在 CPU 中映射或复制未压缩图像。

### 优先实现方式

优先从 FFmpeg `AVHWFramesContext` pool 分配 frame，然后让应用 shader 写入其 `AVVkFrame::img[]`。

流程：

```text
av_hwframe_get_buffer()
  -> AVFrame(format=AV_PIX_FMT_VULKAN)
  -> obtain AVVkFrame
  -> record application compute commands writing to its VkImage
  -> transition and synchronize
  -> avcodec_send_frame()
```

只有在此方式证明无法满足性能或格式需求后，再考虑应用自有 image 导入到 FFmpeg user pool。

### 同步策略

初期建议：

- 单个 compute queue；
- timeline semaphore；
- 每 frame 唯一递增 timeline value；
- frame slot 状态机；
- encoder handoff 前确保写入完成；
- 不做 `vkQueueWaitIdle()` 每帧等待。

```cpp
enum class SlotState {
    Free,
    Processing,
    ReadyForEncode,
    Encoding,
    Recyclable,
    Failed
};
```

当且仅当 encoder 不再持有 frame reference，slot 才可复用。

验收：

- GPU output 不下载；
- encoder 输入为 `AV_PIX_FMT_VULKAN`；
- PCIe upload throughput 接近零；
- 长时间无花帧、旧帧重现或随机 corruption；
- validation layer 无 ownership/layout 错误。

## Phase 6：异步调度和 backpressure

推荐至少 4 个逻辑阶段：

```text
Read/Decode
GPU Process
GPU Encode
Mux/Write
```

队列必须有界：

```text
raw_queue:       bounded
gpu_ready_queue: bounded
packet_queue:    bounded
```

### pool size

```text
pool_size =
    decode_in_flight
  + processing_in_flight
  + encoder_async_depth
  + safety_margin
```

建议初始 8～16 帧，再通过 telemetry 调整。

禁止出现在正常每帧路径：

```cpp
vkDeviceWaitIdle();
vkQueueWaitIdle();
avcodec_flush_buffers();
```

### muxer backpressure

必须：

- packet writer 独立线程或异步 I/O；
- 监测 packet queue 深度；
- 监测实际写入 MB/s；
- 磁盘不足时明确 block 或 fail；
- 不能 drop frame 后继续生成看似正常的文件。

---

## 5. 编码 profile 与输出策略

第一阶段只支持：

```text
ProRes 422 Standard
ProRes 422 HQ
10-bit 4:2:2
progressive
no alpha
MOV
```

后续再增加 Proxy、LT、4444、4444 XQ、alpha 和 interlaced。

不要假定 profile 的数字值永远稳定或与另一个 encoder 相同。使用当前 FFmpeg build 的 option API 和定义。

配置示例：

```json
{
  "encoder": "prores_ks_vulkan",
  "profile": "hq",
  "async_depth": 8,
  "gpu_index": 0,
  "fallback": "prores_ks",
  "gpu_pipeline": true,
  "precision": "fp32"
}
```

至少正确写入：

- width/height；
- frame rate；
- time base；
- color primaries；
- transfer characteristic；
- matrix coefficients；
- color range；
- chroma location；
- SAR/DAR；
- rotation/orientation；
- audio sync。

---

## 6. 错误处理和回退

### 6.1 启动时

以下任一条件失败，GPU backend 标记 unavailable：

- encoder not found；
- Vulkan device creation failed；
- required format unsupported；
- hw frame context init failed；
- shader pipeline creation failed；
- encoder open failed；
- smoke test failed。

```text
Auto mode:
    log reason
    fallback CPU

Force GPU mode:
    fail explicitly
```

### 6.2 运行时

对 device lost：

1. 停止接收新帧；
2. 取消/结束队列；
3. 关闭并标记当前输出不完整；
4. 释放 FFmpeg/Vulkan 资源；
5. 不在同一输出文件中无缝切换 CPU；
6. 允许从 checkpoint 或重新开始整个文件。

### 6.3 临时文件

输出到：

```text
filename.mov.partial
```

仅在 encoder flush、muxer trailer、file close 和基础验证都成功后，原子 rename 为最终文件。

---

## 7. 测试设计

### 7.1 单元测试

覆盖：

- range conversion；
- transfer functions；
- RGB->YCbCr；
- 10-bit quantization；
- rounding；
- chroma filter；
- edge handling；
- PTS rescale；
- profile mapping；
- config validation；
- resource state machine。

### 7.2 GPU shader golden tests

对每个 shader：

1. 小尺寸 deterministic input；
2. GPU dispatch；
3. readback；
4. 与 double/FP64 CPU reference 比较；
5. 报告 max abs error、mean abs error、RMSE、percentile 和异常坐标。

不要只比较最终压缩文件。

### 7.3 整体测试素材

- flat fields；
- near-black Log gradients；
- near-white gradients；
- saturated primaries；
- skin-tone-like patches；
- color checker；
- high-frequency chroma；
- random noise；
- real MCRAW clips；
- extreme white balance；
- clipped highlights；
- bad pixels。

CPU ProRes 与 GPU ProRes bitstream 不必 bit-identical，应比较 decoded frame、metadata、profile、bitrate 和 artifacts。

### 7.4 兼容性矩阵

| Reader/NLE | Decode | Seek | Color | Duration | Audio sync |
|---|---:|---:|---:|---:|---:|
| ffmpeg/ffprobe | | | | | |
| DaVinci Resolve | | | | | |
| Adobe Premiere Pro | | | | | |
| VLC/mpv | | | | | |

---

## 8. Benchmark 方法

分别测：

```text
A. CPU baseline full pipeline
B. CPU image pipeline + GPU upload + Vulkan ProRes
C. GPU image pipeline + readback + CPU ProRes
D. GPU image pipeline + Vulkan ProRes
E. D + mux/write
```

每项：

- warm-up 100 帧；
- 正式测试至少 1000 帧或 30 秒；
- 重复 3～5 次；
- 报 median、p95；
- 固定素材和电源策略；
- 输出写 RAM disk 与真实 SSD 两组。

指标：

```text
fps
ms/frame
GPU process ms
GPU encode ms
CPU submit ms
queue wait ms
disk write ms
VRAM peak
system RAM peak
PCIe H2D MB/s
PCIe D2H MB/s
CPU utilization
GPU compute utilization
```

GPU pipeline 合入建议门槛：

- 画质通过；
- end-to-end 至少提高 1.5×；
- CPU 使用率明显下降；
- 无未压缩 frame readback；
- 无每帧 queue/device idle；
- 4K30 持续实时以上；
- 长时间无资源增长。

---

## 9. Windows 专项要求

- 固定 FFmpeg headers/libs 和 build config；
- 保存 `ffmpeg -buildconf`；
- 明确 LGPL/GPL 配置和 license notices；
- 检查 `vulkan-1.dll` 和 ICD；
- release 不依赖 Vulkan SDK 或 validation layer；
- 为 Vulkan object 设置 debug names；
- 支持 RenderDoc、Nsight、GPUView/ETW markers；
- 避免单次超长 shader dispatch 触发 TDR；
- 不建议要求普通用户修改 TDR registry。

---

## 10. 代码结构建议

```text
src/
  pipeline/
    pipeline_scheduler.*
    frame_metadata.*
    frame_queue.*
    cancellation.*

  cpu/
    cpu_decode_backend.*
    cpu_image_pipeline.*
    cpu_prores_encoder.*

  vulkan/
    vulkan_instance.*
    vulkan_device.*
    vulkan_queue.*
    vulkan_frame_pool.*
    vulkan_sync.*
    vulkan_shader_cache.*
    vulkan_telemetry.*

    shaders/
      raw_normalize.comp
      debayer.comp
      color_transform.comp
      log_encode.comp
      rgb_to_ycbcr.comp
      chroma_422.comp

  ffmpeg/
    ffmpeg_raii.*
    ffmpeg_error.*
    ffmpeg_vulkan_context.*
    prores_vulkan_encoder.*
    mov_muxer.*

  tests/
    color_math_tests.*
    chroma_tests.*
    shader_golden_tests.*
    encoder_smoke_tests.*
    end_to_end_tests.*
```

---

## 11. Telemetry

每次转码输出结构化日志：

```json
{
  "pipeline": "vulkan_prores",
  "ffmpeg_version": "...",
  "ffmpeg_commit": "...",
  "encoder": "prores_ks_vulkan",
  "gpu": "...",
  "driver": "...",
  "resolution": "3840x2160",
  "profile": "hq",
  "async_depth": 8,
  "frame_pool": 12,
  "gpu_resident": true,
  "upload_frames": 0,
  "readback_frames": 0,
  "fps": 0.0,
  "vram_peak_mb": 0,
  "fallback": false
}
```

关键 invariant：

```text
gpu_resident == true
=> upload_frames == 0
=> readback_frames == 0
```

压缩后的 packet 回到 CPU 不计为 frame readback。

---

## 12. Codex 分阶段任务

### Task 1：审计现有 CPU pipeline

输出：

- pipeline 图；
- frame formats；
- thread model；
- color transforms；
- ownership；
- bottlenecks；
- GPU 插入点。

### Task 2：建立 encoder backend interface

完成 `IVideoEncoder`、CPU adapter、GPU stub、config 和 capability reporting，原测试必须全部通过。

### Task 3：引入可复现 FFmpeg Vulkan build

完成 pinned commit、build scripts、CI capability check、license 和 `prores_ks_vulkan` probe。

### Task 4：Vulkan runtime 与 FFmpeg hw context

完成 device selection、shared device、frames context、frame allocation 和 smoke test。

### Task 5：CPU upload bridge encoder

完成 CPU YUV -> Vulkan upload -> encode -> mux -> flush。此任务只证明集成，不宣称最终加速。

### Task 6：GPU RGB->YUV 422

完成 shaders、golden tests、range/log/chroma tests 和 precision mode。

### Task 7：GPU-resident handoff

完成 frame pool、直接写 encoder-compatible image、同步、无 upload、无 readback和长时间测试。

### Task 8：异步 pipeline

完成 bounded queues、async depth、backpressure、cancellation 和 telemetry。

### Task 9：性能优化

只按 profiler 结果优化：

- command submission；
- descriptor churn；
- image transitions；
- queue contention；
- shader occupancy；
- memory bandwidth；
- frame pool stalls；
- disk stalls；
- 不必要的格式转换。

### Task 10：生产化

完成 fallback、partial file、error taxonomy、device lost、compatibility matrix、用户设置和 benchmark report。

---

## 13. 禁止事项

Codex 不得：

- 删除 CPU pipeline；
- 将 upload bridge 当作最终 GPU pipeline；
- 每帧调用 `vkQueueWaitIdle()`；
- 每帧创建/销毁 Vulkan pipeline、descriptor pool 或 device；
- 使用无界 frame queue；
- 在未验证时硬编码 FFmpeg private option；
- 假设 `AV_PIX_FMT_VULKAN` 对应固定内存布局；
- 忽略 color range 和 chroma location；
- 只用肉眼检查画质；
- 为掩盖同步 bug 加全局 queue idle；
- 在无 benchmark 证据时融合全部 shaders；
- 让 GUI 线程承担编码或 mux I/O；
- 将 FFmpeg/Vulkan 裸指针散布到业务层；
- 用隐藏的 CPU copy 伪装 GPU-resident pipeline。

---

## 14. Definition of Done

- [ ] CPU backend 未回归；
- [ ] `prores_ks_vulkan` runtime capability probe；
- [ ] pinned FFmpeg build；
- [ ] Windows NVIDIA 测试通过；
- [ ] 至少一个其他 vendor 的 capability/failure 测试；
- [ ] Vulkan validation clean；
- [ ] GPU shader golden tests 通过；
- [ ] 4:2:2 chroma tests 通过；
- [ ] Log/range/metadata tests 通过；
- [ ] 完整 send/receive/flush；
- [ ] MOV trailer 和 partial file 策略；
- [ ] Resolve/Premiere/FFmpeg compatibility；
- [ ] 无未压缩帧 upload；
- [ ] 无未压缩帧 readback；
- [ ] 无每帧 global wait；
- [ ] bounded queues；
- [ ] cancellation；
- [ ] device lost handling；
- [ ] 1 小时稳定性；
- [ ] batch stability；
- [ ] benchmark 达到项目门槛；
- [ ] CPU fallback 可用；
- [ ] 文档和许可证完成。

---

## 15. 建议的第一条实现路线

```text
CPU reference frozen
  -> encoder abstraction
  -> pinned FFmpeg build
  -> Vulkan device + AVHWFramesContext
  -> CPU YUV upload bridge
  -> prores_ks_vulkan encode/mux
  -> verify compatibility
  -> Vulkan RGB->YUV 422
  -> write into FFmpeg-owned Vulkan frames
  -> remove upload
  -> asynchronous frame pool
  -> optimize with profiler
  -> production fallback and stability
```

不要从自定义 RAW Vulkan debayer直接跳到完整 ProRes 输出。先证明编码器桥接，再证明色彩转换，再证明零拷贝，最后做并行和性能优化。

---

## 16. 参考资料

- FFmpeg `prores_ks_vulkan` source / Doxygen:  
  https://ffmpeg.org/doxygen/trunk/proresenc__kostya__vulkan_8c_source.html

- FFmpeg `AVHWFramesContext`:  
  https://ffmpeg.org/doxygen/trunk/structAVHWFramesContext.html

- FFmpeg Vulkan hardware context header:  
  https://ffmpeg.org/doxygen/trunk/hwcontext__vulkan_8h_source.html

- FFmpeg `AVVkFrame`:  
  https://ffmpeg.org/doxygen/trunk/structAVVkFrame.html

- FFmpeg hardware encode example:  
  https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/vaapi_encode.c

- Khronos: Vulkan Compute codecs in FFmpeg:  
  https://www.khronos.org/blog/video-encoding-and-decoding-with-vulkan-compute-shaders-in-ffmpeg

- FFmpeg codecs documentation:  
  https://ffmpeg.org/ffmpeg-codecs.html

---

## 17. 给 Codex 的最终执行提示

在开始编码前，先提交一份 `GPU_PIPELINE_AUDIT.md`，内容必须包括：

1. 当前 CPU pipeline 的实际数据流；
2. 每阶段输入输出格式；
3. 所有颜色变换；
4. frame ownership；
5. thread/queue model；
6. FFmpeg 接入位置；
7. Vulkan device owner 方案；
8. 零拷贝 handoff 方案；
9. 已识别风险；
10. Phase 1～3 的文件级修改计划。

审计获批后，再从 encoder abstraction 和 upload bridge 开始实施。任何阶段发现当前 FFmpeg build、Vulkan image format 或同步接口不满足零拷贝时，应记录事实和最小复现，不得用隐藏的 CPU copy 伪装成 GPU-resident pipeline。
