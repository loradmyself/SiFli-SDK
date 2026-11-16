# SiFli HFP 蓝牙通话 3A/AEC 开发指南

> 面向 `main/example/bt/hfp` 例程的实战指南：从建立一通电话开始，讲清 3A 算法的代码位置、mic 与喇叭两路数据的流转路径、把 AEC 参考信号接进自研算法的方法，以及把这两路原始 PCM 数据 dump 出来做离线分析的完整操作。

## TL;DR

这个例程默认就跑着 WebRTC 的 AECM + NS + AGC 三件套：**喇叭实际播放的数据**经 `audio_3a_far_put()` 进入 `WebRtcAecm_BufferFarend()` 作为远端参考，**mic 采集数据**经 `audio_3a_uplink()` 做降噪后进入 `WebRtcAecm_Process()` 作为近端，两者在 `audio_3a_data_process()` 里汇合；想换成自研算法，实现同一组接口并改一行 SConscript 即可；想在真机上抓这两路数据，用 `audio_data -aecm_input1 -audprc` 一条命令就能导出。

## 1. 你将学到什么

- 编译、烧录并让 HFP 例程真正进入一通通话的完整步骤
- 3A（AEC/NS/AGC）代码在 SDK 中的位置，以及编译时到底选中了哪个实现
- 通话上下行两条数据通路的每一个节点、缓冲区与帧长约定
- AEC 从哪两个函数、哪两个缓冲区拿到近端与远端数据
- 把自研 AEC 算法移植进 SDK 的两种方式、接口契约与踩坑清单
- dump 出 mic 与喇叭两路 PCM 的两种方式、指令参数、文件落盘位置与解析脚本
- 不接手机、可反复复跑地验证 AEC 效果，并用 ERLE 量化出来

## 2. 前置要求

| 项目 | 要求 |
|------|------|
| 硬件 | eh-lb525 或本例程支持的其他板子（见 README 的支持平台列表） |
| SDK | SiFli-SDK，本例程路径 `main/example/bt/hfp` |
| 工具链 | scons + 对应 ARM 工具链；SifliTrace（串口 dump 用） |
| 手机 | 支持 HFP 的蓝牙手机，用于建立真实通话 |
| 基础 | 了解 RT-Thread 设备模型、环形缓冲（ringbuffer）基本概念 |
| 前置知识 | PCM 音频格式（16 bit / 单声道）、回声消除的基本原理（参考信号 + 近端信号） |

## 3. 例程怎么用

### 3.1 编译与烧录

在 `project` 目录执行：

```bash
cd main/example/bt/hfp/project
scons --board=eh-lb525 -j32
```

编译完成后进入生成的 `build_eh-lb525` 目录，运行下载脚本：

```bash
./uart_download.bat
```

按提示输入串口编号即可。烧录完成后复位，串口会打印系统启动日志。

### 3.2 用 finsh 命令建立通话

例程启动后默认已打开蓝牙，全部操作通过 `hfp_cmd` 命令完成：

| 步骤 | 命令 | 成功标志 |
|------|------|----------|
| 搜索手机 | `hfp_cmd start_inquiry` | 打印 `device [xxx] searched` 与设备地址 |
| 停止搜索 | `hfp_cmd stop_inquiry` | — |
| 连接手机 | `hfp_cmd hfp_connect xx:xx:xx:xx:xx:xx` | `HFP HF connected` |
| 拨打电话 | `hfp_cmd make_call 10086` | `make a call complete` |
| 接听来电 | `hfp_cmd answer_call` | `answer a call complete` |
| 建立通话音频 | `hfp_cmd audio_connect` | `HFP HF audio_connected` |
| 挂断 | `hfp_cmd handup_call` | `hangup a call complete` |
| 断开音频 | `hfp_cmd audio_disconnect` | `HFP HF audio_disconnected` |

⚠️ 注意：**只有 `audio_connect` 成功、SCO 链路建立之后，3A 才会真正启动**。没进入通话状态时调用 dump 命令抓不到任何数据，因为 `audio_3a_open()` 还没被调用、`is_need_3a` 还是 0。

### 3.3 确认 3A 已跑起来

通话建立后，串口应能看到 3A 初始化的日志（由 `audio_3a_open()` 打印）：

```text
3a_w open samplearate=16000
WebRtcNsx Init finish !
WebRtc Aecm Init finish !
WebRtc Agc Init finish !
g_ul_agc_compression_gain_db=18, g_dl_agc_compression_gain_db=10, g_agc_decay=100, g_echoMode=3
```

看到这几行，说明 NS / AECM / AGC 三个模块都已创建成功，后续 dump 才有意义。若只看到 `3a_w open` 而没有算法初始化日志，检查是否被 `PKG_USING_WEBRTC` 相关配置关掉了。

## 4. 3A 从哪里来：源码选择与配置

### 4.1 编译期选型

`middleware/audio/audio_voice_proc/SConscript` 用 Kconfig 宏在多个实现之间二选一（互斥，只编一个）：

| 条件宏 | 编译文件 | 说明 |
|--------|----------|------|
| `PKG_USING_3MICS` | 不编译 3A 文件 | 多麦方案走 `audio_server_3mics.c` |
| `PKG_USING_SOUNDPLUS` | `audio_3a_soundplus.c` | 第三方算法 |
| `PKG_USING_ANYKA` | `audio_3a_anyka.c` | 安凯算法（可跑在 ACPU） |
| `PKG_USING_WEBRTC` | `audio_3a.c` | **本工程使用的实现** |
| 以上均未开 | `audio_3a_dummy.c` | 空桩，除静音外不做任何处理 |

⚠️ 注意：你在目录里看到的 `audio_3a_dummy.c` **不是算法**，它所有接口都是空函数，只在没有选任何 3A 包时兜底。真正的回声消除在 `audio_3a.c` 里。

另外两个文件容易混淆：

- `audio_3a_webrtc_small_delay.c`：低延时变体，整个文件被 `#if 0` 屏蔽，当前不参与编译
- `audio_freqshift.c` / `audio_softeq.c`：防啸叫频移与软件 EQ，需 `AUDIO_FREQSHIFT` / `AUDIO_SOFTEQ` 才会编入，本工程未开启

### 4.2 本工程的实际配置

`project/proj.conf` 中与本主题相关的开关：

```ini
CONFIG_AUDIO=y
CONFIG_AUDIO_PATH_USING_HCPU=y
CONFIG_PKG_USING_WEBRTC=y       # 选中 audio_3a.c
CONFIG_WEBRTC_AECM=y            # 定点 AECM 回声消除
CONFIG_WEBRTC_AGC_FIX=y         # 上下行 AGC
# WEBRTC_ANS_FIX 未显式写，取 Kconfig choice 默认值，即开启定点 NS
CONFIG_BSP_UART1_TX_USING_DMA=y # console 串口 DMA，dump 必需
```

WebRTC 包内部还有若干细分选项（`external/WebRTC_audio_processing/Kconfig`）：

| 选项 | 默认 | 作用 |
|------|------|------|
| `WEBRTC_ANS_FIX` / `_FLOAT` / `_NULL` | `_FIX` | 噪声抑制实现，`_NULL` 表示关闭 |
| `WEBRTC_AECM` / `WEBRTC_AEC` / `_NULL` | `AECM` | 回声消除实现，`_NULL` 表示关闭 |
| `WEBRTC_AGC_FIX` | y | 自动增益 |
| `WEBRTC_RUN_IN_ACPU` | 依芯片而定 | 把 WebRTC 运算搬到 ACPU 执行 |

如果只想验证"关掉 AEC 是什么效果"，把 choice 改成 `WEBRTC_AEC_NULL` 即可，不需要改代码。

## 5. 数据路径全景

一通电话里数据分两条路走，最终在 `audio_3a_data_process()` 汇合。

### 5.1 下行（手机 → 喇叭）

```text
手机 HFP/SCO
  → BT 协议栈解码（mSBC / CVSD）
  → audio_bt_voice.c: audio_3a_downlink(fifo, size)
  → rbuf_dwlink 环形缓冲（按帧切分）
  → 下行 AGC
  → speaker_ring_put()
  → audio_server TX 环形缓冲
  → audio_server 取出 tx_data_tmp（叠加音量）
  → audio_3a_far_put(tx_data_tmp, tx_dma_size)
  → rbuf_far 环形缓冲
  → WebRtcAecm_BufferFarend()   ← 远端参考信号
  → 同时 bf0_audprc_device_write() 写到喇叭硬件
```

关键点在于 `audio_3a_far_put()` 拿到的**不是**蓝牙原始下行数据，而是经过下行 AGC、又叠加了用户音量的 `tx_data_tmp`——也就是**真正送到喇叭功放的那份数据**。这一点非常重要：AEC 要用"喇叭实际发出什么声音"作为参考，才能抵消麦克风拾取到的回声。

### 5.2 上行（mic → 手机）

```text
mic 采集（audprc RX，DMA 半满/全满中断）
  → audio_server.c: audio_3a_uplink(rx_data_tmp, 320, is_mute, is_bt_voice)
  → audio_3a_data_process()（每帧 320 字节 @16k）
      ├─ DC 直流校正
      ├─ NS 噪声抑制
      ├─ AECM 回声消除      ← 近端信号在此与远端参考相减
      ├─ 上行 AGC
      └─ RampOut
  → rbuf_out 环形缓冲
  → bt_voice_encode_process() 编码（mSBC / LC3）
  → 回传手机
```

### 5.3 帧长与缓冲约定

| 采样率 | 每帧字节 (frame_len) | 帧时长 | 每帧样点数 | 说明 |
|--------|---------------------|--------|-----------|------|
| 8000 Hz | 160 | 10 ms | 80 | 窄带通话（CVSD） |
| 16000 Hz | 320 | 10 ms | 160 | 宽带通话（mSBC），最常见 |
| 32000 Hz | 320 | 5 ms | 160 | 超宽带（LC3 SWB） |

`audio_3a_uplink()` 入口处有 `RT_ASSERT(fifo_size == 320)`，即**无论采样率，上层每次固定送 320 字节**；8k 情况下内部会拆成两次 160 字节分别处理。

三个环形缓冲的用途：

| 缓冲 | 生产者 | 消费者 | 容量 |
|------|--------|--------|------|
| `rbuf_far` | `audio_3a_far_put()`（TX 侧） | AEC 取参考帧 | 320 B（32k 为 480 B） |
| `rbuf_dwlink` | `audio_3a_downlink()`（BT 侧） | 下行 AGC | 1280 B（32k 为 1920 B） |
| `rbuf_out` | 3A 处理完的上行数据 | 编码器 | 320 B（32k 为 480 B） |

## 6. AEC 怎么拿到两路数据

这是移植和调试时最需要搞清楚的一段，两个入口一近一远。

### 6.1 远端参考：`audio_3a_far_put()`

调用者是 `audio_server.c` 的喇叭发送路径（`process_speaker_tx()`）。每送一帧给喇叭硬件之前调用一次：

```c
/* audio_server.c，写喇叭前 */
if (my->is_need_3a)
{
    audio_3a_far_put(my->tx_data_tmp, my->tx_dma_size);
}
bf0_audprc_device_write(my->audprc_dev, 0, my->tx_data_tmp, my->tx_dma_size);
```

`audio_3a_far_put()` 内部只做三件事：入 `rbuf_far`、按 `frame_len` 切帧、逐帧喂给算法并打点：

```c
/* audio_3a.c: audio_3a_far_put() */
if (rt_ringbuffer_space_len(p_3a_env->rbuf_far) >= fifo_size)
    rt_ringbuffer_put(p_3a_env->rbuf_far, fifo, fifo_size);

while (rt_ringbuffer_data_len(p_3a_env->rbuf_far) >= p_3a_env->frame_len)
{
    rt_ringbuffer_get(p_3a_env->rbuf_far, (uint8_t *)farframe, p_3a_env->frame_len);
    audio_dump_data(ADUMP_AECM_INPUT1, (uint8_t *)farframe, p_3a_env->frame_len);
    WebRtcAecm_BufferFarend(p_3a_env->aecmInst, (const int16_t *)farframe, p_3a_env->frame_len / 2);
    p_3a_env->is_far_putted = 1;   /* 标志位：远端已有数据，AEC 可以开始工作 */
}
```

三个要点：

1. **接入点是 `WebRtcAecm_BufferFarend()`**，自研算法把这里换成自己的 `xxx_Farend()` 即可
2. `frame_len / 2` 是**样点数**（字节数除以 2），别传错
3. `is_far_putted` 是"远端已就绪"的闸门，AEC 处理分支会先判断它，避免上电初期用空参考去消回声

### 6.2 近端：`audio_3a_uplink()` → `audio_3a_data_process()`

mic 数据由 `audio_server.c` 在每次 ADC 数据就绪时送入：

```c
audio_3a_uplink(my->rx_data_tmp, readlen, server->public_is_rx_mute, server->is_bt_3a);
```

`audio_3a_data_process()` 是整条链路的骨架，用 `SWAP_IN_OUT()` 在两个帧缓冲之间来回倒腾，顺序固定为 DC → NS → AECM → AGC → RampOut：

```c
/* 简化后的核心流程 */
memcpy(in, fifo, fifo_size);
audio_dump_data(ADUMP_AUDPRC, fifo, fifo_size);          /* 原始 mic 打点 */

DcCorrection_Process(dcInst, in, out, frame_len / 2);    /* DC 校正 */
audio_ans_proc(pNS_inst, in, out);                       /* NS 降噪 */
audio_dump_data(ADUMP_ANS_OUT, out, frame_len);

if (p_3a_env->is_far_putted && g_u16_test_aec)
{
    aec_input_para_t para = {
        .nearframe = in,          /* 降噪后的 mic：近端 */
        .nearframe_clean = NULL,
        .outframe = out,
    };
    audio_dump_data(ADUMP_AECM_INPUT2, in, frame_len);
    audio_aec_proc(aecmInst, &para, samplerate);          /* 回声消除 */
}
audio_dump_data(ADUMP_AECM_OUT, out, frame_len);

audio_agc_proc(agcInst, in, out, samplerate);             /* 上行 AGC */
RampOut_Process(rampOutInst, in, out, frame_len / 2);
rt_ringbuffer_put(rbuf_out, out, frame_len);
```

真正调用 WebRTC 的那一层：

```c
/* audio_3a.c: audio_aec_proc() */
int16_t audio_aec_proc(void *aecmInst, aec_input_para_t *para, uint16_t samplerate)
{
    uint16_t u16_frame_len = 80;
    if (16000 <= samplerate) u16_frame_len = 160;

    if (0 != WebRtcAecm_Process(aecmInst,
                                para->nearframe,        /* 近端（含回声） */
                                para->nearframe_clean,  /* 输出回声估计，可为 NULL */
                                para->outframe,         /* 消除后的结果 */
                                u16_frame_len,
                                g_aec_delay))           /* 近远端延时，单位 ms */
    {
        LOG_W("WebRtcAecm_Process  error !\n");
        return 1;
    }
    return 0;
}
```

### 6.3 两路数据是怎么对上的

| 机制 | 说明 |
|------|------|
| 同节拍产生 | 上下行都由音频 DMA 中断驱动，都是 10 ms 一帧 |
| 环形缓冲削峰 | `rbuf_far` / `rbuf_out` 吸收两侧调度抖动 |
| 延时补偿 | `g_aec_delay`（默认 10）告诉 AEC 参考信号相对近端滞后多少 ms |
| 就绪闸门 | `is_far_putted` 保证有参考数据后才开始消回声 |

调试时如果发现回声消不干净，`g_aec_delay` 是第一个该动的参数：它对应"从数据写入喇叭到被麦克风拾取"的声学+电路延时，不同腔体、不同喇叭功率差别很大，10 ms 只是通用起点。

## 7. 移植自己的 AEC 算法

### 7.1 两种方式

**方案 A：新增实现文件（推荐）**

新增 `audio_3a_myaec.c`，实现与 `audio_3a.c` 相同的一组接口，然后在 SConscript 加一个分支：

```python
# middleware/audio/audio_voice_proc/SConscript
if GetDepend('PKG_USING_3MICS'):
    src = []
elif GetDepend('PKG_USING_MYAEC'):
    src = Glob('audio_3a_myaec.c')
elif GetDepend('PKG_USING_SOUNDPLUS'):
    src = Glob('audio_3a_soundplus.c')
# ... 其余分支保持不变
```

再在 Kconfig 里加上 `PKG_USING_MYAEC` 选项，工程 `proj.conf` 中打开它、同时关掉 `CONFIG_PKG_USING_WEBRTC`。

优点：不动原文件，后续 SDK 升级不冲突；可随时切回 WebRTC 做 A/B 对比。

**方案 B：直接改 `audio_3a.c`**

只替换 `audio_aec_proc()` 内部实现，其余链路（NS / AGC / dump / 缓冲）全部复用。适合只想换 AEC 内核、保留其它处理的场景。改动量最小，但后续 SDK 升级需要重新合入。

### 7.2 接口契约

无论哪种方案，都必须实现下面这组函数（声明见 `middleware/audio/include/audioproc.h`）：

| 函数 | 调用者 | 调用上下文 | 职责 |
|------|--------|-----------|------|
| `audio_3a_open(audio_3a_input_t *input)` | `audio_server.c` 通话建立时 | 线程 | 按 `input->samplerate` 初始化算法，申请内存 |
| `audio_3a_close(void)` | 通话结束时 | 线程 | 释放全部资源 |
| `audio_3a_uplink(uint8_t *fifo, uint16_t len, uint8_t is_mute, uint8_t is_bt_voice)` | mic 数据就绪 | 音频线程，10 ms 一次 | 处理近端，结果写回 `fifo`；`is_mute` 时输出静音 |
| `audio_3a_downlink(uint8_t *fifo, uint16_t size)` | BT 下行解码后 | BT 语音线程 | 处理下行并 `speaker_ring_put()` 送喇叭 |
| `audio_3a_far_put(uint8_t *fifo, uint16_t size)` | 写喇叭前 | TX 发送路径 | **接收远端参考**，喂给算法 |
| `audio_3a_dnlink_buf_is_full(uint16_t size)` | 下行数据到来前 | BT 语音线程 | 流控，缓冲不够返回 1 |
| `audio_3a_set_bypass(uint8_t bypass, uint8_t mic, uint8_t down)` | 产测命令 | 线程 | 直通开关，旁路全部算法 |
| `audio_command_process(uint8_t *cmd)` | 调试通道 | 线程 | 参数下发入口，可留空 |

`audio_3a_input_t` 结构（新版接口，注意与旧版四参数原型不同）：

```c
typedef struct
{
    uint32_t samplerate;         /* 8000 / 16000 / 32000 */
    uint8_t  is_bt_voice;        /* 1: 蓝牙通话，输出要交给编码器 */
    uint8_t  disable_uplink_agc; /* 1: 关闭上行 AGC */
    uint8_t  all_mic_channels;   /* mic 通道数 */
    uint8_t  enable_mic_ssl;     /* mic 特定处理开关 */
} audio_3a_input_t;
```

### 7.3 移植要点与坑

1. **帧长是硬约束**：上层固定送 320 字节；内部按 `frame_len`（16k 为 320、8k 为 160）切帧。自研算法若不支持 10 ms 帧，需要在内部做缓存重排，不能直接改上游。
2. **样点数不是字节数**：所有算法 API 的长度参数都是 `int16` 样点数，等于字节数除以 2。
3. **不要 malloc**：`audio_3a_uplink()` 在音频线程里 10 ms 调用一次，函数内动态内存分配会造成抖动甚至丢帧，全部改在 `open()` 里预分配。
4. **上下文与耗时**：这段代码跑在音频服务线程，单帧预算 10 ms，WebRTC 定点 AECM 通常只占几百微秒。自研算法若复杂度高，建议开 `AUDIO_3A_STATIC_TIME` 用 `audio_tick_in/out` 打点，或把重活搬到 ACPU（参考 `WEBRTC_RUN_IN_ACPU` 的做法）。
5. **`is_far_putted` 闸门要保留**：否则上电前几十毫秒会用全零参考去消回声，反而把近端语音削掉。
6. **bypass 要能真直通**：`audio_3a_set_bypass(1, ...)` 必须让数据原样透传，产测和问题定位都靠它。
7. **保持 dump 打点**：把 `audio_dump_data()` 调用照抄进你的实现，否则后面没法用同一套工具抓数据；每个 `audio_dump_type_t` 只能被一路数据占用。
8. **ringbuffer 满要处理**：`rbuf_out` 满时丢帧会直接体现为上行断续，`LOG_I("3a_w rbuf_out full")` 是重要线索。
9. **32k 特例**：`frame_len` 仍是 320，但一帧只有 5 ms，`samplerate` 分支要单独验证。

### 7.4 移植后的验证顺序

1. 先 `audio_3a_set_bypass(1, 0, 0)` 验证通路（应能正常通话，无算法介入）；
2. 打开自研算法，dump `ADUMP_AECM_INPUT1` 与 `ADUMP_AECM_INPUT2`，确认两路数据都有、帧率一致；
3. 对比 `ADUMP_AECM_OUT`，用耳机/扬声器两种模式分别听感评估；
4. 最后再调 NS、AGC 的增益与门限。

## 8. 怎么 dump 出数据

SDK 内置了一套音频 dump 框架，在 3A 链路的每个节点都埋了打点，取数不需要自己写代码。

### 8.1 两种方式怎么选

| 维度 | 串口方式（默认） | 文件方式 |
|------|-----------------|---------|
| 开关 | `#define AUDIO_DATA_CAPTURE_UART` | 再加 `#define AUDIO_DATA_CAPTURE_FILE` |
| 输出 | 实时打到 console 串口，由 SifliTrace 接收 | 写入设备文件系统 |
| 依赖 | console 串口 TX DMA + finsh | 追加依赖 DFS + 可写文件系统（建议 NAND） |
| 优点 | 无需文件系统，边跑边看 | 不受串口带宽限制，可长时间录 |
| 缺点 | 带宽受限，dump 期间看不到 log | 需要导出文件，nor flash 速度可能不够 |

两个宏都在 `middleware/audio/audio_manager/audio_server.c` 顶部（约 72 行）：

```c
#define AUDIO_DATA_CAPTURE_UART
#define AUDIO_DATA_CAPTURE_FILE
```

⚠️ 注意：**切到文件方式时，`AUDIO_DATA_CAPTURE_UART` 必须保留**。整个 `audio_data` 命令包在 `#if defined (AUDIO_DATA_CAPTURE_UART) && defined (RT_USING_FINSH)` 里，把它注释掉 finsh 里就没有这个命令，执行时会提示找不到指令。文件方式靠 `AUDIO_DATA_CAPTURE_FILE` 让 dump 目标从串口切到文件，不需要牺牲命令本身。

### 8.2 可用指令与参数

`audio_data` 命令的参数与数据类型一一对应：

| 参数 | 数据内容 | 对应位置 |
|------|----------|----------|
| `-audprc` | 原始 mic 采集数据（进算法前） | `audio_3a_data_process()` 入口 |
| `-downlink` | 蓝牙下行原始数据（解码后） | `audio_3a_downlink()` |
| `-downlink_agc` | 下行 AGC 后数据（= 送喇叭前） | `audio_3a_downlink()` 尾部 |
| `-aecm_input1` | **喇叭参考信号（AEC 远端）** | `audio_3a_far_put()` |
| `-aecm_input2` | **AEC 前 mic 数据（AEC 近端）** | AEC 调用前 |
| `-aecm_out` | AEC 消除后的上行数据 | AEC 调用后 |
| `-ans_out` | NS 降噪后数据 | NS 之后 |
| `-agc_out` | 上行 AGC 后数据 | AGC 之后 |
| `-ramp_out_out` | 最终送 BT 编码的数据 | RampOut 之后 |
| `-dc_out` | DC 校正后数据 | DC 之后 |
| `-pdm` | PDM 麦克风原始数据 | `audio_3a_save_pdm()` |
| `-stop` / `-off` | 停止 dump，恢复 log | — |

常用组合：

```bash
# AEC 近端 + 远端参考（做离线 AEC 分析的标准两路）
audio_data -aecm_input1 -audprc

# 看 AEC 前后效果
audio_data -aecm_input1 -aecm_out

# 看喇叭实际播放的数据 + 原始 mic
audio_data -downlink_agc -audprc

# 三路（注意串口带宽）
audio_data -downlink -downlink_agc -aecm_out
```

### 8.3 串口方式操作步骤

1. 确认 `CONFIG_BSP_UART1_TX_USING_DMA=y`（本工程已开）；关掉 BT HCI log 释放串口带宽：
   ```bash
   nvds update hci_log 0
   ```
   重启设备。
2. 建立通话（见 3.2 节），确认 3A 已初始化。
3. 打开 SifliTrace，选择 console 串口并连接，选择音频功能。
4. 输入 dump 命令，例如：
   ```bash
   audio_data -aecm_input1 -audprc
   ```
   串口会回应：
   ```text
   dump enable: input1
   dump enable: audprc
   audio_data_start: enable 2 2848
   ```
   此时系统 log 暂停（正常现象），数据开始往串口灌。
5. 录够时长后停止：
   ```bash
   audio_data -stop
   ```
6. 在工具里点保存，数据落在 SifliTrace 的 log 目录下 `xxx_audio/` 子目录，工具已按类型拆好：
   ```text
   <SifliTrace>/log/2026xxxx_audio/
   ├── xxx_audprc.pcm
   └── xxx_aecm_input1.pcm
   ```

### 8.4 串口的数据帧格式

串口上跑的是一条复用流，每帧前面带 12 字节头，工具依据 `type` 拆包：

```c
typedef struct
{
    uint32_t magic;   /* "AUDI"，小端值 0x49445541 */
    uint16_t type;    /* 见 8.2 参数与枚举的对应关系 */
    uint16_t len;     /* 本帧字节数，16k 时 320 */
    uint32_t no;      /* 该类型自己的帧序号 */
    uint8_t  data[320];
} audio_data_t;       /* 头长 AUDIO_DATA_HEADER_LEN = 12 */
```

### 8.5 文件方式操作步骤

> **前提：FS 分区已挂载（NOR / NAND 均可）。** 挂载代码 `mnt_init()` 位于 `main.c`。hfp 例程原始版本只写了 NAND 分支（要求 `BSP_USING_SPI_NAND`），在 `sf32lb57-lcd_n16r16_qspi` 这类 NOR 板上会整段被裁掉；现已改为 NOR/NAND 自适应写法（`RT_USING_MTD_NOR` 时用 `register_nor_device()`，与 `music_source` 例程一致），FS 分区大小由工程分区表 `ptab.h` 的 `FS_REGION_START_ADDR` / `FS_REGION_SIZE` 决定（57 工程为 4 MB）。
>
> **NOR 上跑 FAT 的两个注意点**：① 官方建议 NAND 是因为 NOR 上每写一批数据要"读改写 + 擦除"同一些 FAT/目录扇区，磨损放大明显——用来调试 dump 完全够用，但别当量产方案；② dump 是在音频线程里同步写的，FAT 扇区擦除（典型 45 ms/4 KB）瞬间可能造成音频短暂卡顿或 `AE` 空转计数增加，三路齐抓时更明显，建议先单路验证。若 `dump open xxx error` 之外还出现 `reserved space error`，是 `F_RESERVE_CONT_SPACE` ioctl 在 elm 文件系统上不支持所致，无害。

1. 修改 `audio_server.c` 顶部两个宏（两个都要开）：
   ```c
   #define AUDIO_DATA_CAPTURE_UART
   #define AUDIO_DATA_CAPTURE_FILE
   ```
2. 重新编译烧录。开机 log 确认文件系统已挂载：
   ```text
   mount fs on flash to root success
   ```
   未见此行说明 `BSP_USING_SPI_NAND` / `RT_USING_DFS` 未生效，文件写不出来。
3. 通话中执行同样的命令：
   ```bash
   audio_data -downlink_agc -audprc
   ```
4. 数据直接落到根目录，**每一路一个独立文件，没有 12 字节包头，纯 PCM**：

   | 数据类型 | 文件路径 |
   |----------|----------|
   | 原始 mic | `/dump_audprc.pcm` |
   | 喇叭参考（AEC input1） | `/dump_aecm_input1.pcm` |
   | AEC 前 mic（input2） | `/dump_aecm_input2.pcm` |
   | AEC 输出 | `/dump_aecm_out.pcm` |
   | 下行原始 | `/dump_downlink.pcm` |
   | 下行 AGC 后 | `/dump_downlink_agc.pcm` |
   | NS 输出 | `/dump_ans_out.pcm` |
   | 上行 AGC 后 | `/dump_agc_out.pcm` |
   | 送编码前 | `/dump_ramp_out_out.pcm` |
   | DC 校正后 | `/dump_dc_out.pcm` |

5. 录制时长由 `g_record_time` 控制，**默认 10 秒**（`audio_3a.c` 中 `uint32_t g_record_time = 10;`）。`max_frame = g_record_time * 1024 / 10`，16k 下约 1024 帧、10.24 秒。要录更长，直接改这个默认值再编译；当前版本里 `audio_command_process()` 的 `time=` 解析被 `#if 0` 屏蔽，运行时改不了。
6. 到时间自动关闭文件，也可以手动 `audio_data -stop` 提前结束。手动停止且文件仍打开时，每个文件尾部会追加 12 字节记录 `{0x55667788, cur_frame, max_frame}`，分析时若长度不是帧长整数倍，先砍掉末尾 12 字节。
7. 文件导出：`ls /` 查看，建议用 SifliTrace 的文件读取或 SifliBleApp 的 GET DUMP 拉取。不建议直接 `cat`——二进制数据经 console 打印既慢又容易和 log 混淆。

### 8.6 离线解析脚本

串口文件是带头的复用流，用下面脚本拆成两路；文件方式的产物已经是纯 PCM，可跳过：

```python
import struct

raw = open("dump.bin", "rb").read()
out = {0: open("downlink.pcm", "wb"),
       1: open("downlink_agc.pcm", "wb"),
       2: open("audprc.pcm", "wb"),
       5: open("aecm_input1.pcm", "wb"),
       6: open("aecm_input2.pcm", "wb"),
       7: open("aecm_out.pcm", "wb")}
i = 0
while i + 12 <= len(raw):
    magic, typ, ln, no = struct.unpack_from("<IHHI", raw, i)
    if magic != 0x49445541:      # "AUDI"
        i += 1                   # 串口丢字节时重同步
        continue
    i += 12
    if typ in out:
        out[typ].write(raw[i:i + ln])
    i += ln
```

PCM 参数：16000 Hz（8k 场景为 8000 Hz）、16 bit、单声道、小端。用 Audacity 时选 File → Import → Raw，编码 Signed 16-bit PCM、字节序 Little-endian、声道 Mono。

### 8.7 dump 常见问题

| 现象 | 原因与处理 |
|------|-----------|
| `audio_data` 提示找不到指令 | `AUDIO_DATA_CAPTURE_UART` 被注释掉了，或 `RT_USING_FINSH` 未开 |
| 执行命令后设备断言挂死 | console 串口没开 TX DMA，`audio_data_start()` 里的 `RT_ASSERT` 触发 |
| 数据断断续续、帧号跳变 | 波特率不足（默认 1M），减一路数据或把 `RT_SERIAL_DEFAULT_BAUDRATE` 提到 1.5M |
| 抓不到 `aecm_input1` | 没真正进入通话（`audio_3a_far_put()` 未被调用），确认 SCO 已建立 |
| 文件方式没有任何文件 | 文件系统没挂上，看开机 log 是否有 `mount fs on flash to root success` |
| dump 期间收不到 log | 正常，dump 开始时会 `log_pause(true)`，`-stop` 后恢复 |
| 设备进睡眠后 dump 中断 | dump 必须保持运行态，注意关闭低功耗策略 |

## 9. 怎么验证 AEC 效果

前面几章讲的是"数据怎么流"，这一章讲"效果怎么量"。直接打一通电话听回声，是所有验证手段里最不可控的一种；下面先说明它的问题，再给出一条不用连手机、可反复复跑的本地环回方案。

### 9.1 真机通话的四个硬伤

| 问题 | 具体表现 | 后果 |
|------|----------|------|
| 依赖真实通话 | `is_need_3a` 要 SCO 建立后才置 1 | 每次测试都要连手机、拨号，无法自动化 |
| 远端信号不可控 | 对方说什么、多大声、何时开口全随机 | 同一组参数没法复跑对比 |
| 手机侧二次处理 | 上行还要过对方手机的降噪 / AGC | 听到的是"本机 AEC → 手机 3A"的叠加结果，出问题分不清责任 |
| 无法量化 | 只能靠耳朵判断"好像还有一点回声" | 说不出 ERLE 是多少 dB、多久收敛、双讲损伤多大 |

真机通话依然要做，但它适合**验收**，不适合**调参**。

### 9.2 三条测试路径

| 路径 | 真实性 | 可控 / 可复现 | 能出指标 | 改动量 | 适用场景 |
|------|--------|--------------|----------|--------|----------|
| A 真实通话 + dump | 最高 | 差 | 可以（离线算） | 0 | 验收、真实手机链路对比 |
| **B 本地环回注入** | 中（真喇叭、真 mic、真声学路径，只是远端信号可控） | 高 | 可以 | 小 | **日常调参、回归** |
| C 纯离线（PC 跑同一算法） | 低（没有硬件与时序效应） | 最高 | 可以 | 需自己做 | 算法迭代、快速对比 |

推荐组合：**日常用 B 调，发版前用 A 验，算法本身用 C 排错**。

### 9.3 本地环回为什么能跑 3A

关键点：3A 的开关只认 `is_need_3a`，与音频类型、有没有蓝牙链路无关。

| 位置 | 代码 | 作用 |
|------|------|------|
| `audio_server.c` `audio_open()` | `if (audio_type == AUDIO_TYPE_BT_VOICE \|\| parameter->is_need_3a) device = AUDIO_DEVICE_SPEAKER;` | 本地类型也能落到 speaker + mic 这套设备上 |
| `audio_server.c` speaker TX | `if (my->is_need_3a) audio_3a_far_put(my->tx_data_tmp, my->tx_dma_size);` | **任何**写往喇叭的数据都会成为 AEC 远端参考 |
| `audio_server.c` mic RX | `if (my->is_need_3a) audio_3a_uplink(...)`；`server->is_bt_3a == 0` 时回调 `as_callback_cmd_data_coming` | mic 进 3A，处理结果交回应用层 |

所以只要 open 时把 `parameter.is_need_3a` 置 1：

- 用 `audio_write()` 往喇叭写一段 wav，这段声音既被放出来（形成真实回声），又被当作 far-end 参考；
- mic 采集到的"回声 + 本地说话"经 AEC/NS/AGC 处理后，通过回调返回；
- 全程不需要蓝牙、不需要拨号。

一个附带好处：`audio_3a_far_put()` 取的是**加过音量、真正写进 audprc 的数据**，比拿蓝牙原始下行数据当参考更贴近实际。

### 9.4 最简落地：改一行 + 一条命令

SDK 里已经有"一边播一边录"的现成实现：`middleware/audio/audio_local_music/audio_test_api.c` 的 `record_start()`，只差 3A 开关没打开。

**第 1 步**：工程配置里打开测试 API（hfp 工程默认没开）

```conf
CONFIG_PKG_USING_AUDIO_TEST_API=y
```

**第 2 步**：`record_start()` 里加一行

```c
static void record_start(void)
{
    ...
    pa.write_cache_size = WRITE_CACHE_SIZE;
    pa.is_need_3a = 1;          // ← 唯一改动：本地环回也要跑 3A
    if (g_playing)
        rwflag = AUDIO_TXRX;    // 边播边录
    g_client = audio_open(AUDIO_TYPE_LOCAL_RECORD, rwflag, &pa,
                          audio_callback_record_start, (void *)fd);
}
```

**第 3 步**：准备素材，放一段 16 kHz / 16 bit / 单声道人声到 `/dyn/16k.pcm`（文件名由 `FILE_PLAYING_WHEN_RECORD` 宏决定，可自行改）。

**第 4 步**：通话场景下执行

```
audio_data -audprc -aecm_input2 -aecm_out
audio_test_cmd record 1 0
```

第二个参数传 `0`，关掉测试 API 自带的 dump（它只会抓 `ADUMP_AUDPRC`），改用 `audio_data` 一次抓三路。录制 6 秒后自动结束（`RECORD_TIME_SECOND`）。

> 两个小坑：`/dyn` 目录必须在文件系统中已存在（部分板子只有根目录，此时把 `FILE_PLAYING_WHEN_RECORD` 宏改成 `/16k.pcm` 这类可直接落盘的路径）；素材播完不会循环，所以素材时长应不短于测试时长。

> 注意：`audio_test_api.c` 初始化时会把 `g_test_3a_enable = 1`，但它**只影响下行 AGC**（`audio_3a.c` 里 `audio_test_api_3a_is_enable()` 只是把 `g_u16_test_agc` 置 1），不是 AEC 的开关。别误以为这条命令已经在跑 AEC。

### 9.5 自建 `aec_test` 命令（推荐）

`audio_test_cmd` 跟音乐播放测试耦合得比较紧，参数也绕。如果要长期用，建议写一个独立命令，行为更明确：

```c
#include <rtthread.h>
#include <string.h>
#include <dfs_posix.h>
#include "audio_server.h"

static audio_client_t s_cli;
static int  s_fd_ref = -1, s_fd_out = -1;
static int16_t s_buf[2048];

static int aec_test_cb(audio_server_callback_cmt_t cmd, void *user, uint32_t reserved)
{
    if (cmd == as_callback_cmd_cache_half_empty || cmd == as_callback_cmd_cache_empty)
    {
        if (s_fd_ref >= 0 && s_cli)
        {
            int n = read(s_fd_ref, s_buf, sizeof(s_buf));
            if (n > 0)
                audio_write(s_cli, (uint8_t *)s_buf, n);
        }
    }
    else if (cmd == as_callback_cmd_data_coming)
    {
        audio_server_coming_data_t *p = (audio_server_coming_data_t *)reserved;
        if (s_fd_out >= 0)
            write(s_fd_out, p->data, p->data_len);
    }
    return 0;
}

static int aec_test(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "stop"))
    {
        if (s_cli)        { audio_close(s_cli); s_cli = NULL; }
        if (s_fd_ref >= 0) { close(s_fd_ref); s_fd_ref = -1; }
        if (s_fd_out >= 0) { close(s_fd_out); s_fd_out = -1; }
        return 0;
    }
    if (argc < 2)
    {
        rt_kprintf("usage: aec_test <ref.pcm> [out.pcm] | aec_test stop\n");
        return -1;
    }

    s_fd_ref = open(argv[1], O_RDONLY | O_BINARY);
    if (s_fd_ref < 0) { rt_kprintf("open %s failed\n", argv[1]); return -1; }
    s_fd_out = (argc >= 3) ? open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_BINARY) : -1;

    audio_parameter_t pa = {0};
    pa.write_samplerate    = 16000;
    pa.write_channnel_num  = 1;
    pa.write_bits_per_sample = 16;
    pa.read_samplerate     = 16000;
    pa.read_channnel_num   = 1;
    pa.read_bits_per_sample  = 16;
    pa.write_cache_size    = 8192;
    pa.is_need_3a          = 1;      // 关键：打开 3A

    s_cli = audio_open(AUDIO_TYPE_LOCAL_RECORD, AUDIO_TXRX, &pa, aec_test_cb, NULL);
    rt_kprintf("aec_test started, ref=%s\n", argv[1]);
    return 0;
}
MSH_CMD_EXPORT(aec_test, local AEC loopback test);
```

用法：

```
aec_test /dyn/far_16k.pcm /dyn/aec_out.pcm     # 开始
audio_data -audprc -aecm_input2 -aecm_out      # 同时抓三路
aec_test stop                                  # 结束
```

`/dyn/aec_out.pcm` 是 AEC 处理后的上行数据，可以直接试听"回声被压掉多少"。

### 9.6 指标怎么算

先搞清楚每个 dump 点在链路中的位置（都在 `audio_3a.c`）：

| dump 类型 | 代码位置 | 含义 | 用途 |
|-----------|----------|------|------|
| `ADUMP_AUDPRC` | `audio_3a_data_process()` 入口 | 原始 mic，含回声 + 噪声 | 整链抑制量 |
| `ADUMP_DC_OUT` | DC 抑制后 | 去直流 | 一般不看 |
| `ADUMP_AECM_INPUT1` | `audio_3a_far_put()` / 处理内 | 送 AEC 的远端参考 | 对齐、画语谱 |
| `ADUMP_ANS_OUT` | NS 之后 | 降噪后 | 看 NS 贡献 |
| `ADUMP_AECM_INPUT2` | AEC 之前 | **纯 AEC 输入** | 算纯 AEC 抑制量 |
| `ADUMP_AECM_OUT` | AEC 之后、AGC 之前 | **纯 AEC 输出** | 算纯 AEC 抑制量 |
| `ADUMP_AGC_OUT` | 上行 AGC 后 | 最终送编码 | 听感、电平检查 |

两个指标要分清：

- **纯 AEC 抑制量（ERLE）** = `10·log10( P(aecm_input2) / P(aecm_out) )` —— 只反映 AEC 本身的贡献；
- **整链抑制量** = `10·log10( P(audprc) / P(aecm_out) )` —— 包含 NS，数值会明显偏高，别拿它当 AEC 的性能指标。

```python
import numpy as np

def load(p):
    return np.fromfile(p, dtype="<i2").astype(np.float64)

ref = load("dump_aecm_input2.pcm")
out = load("dump_aecm_out.pcm")
n = min(len(ref), len(out))
ref, out = ref[:n], out[:n]

W = 1600                                  # 100 ms 窗 @ 16 kHz
for i in range(0, n - W, W):
    pr = np.mean(ref[i:i+W] ** 2)
    po = np.mean(out[i:i+W] ** 2)
    if pr > 100:                          # 远端静音段不计入
        print(f"{i/16000:6.2f}s  {10*np.log10(pr/max(po,1)):6.1f} dB")
```

两个容易踩的点：

1. `aecm_out` 相对 `aecm_input2` 有算法内部延时，逐窗直接比会低估抑制量。做精确评估时先用互相关估出延时，平移后再算。
2. 远端静音段（没在播放）不该计入 ERLE，用 `pr > 100` 之类的门限过滤掉。

### 9.7 素材与用例设计

| 素材 | 时长 | 看什么 |
|------|------|--------|
| 单人连续语音 | 10 s | 收敛速度：前 1 s 内 ERLE 能否爬到 15 dB 以上 |
| 双讲片段（板子播远端人声，同时有人对板子说话） | 10 s | 近端损伤：AEC 不该把本地语音一起吃掉 |
| 白噪 / 对数扫频 | 5 s | 抑制带宽：高频段是否也压得住 |
| 静音 + 拍手 | 5 s | 双讲检测与发散后能否恢复 |

顺序建议：先用 1 kHz 单音确认链路通（`audio_test_cmd 1k 16000 0`），再上人声。单音只用来验通路，不要用它评估 NS 效果——单音会被 NS 当作稳态噪声直接压掉。

## 10. 常见问题 FAQ

**Q：为什么 `audio_3a_dummy.c` 里全是空函数？**
A：它是编译兜底实现，只有在没选任何 3A 包时才会被编入。本工程选了 `PKG_USING_WEBRTC`，实际编译的是 `audio_3a.c`。

**Q：AEC 参考信号为什么不用蓝牙下行原始数据？**
A：参考必须是"喇叭真正发出的声音"。下行原始数据没经过 AGC 和音量调节，幅度与实际播放不一致，用它做参考会导致自适应滤波器收敛到错误的增益上。

**Q：`audio_3a_uplink()` 里为什么要断言 320 字节？**
A：这是上层音频服务的固定节拍，与采样率无关。8k 时内部拆成两帧 160 字节处理，改动这个约定会破坏整个上下行时序。

**Q：能否只替换 AEC、保留 NS 和 AGC？**
A：可以。方案 B，仅替换 `audio_aec_proc()` 内部实现，NS（`audio_ans_proc`）与 AGC（`audio_agc_proc`）保持不动即可。

**Q：dump 出来的两路数据能直接对齐做 AEC 吗？**
A：能，但要注意两点：两路的帧序号 `no` 是各自独立计数的，不能直接比较；真正的时间关系由 `g_aec_delay` 描述，离线处理时先用互相关估一下实际延时再验证。

## 11. 关键代码位置索引

| 功能 | 文件 | 关键函数 / 行号参考 |
|------|------|-------------------|
| 3A 实现选型 | `middleware/audio/audio_voice_proc/SConscript` | Kconfig 分支 |
| WebRTC 3A 主流程 | `middleware/audio/audio_voice_proc/audio_3a.c` | `audio_3a_data_process()` |
| AEC 初始化与调用 | 同上 | `audio_aec_init()` / `audio_aec_proc()` |
| 远端参考入口 | 同上 | `audio_3a_far_put()` |
| 近端入口 | 同上 | `audio_3a_uplink()` |
| 接口声明 | `middleware/audio/include/audioproc.h` | `audio_3a_input_t` / 各接口原型 |
| dump 框架实现 | `middleware/audio/audio_manager/audio_server.c` | `audio_dump_data()` / `audio_data_cmd()` |
| dump 宏开关 | 同上，约 72 行 | `AUDIO_DATA_CAPTURE_UART` / `_FILE` |
| 喇叭数据送入参考 | 同上 | `process_speaker_tx()` 内 `audio_3a_far_put()` |
| mic 数据送入处理 | 同上 | `audio_3a_uplink()` 调用处 |
| 本地环回测试实现 | `middleware/audio/audio_local_music/audio_test_api.c` | `record_start()` / `audio_test_cmd` |
| 测试 API 配置项 | `middleware/audio/Kconfig` | `PKG_USING_AUDIO_TEST_API` |
| 官方 dump 文档 | `main/docs/source/zh_CN/audio/audio_dump.md` | — |

## 元信息

- 目标读者：中级嵌入式音频开发工程师
- 前置知识：RT-Thread 设备模型、PCM 音频基础、回声消除基本原理
- 技术栈版本：SiFli-SDK（middleware/audio 2026-11 版本），例程 `example/bt/hfp`，WebRTC 定点 AECM/NS/AGC
- 预计阅读时间：35 分钟
- 验证环境：eh-lb525（sf32lb52x），16 kHz 宽带通话
