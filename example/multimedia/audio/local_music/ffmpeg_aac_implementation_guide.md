# SiFli SDK AAC 音频编解码实现指南

## 目录

1. [概述](#1-概述)
2. [AAC 解码方案对比](#2-aac-解码方案对比)
3. [FFmpeg AAC 解码器详解](#3-ffmpeg-aac-解码器详解)
4. [libfaad2 AAC 解码器详解](#4-libfaad2-aac-解码器详解)
5. [AAC 编码器](#5-aac-编码器)
6. [硬件支持](#6-硬件支持)
7. [配置选项详解](#7-配置选项详解)
8. [推荐例程](#8-推荐例程)
9. [性能分析](#9-性能分析)
10. [实现步骤](#10-实现步骤)
11. [性能优化建议](#11-性能优化建议)
12. [常见问题](#12-常见问题)

---

## 1. 概述

### 1.1 AAC 简介

AAC (Advanced Audio Coding) 是一种高效的音频编码标准，相比 MP3 提供更好的音质和更高的压缩率。在 SiFli SDK 中，AAC 编解码通过**纯软件实现**，没有专用的硬件加速器。

### 1.2 SiFli SDK 中的 AAC 实现

| 实现方式          | 库                   | 用途                  | 默认状态 |
| ----------------- | -------------------- | --------------------- | -------- |
| FFmpeg AAC 解码器 | `external/ffmpeg/` | AAC 音频解码          | ✅ 启用  |
| FFmpeg AAC 编码器 | `external/ffmpeg/` | AAC 音频编码          | ❌ 禁用  |
| libfaad2          | `external/faad2/`  | AAC 音频解码 (独立库) | ❌ 禁用  |

### 1.3 关键结论

**AAC 编解码在 SiFli SDK 中是纯软件实现，没有硬件加速。** 但可以通过以下方式优化性能：

- 使用 DMA 传输减少 CPU 参与
- 选择合适的采样率和通道配置

---

## 2. AAC 解码方案对比

### 2.1 FFmpeg AAC 解码器 vs libfaad2

| 特性                     | FFmpeg AAC 解码器                       | libfaad2                            |
| ------------------------ | --------------------------------------- | ----------------------------------- |
| **路径**           | `external/ffmpeg/libavcodec/aacdec.c` | `external/faad2/libfaad/`         |
| **许可证**         | LGPL v2.1+                              | GPL v2+                             |
| **支持的 Profile** | Main, LC, LTP, SSR, HE-AAC              | Main, LC, LTP, SSR, HE-AAC, LD, DRM |
| **浮点/定点**      | 支持浮点和定点两种模式                  | 支持浮点和定点                      |
| **SBR 支持**       | ✅ 完整支持                             | ✅ 完整支持                         |
| **PS 支持**        | ✅ 支持                                 | ✅ 支持                             |
| **代码大小**       | 较大 (含完整 FFmpeg 框架)               | 较小 (独立库)                       |
| **集成难度**       | 需要 FFmpeg 框架                        | 独立易集成                          |
| **推荐场景**       | 已使用 FFmpeg 的项目                    | 轻量级 AAC 解码需求                 |

### 2.2 推荐选择

- **如果项目已使用 FFmpeg**：直接使用 FFmpeg 内置的 AAC 解码器
- **如果只需要 AAC 解码**：使用 libfaad2 更轻量
- **如果需要 AAC 编码**：只能使用 FFmpeg (需手动启用)

---

## 3. FFmpeg AAC 解码器详解

### 3.1 支持的 AAC Profile

FFmpeg AAC 解码器支持以下 Profile：

| Profile                                  | 说明                         | 默认支持 |
| ---------------------------------------- | ---------------------------- | -------- |
| **AAC-LC** (Low Complexity)        | 最常用的 Profile，编码效率高 | ✅       |
| **AAC Main**                       | 比 LC 复杂，但音质略好       | ✅       |
| **AAC-LTP** (Long Term Prediction) | 支持长期预测                 | ✅       |
| **AAC-SSR** (Scalable Sample Rate) | 支持可伸缩采样率             | ✅       |
| **HE-AAC** (High Efficiency)       | 含 SBR，低码率音质好         | ✅       |
| **HE-AAC v2**                      | 含 SBR + PS                  | ✅       |
| **MPEG-2 AAC-LC**                  | MPEG-2 兼容模式              | ✅       |

### 3.2 源文件组成

FFmpeg AAC 解码器由以下文件组成：

```
external/ffmpeg/libavcodec/
├── aacdec.c           # 主解码器实现
├── aacdec_fixed.c     # 定点解码器 (CONFIG_AUDIO_USING_FIXED=1)
├── aacsbr.c           # SBR (Spectral Band Replication) 解码
├── aacsbr_fixed.c     # 定点 SBR 解码
├── aacps.c            # PS (Parametric Stereo) 解码
├── aacps_fixed.c      # 定点 PS 解码
├── aacpsdsp.c         # PS DSP 运算
├── aacpsdsp_fixed.c   # 定点 PS DSP 运算
├── aactab.c           # AAC 表格数据
├── sbrdsp.c           # SBR DSP 运算
├── sbrdsp_fixed.c     # 定点 SBR DSP 运算
├── kbdwin.c           # KBD 窗函数
├── aac_ac3_parser.c   # AAC/AC3 解析器
├── aac_parser.c       # AAC 解析器
└── aacadtsdec.c       # ADTS 解码

external/ffmpeg/libavformat/
├── aacdec_fmt.c       # AAC 格式处理
```

### 3.3 浮点 vs 定点模式

在 `external/ffmpeg/SConscript` 中配置：

```python
# 浮点模式 (默认)
CONFIG_AUDIO_USING_FLOAT = 1
CONFIG_AUDIO_USING_FIXED = 0

# 定点模式 (推荐用于无 FPU 的 MCU)
CONFIG_AUDIO_USING_FLOAT = 0
CONFIG_AUDIO_USING_FIXED = 1
```

**模式对比：**

| 特性               | 浮点模式      | 定点模式      |
| ------------------ | ------------- | ------------- |
| **精度**     | 高            | 略低          |
| **速度**     | 有 FPU 时更快 | 无 FPU 时更快 |
| **代码大小** | 较小          | 较大          |
| **内存使用** | 较少          | 较多          |
| **推荐场景** | 有 FPU 的芯片 | 无 FPU 的 MCU |

**注意：** SiFli 的 Cortex-M33 HCPU 通常带有 FPU，建议使用浮点模式以获得更好的性能。

### 3.4 编译宏定义

启用 AAC 解码器后，SConscript 会定义以下宏：

```c
CONFIG_AAC_DECODER_SUPPORT  // AAC 解码器支持
CONFIG_AAC_DECODER          // AAC 解码器
CONFIG_MDCT                 // MDCT 运算 (AAC 核心)
```

---

## 4. libfaad2 AAC 解码器详解

### 4.1 简介

libfaad2 是一个独立的 AAC 解码库，支持所有主要的 AAC Profile。相比 FFmpeg，它更轻量，适合只需要 AAC 解码的场景。

### 4.2 支持的 Profile

```c
// 来自 neaacdec.h
#define MAIN       1    // AAC Main
#define LC         2    // AAC-LC (Low Complexity)
#define SSR        3    // AAC-SSR
#define LTP        4    // AAC-LTP
#define HE_AAC     5    // HE-AAC (含 SBR)
#define ER_LC     17    // ER-AAC-LC (Error Resilience)
#define ER_LTP    19    // ER-AAC-LTP
#define LD        23    // AAC-LD (Low Delay)
#define DRM_ER_LC 27    // DRM 专用
```

### 4.3 API 使用示例

```c
#include "neaacdec.h"

// 1. 打开解码器
NeAACDecHandle decoder = NeAACDecOpen();

// 2. 配置解码器
NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(config);
config->outputFormat = FAAD_FMT_16BIT;  // 16bit 输出
config->useOldSBR = 0;                   // 使用新 SBR 解码
NeAACDecSetConfiguration(decoder, config);

// 3. 初始化解码器 (从 ADTS 头获取信息)
unsigned long sample_rate;
unsigned char channels;
unsigned char *buffer = ...;  // AAC 数据
unsigned long buffer_size = ...;
NeAACDecInit(decoder, buffer, buffer_size, &sample_rate, &channels);

// 4. 解码帧
NeAACDecFrameInfo frame_info;
int16_t *output = (int16_t *)NeAACDecDecode(decoder, &frame_info, buffer, buffer_size);

// 5. 处理解码数据
if (frame_info.error == 0) {
    // output 包含 PCM 数据
    // frame_info.samples 为样本数
    // frame_info.channels 为通道数
}

// 6. 关闭解码器
NeAACDecClose(decoder);
```

### 4.4 编译配置

在 `proj.conf` 中启用：

```conf
CONFIG_PKG_USING_AAC_DECODER_LIBFAAD=y
```

### 4.5 与 FFmpeg 的选择建议

| 场景                                 | 推荐              |
| ------------------------------------ | ----------------- |
| 已使用 FFmpeg 播放视频 (含 AAC 音轨) | FFmpeg AAC 解码器 |
| 只需要 AAC 音频解码                  | libfaad2          |
| 需要 AAC 编码                        | FFmpeg (需启用)   |
| 需要 DRM 支持                        | libfaad2          |
| 代码大小敏感                         | libfaad2          |

---

## 5. AAC 编码器

### 5.1 FFmpeg AAC 编码器

FFmpeg 内置了 AAC 编码器，但**默认禁用**。

**支持的编码 Profile：**

```c
// 来自 aacenctab.h
static const int aacenc_profiles[] = {
    FF_PROFILE_AAC_MAIN,        // AAC Main
    FF_PROFILE_AAC_LOW,         // AAC-LC
    FF_PROFILE_AAC_LTP,         // AAC-LTP
    FF_PROFILE_MPEG2_AAC_LOW,   // MPEG-2 AAC-LC
};
```

### 5.2 启用 AAC 编码器

在 `external/ffmpeg/SConscript` 中修改：

```python
CONFIG_AAC_ENCODER = 1  # 启用 AAC 编码器
```

然后在 `proj.conf` 中添加：

```conf
CONFIG_PKG_USING_FFMPEG=y
```

### 5.3 AAC 编码器源文件

```
external/ffmpeg/libavcodec/
├── aacenc.c           # 主编码器
├── aaccoder.c         # 编码算法 (twoloop/trellis)
├── aacenctab.c        # 编码表格
├── aacpsy.c           # 心理声学模型
├── aacenc_is.c        # 强度立体声
├── aacenc_tns.c       # TNS 编码
├── aacenc_ltp.c       # LTP 编码
├── aacenc_pred.c      # 预测编码
├── psymodel.c         # 心理声学模型
├── audio_frame_queue.c # 音频帧队列
├── iirfilter.c        # IIR 滤波器
└── lpc.c              # LPC 编码
```

### 5.4 AAC 编码 API 示例

```c
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

// 1. 查找编码器
const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

// 2. 分配编码上下文
AVCodecContext *ctx = avcodec_alloc_context3(codec);
ctx->codec_id = AV_CODEC_ID_AAC;
ctx->codec_type = AVMEDIA_TYPE_AUDIO;
ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // 浮点平面格式
ctx->sample_rate = 44100;
ctx->channel_layout = AV_CH_LAYOUT_STEREO;
ctx->bit_rate = 128000;  // 128kbps

// 3. 打开编码器
avcodec_open2(ctx, codec, NULL);

// 4. 编码帧
AVFrame *frame = av_frame_alloc();
frame->nb_samples = 1024;  // AAC 帧大小
frame->format = ctx->sample_fmt;
frame->channel_layout = ctx->channel_layout;
av_frame_get_buffer(frame, 0);

// 填充 PCM 数据到 frame->data[0]

AVPacket *pkt = av_packet_alloc();
int ret = avcodec_send_frame(ctx, frame);
if (ret == 0) {
    ret = avcodec_receive_packet(ctx, pkt);
    if (ret == 0) {
        // pkt->data 包含编码后的 AAC 数据
    }
}

// 5. 清理
av_frame_free(&frame);
av_packet_free(&pkt);
avcodec_free_context(&ctx);
```

---

## 6. 硬件支持

### 6.1 重要说明

**AAC 编解码本身没有硬件加速器。** 本方案采用 **纯软件实现**，不依赖 AUDPRC/FACC/FFT 等硬件加速器。所有音频处理（解码、混音、采样率转换等）均由 CPU 完成。

### 6.2 DMA 传输

使用 DMA 循环传输 AAC 解码后的 PCM 数据到 DAC，减少 CPU 参与数据搬运：

```conf
# 启用 DMA
CONFIG_BSP_AUDCODEC_DAC0_DMA=y
```

---

## 7. 配置选项详解

### 7.1 FFmpeg AAC 配置

```conf
# ========== FFmpeg 核心 ==========
CONFIG_PKG_USING_FFMPEG=y              # 启用 FFmpeg
CONFIG_PKG_USING_FFMPEG_AUDIO=y        # 启用音频支持 (默认 y)
```

### 7.2 AAC 解码器配置 (SConscript)

在 `external/ffmpeg/SConscript` 中：

```python
# AAC 解码器 (默认启用)
CONFIG_AAC_DECODER_SUPPORT = 1

# 浮点/定点选择
CONFIG_AUDIO_USING_FLOAT = 1   # 浮点模式 (推荐)
CONFIG_AUDIO_USING_FIXED = 0   # 定点模式
```

### 7.3 libfaad2 配置

```conf
# 使用 libfaad2 (独立 AAC 解码库)
CONFIG_PKG_USING_AAC_DECODER_LIBFAAD=y
```

### 7.4 AAC 编码器配置

```python
# 在 external/ffmpeg/SConscript 中
CONFIG_AAC_ENCODER = 1  # 启用 AAC 编码器
```

### 7.5 音频系统配置

```conf
# ========== 音频系统 ==========
CONFIG_AUDIO=y                         # 启用音频系统
CONFIG_AUDIO_PATH_USING_HCPU=y         # 音频处理在 HCPU
CONFIG_AUDIO_USING_AUDPROC=y           # 启用音频处理框架
CONFIG_AUDIO_USING_MANAGER=y           # 启用音频管理器
CONFIG_AUDIO_LOCAL_MUSIC=y             # 启用本地音乐播放

# 播放路径
CONFIG_AUDIO_SPEAKER_USING_CODEC=y     # 扬声器使用片上 Codec
```

### 7.6 硬件配置

```conf
# ========== 硬件 ==========
CONFIG_BSP_ENABLE_AUD_CODEC=y          # 启用 AUDCODEC

# DMA 通道
CONFIG_BSP_AUDCODEC_DAC0_DMA=y
```

### 7.7 完整配置示例

```conf
# ========== 文件系统 ==========
CONFIG_RT_USING_DFS_ELMFAT=y

# ========== FFmpeg AAC 解码 ==========
CONFIG_PKG_USING_FFMPEG=y
CONFIG_PKG_USING_FFMPEG_AUDIO=y

# ========== 音频系统 ==========
CONFIG_AUDIO=y
CONFIG_AUDIO_PATH_USING_HCPU=y
CONFIG_AUDIO_USING_AUDPROC=y
CONFIG_AUDIO_USING_MANAGER=y
CONFIG_AUDIO_LOCAL_MUSIC=y
CONFIG_AUDIO_SPEAKER_USING_CODEC=y

# ========== 硬件 ==========
CONFIG_BSP_ENABLE_AUD_CODEC=y
CONFIG_BSP_AUDCODEC_DAC0_DMA=y

# ========== 性能监控 ==========
CONFIG_USING_CPU_USAGE_PROFILER=y
```

---

## 8. 推荐例程

### 8.1 AAC 音频播放示例

虽然 SDK 中没有专门的 AAC 播放例程，但以下例程可以作为参考：

| 例程                       | 路径                                          | 参考价值                         |
| -------------------------- | --------------------------------------------- | -------------------------------- |
| **local_music**      | `example/multimedia/audio/local_music/`     | 音频播放框架、Audio Manager 使用 |
| **flac**             | `example/multimedia/audio/flac/`            | 完整的录音→编码→解码→播放流程 |
| **opus**             | `example/multimedia/audio/opus/`            | 实时编解码、WebRTC 集成          |
| **lvgl_v8_examples** | `example/multimedia/lvgl/lvgl_v8_examples/` | FFmpeg 集成、视频+音频播放       |

### 8.2 推荐方案

#### 方案 A：使用 FFmpeg 播放含 AAC 音轨的视频

```conf
CONFIG_PKG_USING_FFMPEG=y
CONFIG_PKG_USING_FFMPEG_AUDIO=y
CONFIG_AUDIO=y
CONFIG_AUDIO_LOCAL_MUSIC=y
```

参考例程：`lvgl_v8_examples` 中的 `lv_example_ffmpeg_2`

#### 方案 B：使用 libfaad2 独立 AAC 解码

```conf
CONFIG_PKG_USING_AAC_DECODER_LIBFAAD=y
CONFIG_AUDIO=y
CONFIG_AUDIO_LOCAL_MUSIC=y
```

参考例程：`local_music` (修改为 AAC 输入)

#### 方案 C：AAC 编码 + 解码

```conf
CONFIG_PKG_USING_FFMPEG=y
CONFIG_PKG_USING_FFMPEG_AUDIO=y
CONFIG_AUDIO=y
```

需要手动修改 `external/ffmpeg/SConscript` 启用编码器。

---

## 9. 性能分析

### 9.1 AAC 解码性能特点

AAC 解码是纯软件实现，CPU 消耗取决于：

| 因素                | 影响                                  |
| ------------------- | ------------------------------------- |
| **采样率**    | 采样率越高，CPU 消耗越大              |
| **通道数**    | 立体声比单声道消耗更多 CPU            |
| **Profile**   | HE-AAC 比 AAC-LC 消耗更多 (含 SBR/PS) |
| **浮点/定点** | 无 FPU 时定点更快                     |
| **优化级别**  | 编译优化选项影响性能                  |

### 9.2 典型性能指标

**Cortex-M33 @ 200MHz (带 FPU)：**

| AAC 配置                 | 码率        | 预估 CPU 使用率 |
| ------------------------ | ----------- | --------------- |
| AAC-LC 44.1kHz 单声道    | 64-128kbps  | 5-10%           |
| AAC-LC 44.1kHz 立体声    | 128-256kbps | 10-20%          |
| AAC-LC 48kHz 立体声      | 128-256kbps | 12-22%          |
| HE-AAC 44.1kHz 立体声    | 32-64kbps   | 15-25%          |
| HE-AAC v2 44.1kHz 立体声 | 24-48kbps   | 18-30%          |

**Cortex-M33 @ 200MHz (无 FPU，定点模式)：**

| AAC 配置              | 码率        | 预估 CPU 使用率 |
| --------------------- | ----------- | --------------- |
| AAC-LC 44.1kHz 单声道 | 64-128kbps  | 8-15%           |
| AAC-LC 44.1kHz 立体声 | 128-256kbps | 15-30%          |
| HE-AAC 44.1kHz 立体声 | 32-64kbps   | 20-35%          |

### 9.3 性能监控方法

#### 方法一：CPU 使用率监控

```conf
CONFIG_USING_CPU_USAGE_PROFILER=y
```

```c
#include <rtthread.h>

// 获取 CPU 使用率
rt_uint8_t cpu_usage = rt_cpu_usage();
rt_kprintf("AAC Decode CPU Usage: %d%%\n", cpu_usage);
```

#### 方法二：DWT 周期计数器（高精度，推荐）

Cortex-M33 内置 DWT 单元，可提供 CPU 周期级精度的耗时统计：

```c
#include <core_cm33.h>

// 初始化 DWT
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

// 开始计时
uint32_t start = DWT->CYCCNT;

// 执行 AAC 解码
aac_decode(...);

// 结束计时
uint32_t end = DWT->CYCCNT;

// 计算耗时 (假设 CPU 频率为 200MHz)
uint32_t cycles = end - start;
float time_us = (float)cycles / 200.0f;
rt_kprintf("AAC decode: %d cycles = %.1f us\n", cycles, time_us);
```

#### 方法三：SystemView 追踪

```conf
CONFIG_PKG_USING_SYSTEMVIEW=y
```

使用 SEGGER SystemView 可以详细分析：

- AAC 解码耗时
- 音频数据传输耗时
- 线程调度情况

### 9.4 关键性能指标

| 性能参数               | 测量方法                  | 目标值/说明         |
| ---------------------- | ------------------------- | ------------------- |
| **CPU 利用率**   | `rt_cpu_usage()`        | AAC-LC 立体声 < 20% |
| **单帧解码时间** | DWT 周期计数              | 1024 samples < 10ms |
| **端到端延迟**   | PCM 数据写入到 DAC 输出   | < 50ms              |
| **内存使用**     | `rt_memory_info()` 监控 | 堆栈 + 堆           |
| **帧丢失率**     | 统计解码失败帧数          | = 0                 |
| **DMA 传输延迟** | DMA 中断间隔              | 稳定无抖动          |

### 9.5 影响 CPU 使用率的因素

1. **编译优化**：使用 `-O2` 或 `-Os` 优化
2. **FPU 使用**：启用浮点运算单元
3. **缓存**：合理使用 I-Cache 和 D-Cache
4. **DMA**：使用 DMA 传输减少 CPU 参与
5. **线程优先级**：合理设置解码线程优先级

### 9.6 性能监控代码框架

```c
#include <rtthread.h>
#include <core_cm33.h>

// 性能统计结构体
struct aac_perf_stats {
    uint32_t decode_cycles;      // 解码周期数
    uint32_t max_decode_cycles;  // 最大解码周期
    uint32_t frame_count;        // 帧计数
    uint32_t error_count;        // 错误帧计数
};

static struct aac_perf_stats perf_stats = {0};

// 初始化 DWT
void perf_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// 开始计时
uint32_t perf_start(void)
{
    return DWT->CYCCNT;
}

// 结束计时并记录
void perf_stop(uint32_t start)
{
    uint32_t cycles = DWT->CYCCNT - start;
    perf_stats.decode_cycles += cycles;
    perf_stats.frame_count++;
    if (cycles > perf_stats.max_decode_cycles) {
        perf_stats.max_decode_cycles = cycles;
    }
}

// 打印统计报告
void perf_report(void)
{
    if (perf_stats.frame_count == 0) return;
  
    uint32_t avg_cycles = perf_stats.decode_cycles / perf_stats.frame_count;
    rt_kprintf("=== AAC Performance Report ===\n");
    rt_kprintf("Total frames: %d\n", perf_stats.frame_count);
    rt_kprintf("Avg decode: %d cycles (%.1f us @200MHz)\n", 
               avg_cycles, (float)avg_cycles / 200.0f);
    rt_kprintf("Max decode: %d cycles (%.1f us @200MHz)\n",
               perf_stats.max_decode_cycles, 
               (float)perf_stats.max_decode_cycles / 200.0f);
    rt_kprintf("Error frames: %d\n", perf_stats.error_count);
}
```

---

## 10. 实现步骤

### 10.1 步骤一：选择 AAC 解码方案

根据项目需求选择：

- **已使用 FFmpeg** → 使用 FFmpeg AAC 解码器
- **轻量级需求** → 使用 libfaad2
- **需要编码** → 使用 FFmpeg AAC 编码器

### 10.2 步骤二：配置 proj.conf

以 FFmpeg AAC 解码为例：

```conf
# 文件系统
CONFIG_RT_USING_DFS_ELMFAT=y

# FFmpeg
CONFIG_PKG_USING_FFMPEG=y
CONFIG_PKG_USING_FFMPEG_AUDIO=y

# 音频系统
CONFIG_AUDIO=y
CONFIG_AUDIO_PATH_USING_HCPU=y
CONFIG_AUDIO_USING_AUDPROC=y
CONFIG_AUDIO_USING_MANAGER=y
CONFIG_AUDIO_LOCAL_MUSIC=y
CONFIG_AUDIO_SPEAKER_USING_CODEC=y

# 硬件
CONFIG_BSP_ENABLE_AUD_CODEC=y
CONFIG_BSP_AUDCODEC_DAC0_DMA=y

# 性能监控
CONFIG_USING_CPU_USAGE_PROFILER=y
```

### 10.3 步骤三：修改 SConscript (可选)

如果需要调整 AAC 解码选项，修改 `external/ffmpeg/SConscript`：

```python
# 浮点模式 (推荐)
CONFIG_AUDIO_USING_FLOAT = 1
CONFIG_AUDIO_USING_FIXED = 0

# 启用 AAC 编码器 (如果需要)
CONFIG_AAC_ENCODER = 1
```

### 10.4 步骤四：编写 AAC 解码代码

```c
#include <rtthread.h>
#include "media_dec.h"
#include "audio_server.h"

// AAC 文件路径
#define AAC_FILE_PATH "/music/test.aac"

void aac_decode_demo(void)
{
    ffmpeg_handle handle;
    ffmpeg_config_t cfg = {0};

    // 配置
    cfg.src = e_src_localfile;
    cfg.audio_enable = 1;
    cfg.video_enable = 0;  // 纯音频
    cfg.is_loop = 0;
    cfg.file_path = AAC_FILE_PATH;

    // 通知回调
    cfg.notify = aac_notify_callback;

    // 打开播放
    int ret = ffmpeg_open(&handle, &cfg, 0);
    if (ret == 0) {
        rt_kprintf("AAC playback started\n");

        // 播放控制
        ffmpeg_resume(handle);

        // 等待播放完成
        rt_thread_mdelay(30000);  // 等待 30 秒

        // 关闭
        ffmpeg_close(handle);
    }
}
```

### 10.5 步骤五：编译与测试

```bash
# 设置环境
source ./export.sh

# 编译
cd example/multimedia/audio/local_music/project
scons --board=sf32lb56-lcd_a128r12n1_hcpu -j8

# 烧录与测试
```

### 10.6 步骤六：性能测试

1. 启用性能监控（CPU 使用率 + DWT 周期计数）
2. 播放不同配置的 AAC 文件
3. 记录 CPU 使用率、单帧解码时间
4. 根据结果优化配置

---

## 11. 性能优化建议

### 11.1 降低 CPU 使用率

1. **使用浮点模式**：如果有 FPU，使用浮点解码更快
2. **使用 DMA 传输**：减少 CPU 参与数据传输
3. **优化编译选项**：使用 `-O2` 优化
4. **使用合适的 Profile**：AAC-LC 比 HE-AAC 更轻量

### 11.2 降低内存使用

1. **使用定点模式**：如果内存紧张
2. **调整缓冲区大小**：根据实际需求调整
3. **使用流式解码**：避免一次性加载整个文件

### 11.3 提升音质

1. **使用高码率**：128kbps 以上音质更好
2. **使用 AAC-LC**：音质和复杂度平衡最好
3. **启用 SBR/PS**：HE-AAC 在低码率下音质更好

---

## 12. 常见问题

### Q1: AAC 解码有硬件加速吗？

**A:** 没有。SiFli SDK 中的 AAC 解码是纯软件实现。所有音频处理（解码、混音等）均由 CPU 完成。

### Q2: FFmpeg AAC 解码器和 libfaad2 哪个更好？

**A:** 两者功能相似。FFmpeg 集成在 FFmpeg 框架中，适合已使用 FFmpeg 的项目；libfaad2 更轻量，适合只需要 AAC 解码的场景。

### Q3: 如何启用 AAC 编码器？

**A:** 在 `external/ffmpeg/SConscript` 中设置 `CONFIG_AAC_ENCODER = 1`，然后重新编译。

### Q4: AAC 解码的 CPU 使用率是多少？

**A:** 取决于配置。典型值：AAC-LC 44.1kHz 立体声约 10-20% (Cortex-M33 @ 200MHz，带 FPU)。

### Q5: 如何查看实际的 CPU 使用率？

**A:** 启用 `CONFIG_USING_CPU_USAGE_PROFILER=y`，然后使用 `rt_cpu_usage()` API 获取。

### Q6: 支持哪些 AAC Profile？

**A:** 支持 AAC-LC、AAC Main、AAC-LTP、AAC-SSR、HE-AAC、HE-AAC v2、MPEG-2 AAC-LC。

### Q7: 如何使用硬件 FFT 加速？

**A:** 如果使用 WebRTC 音频处理，可以在板级 Kconfig 中选择 `FFT_USING_ONCHIP=y` (需要 SF32LB56X 或 SF32LB58X)。

---

## 附录

### A. 相关文件索引

| 类别              | 文件路径                                          |
| ----------------- | ------------------------------------------------- |
| FFmpeg Kconfig    | `external/ffmpeg/Kconfig`                       |
| FFmpeg SConscript | `external/ffmpeg/SConscript`                    |
| FFmpeg AAC 解码器 | `external/ffmpeg/libavcodec/aacdec.c`           |
| FFmpeg AAC 编码器 | `external/ffmpeg/libavcodec/aacenc.c`           |
| libfaad2 头文件   | `external/faad2/libfaad/include/neaacdec.h`     |
| 媒体解码 API      | `middleware/media/media_dec.h`                  |
| 音频管理器        | `middleware/audio/audio_manager/audio_server.c` |
| AUDCODEC HAL      | `drivers/hal/bf0_hal_audcodec.c`                |

### B. AAC Profile 对照表

| Profile   | 复杂度 | 音质          | 码率        | 典型应用     |
| --------- | ------ | ------------- | ----------- | ------------ |
| AAC-LC    | 低     | 好            | 128-256kbps | 音乐流媒体   |
| AAC Main  | 中     | 略好          | 128-256kbps | 高质量音乐   |
| AAC-LTP   | 高     | 好            | 128-256kbps | 语音/音乐    |
| HE-AAC    | 中     | 好 (低码率)   | 32-64kbps   | 低带宽流媒体 |
| HE-AAC v2 | 高     | 好 (极低码率) | 24-48kbps   | 极低带宽     |

### C. 参考文档

- `docs/source/en/hal/audcodec.md` - AUDCODEC HAL 使用文档
- `docs/source/zh_CN/hal/audcodec.md` - AUDCODEC 中文文档

---

**文档版本：** v2.1 (AAC 专题版 - 纯软件实现)
**生成日期：** 2026-08-07
**适用 SDK 版本：** SiFli SDK main branch
