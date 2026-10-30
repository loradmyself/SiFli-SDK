# USB_Display 性能优化实现文档

> 目标平台：SF32LB586 NAND 板（`sf32lb58-lcd_a128r32n1_qspi`）+ ST7265 800x480 DPI 屏
> 当前状态：扩展/复制模式显示正常，`lcd_ctrl fps 1` 实测约 **24 fps**
> 文档整理日期：2026-10-30

---

## 1. 当前架构与数据流

```
PC (Windows IDD 驱动, 60Hz 推流)
   │  USB HS Bulk (OUT 0x02)，MPS 512，MUSB DMA 已开启
   ▼
CherryUSB display 类驱动 (usbd_display.c)
   │  16KB 分包接收，短包判帧；帧头 16 字节 {crc16,type,cmd,x,y,w,h,frame_id:10,payload_total:22}
   │  chry_mempool 队列（当前 2 缓冲，PSRAM，每帧 770048 字节）
   ▼
main.c 主循环（轮询 mdelay(1)）
   │  解析帧头 → CPU memcpy 按行合成进 screen_fb（PSRAM, 768KB）
   │  → SCB_CleanDCache_by_Addr 清脏区
   ▼
drv_lcd (LCD task)
   │  draw_rect(0,0,799,479) —— 同步等待
   ▼
LCDC (DPI, PCLK 30MHz) ──► ST7265 面板
```

### 每帧耗时拆解（24fps ≈ 41.7ms 的来源）

| 环节 | 耗时 | 说明 |
|---|---|---|
| USB 收帧 768KB | ~22ms（与处理重叠） | 18.4MB/s，HS Bulk 上限 35~40MB/s，**未到瓶颈** |
| CPU memcpy 合成 | ~4-8ms | PSRAM→PSRAM，WB 缓存 |
| 清 cache | ~1-2ms | 768KB 脏区 |
| **draw_rect 整屏推送** | **~17-35ms（阻塞）** | draw_core 等 XferCplt = 等当前 DPI 扫描结束（一帧扫描 ~17.5ms），**最大头** |

**理论天花板**：RGB565 全帧方案的极限 = USB 带宽 ÷ 768KB ≈ **45~50fps**，不改协议永远到不了 60fps。

---

## 2. 优化路线（按投入产出排序）

### 第一步：去掉每帧整屏 draw_rect（预期 24 → 40+ fps）

**原理**：LCDC 视频接口（DPI）持续扫描，图层只需初始化时配置**一次**，之后每帧自动从 `screen_fb` 重新读数。

**改法**：
- `main()` 初始化后调用一次 `rt_graphix_ops(lcd_dev)->draw_rect(screen_fb, 0, 0, 799, 479)`，让 LCDC 指向 screen_fb；
- 主循环 `display_frame()` 里**删除最后一步整屏 draw_rect**，只保留"合成 + 清 cache"；
- LCDC 自动把 fb 的更新扫到屏上。

**代价**：合成写入与 LCDC 扫描之间无同步 → **撕裂**。小脏矩形（几 KB，微秒级写入）无感；全帧合成（768KB，几 ms）偶发撕裂线。

**验证**：`lcd_ctrl fps 1` 看帧率跳变；桌面快速拖动窗口看撕裂程度。

### 第二步：合成改用 EPIC 硬件 2D 引擎（CPU 占用 → 0）

**原理**：58x 自带 EPIC blitter（2D DMA，支持图层坐标搬运 + RGB565 格式转换），API 在 `drivers/Include/bf0_hal_epic.h`：
- `HAL_EPIC_Init` / `HAL_EPIC_BlendDataInit` / `HAL_EPIC_BlendStart`
- 纯矩形拷贝场景用 Blend（fg 层 = USB 帧矩形，bg 层 = screen_fb 目标区）或 Fill 变体

**改法**：
- 初始化时 `HAL_EPIC_Init`；
- `display_frame()` 里把按行 memcpy 换成一次 `HAL_EPIC_BlendStart`（异步），完成回调（信号量）后再清 cache / 通知；
- 主循环变成"收帧 → 提交 EPIC → 等完成 → 释放帧"，期间 USB 中断照常收下一帧。

**收益**：CPU 合成耗时归零、撕裂窗口大幅缩小、HCPU 占用显著下降。

### 第三步：切 JPEG 协议（60fps 的正解，结构性改造）

**原理**：PC 驱动（xfz1986 DLL）支持 `Ejpg<quality>` 编码（产品字符串解析已验证），每帧 JPEG 约 40~80KB（RGB565 的 1/10~1/15）；58x 有硬件 JPEG 解码器（`drivers/hal/bf0_hal_jpegd.c`，`HAL_JPEGD_Decode` 异步 DMA + 完成回调）。

**改法**：
1. `usb_display_device.c` 产品字符串改 `"sifli_R800x480_Ejpg80"`，**拔插 USB** 让 Windows 重读；
2. 帧池缩小（每帧 128KB 足够，缓冲数提到 3~4）；
3. main.c 收到帧后走 `HAL_JPEGD_Init + HAL_JPEGD_Decode`（配置输出直写 screen_fb 或中间 YUV 缓冲再转 RGB565）；
4. 解码完成回调里清 cache / 触发图层切换。

**注意**：
- JPEGD 输出格式通常是 YUV420，需要确认 `HAL_JPEGD_GetOutputSize` 的输出布局，RGB565 转换可用 EPIC 顺带完成（EPIC 支持 YUV→RGB 转换则一步到位，需查 `bf0_hal_epic.h` 的格式枚举）；
- 类驱动 `usbd_display.c` 对 jpg 帧的处理：`frame_format = header->type`，应用层据此分支。

**收益**：USB 带宽压力消失（3~5MB/s），60fps 不再受传输限制；CPU 几乎零负载。

### 目标形态链路

```
USB 收 JPEG (几 ms) → mempool → JPEGD 硬件解码 (DMA) → EPIC 转换/合成 → LCDC 自扫输出
     HCPU 只做调度，三个硬件引擎并行
```

| 环节 | 现在 | 目标 |
|---|---|---|
| 传输 | RGB565，46MB/s @60fps | JPEG，3~5MB/s |
| 解码/合成 | CPU memcpy | JPEGD / EPIC DMA |
| 显示 | 每帧同步等扫描 | 图层一次配置，LCDC 自扫 |
| HCPU 占用 | 高 | < 10% |

---

## 3. 内存布局规划

58x 资源（`drivers/cmsis/sf32lb58x/mem_map.h`）：

| 区域 | 容量 | 说明 |
|---|---|---|
| HCPU SRAM (HPSYS RAM0-9) | 1.5MB | 0x20000000 起，其中 512KB 留给 ACPU |
| LCPU SRAM (LPSYS) | 1MB | LCPU 私有 |
| PSRAM | 32MB | 当前帧池所在（`L2_RET_BSS_SECT`） |

**建议布局**：

| 数据 | 位置 | 理由 |
|---|---|---|
| `screen_fb` 768KB | HCPU SRAM（预算允许时） | LCDC 直读，带宽最高；标准 SiFli UI 应用 fb 就在 HPSYS RAM。放之前用 `free`/`list_memheap` 确认堆余量 |
| USB 帧池 ×2~3 | PSRAM（维持现状） | DMA 目标，容量大，cache 维护钩子已验证 |
| JPEG 解码工作区 | SRAM | JPEGD 高频访问 |
| JPEG 帧池（切 JPEG 后） | PSRAM，单帧 128KB ×4 | 吸收 PC 推流抖动 |

**cache 一致性提醒**（无论 SRAM 还是 PSRAM）：
- USB 写入 → CPU 读：MUSB glue 已做 invalidate（`usb_glue_sifli.c`）；
- CPU 写 fb → LCDC 读：必须 `SCB_CleanDCache_by_Addr`（当前实现已做，改造时别丢）；
- EPIC/JPEGD 写 fb → LCDC 读：DMA 引擎不经过 CPU cache，无需 clean（反而要确认 EPIC 源缓冲在 EPIC 读之前被 clean）。

---

## 4. 多核分工（HCPU / LCPU / ACPU）

| 核 | 本项目角色 | 说明 |
|---|---|---|
| **HCPU** | USB 中断 + 帧调度 + 图层管理 | 唯一合理宿主：USB/LCDC/EPIC/JPEGD 都在 HCPU 域 |
| **LCPU** | 保持默认（BLE 协议栈） | 访问 HCPU 域外设需走 IPC，搬任务成本 > 收益 |
| **ACPU** | 空闲（音频 DSP） | 不适合显示任务 |

**结论**：这个场景"发挥多核"的正确姿势是**让 DMA 引擎干活、CPU 只做调度**——EPIC/JPEGD/LCDC 三引擎并行后，HCPU 占用可降到 10% 以下，无需跨核搬代码。

---

## 5. 工程结构优化

1. **`src/usb_display_config.h`（新增，唯一事实来源）**
   ```c
   #define UDISP_WIDTH     800
   #define UDISP_HEIGHT    480
   #define UDISP_BPP       2
   #define UDISP_PRODUCT   "sifli_R800x480_Ergb16"   /* 切 JPEG 时改 Ejpg80 */
   #define UDISP_FRAME_CNT 2                          /* 帧池缓冲数 */
   ```
   `usb_display_device.c` / `main.c` 全部引用它，消除"三处手改"的老问题。

2. **模块拆分**
   ```
   src/
   ├── usb_display_config.h
   ├── usb_display_device.c      /* USB 传输层：描述符/帧池/收发 */
   ├── udisp_codec.c             /* 编解码：RGB 直通 或 JPEG 解码（编译期切换） */
   ├── udisp_compositor.c        /* 合成：memcpy 或 EPIC */
   └── main.c                    /* 调度 + 统计 */
   ```

3. **事件驱动**：USB 帧到达回调 `rt_sem_release`，替代 `mdelay(1)` 轮询，降低延迟与空转。

4. **统计命令 `udisp_stats`**：同时打印三个数——USB 收帧率 / 实际上屏率 / 丢弃数。
   现在 `lcd_ctrl fps` 只能看上屏率，分不清"PC 没推"还是"设备丢了"。

---

## 6. 已修复问题备忘（勿回退）

| 问题 | 修复 | 位置 |
|---|---|---|
| DPI 无 GRAM，draw_rect 是整层替换 | 持久 fb `screen_fb` + 按帧头合成 + 整屏推送 | main.c |
| 帧缓冲装不下 16B 帧头 + payload | FRAME_BUF_SIZE 自动取 16KB 整数倍（770048） | usb_display_device.c |
| 单缓冲丢帧 | frame_pool[2] 双缓冲 | usb_display_device.c |
| CherryUSB 断言反向 bug（bufsize 为 16KB 倍数反而失败） | `(frame_bufsize % 16384) == 0` | external/cherryusb/class/vendor/display/usbd_display.c:169 |
| main.c 漏包含 mem_section.h | 已补 | main.c |

**PC 驱动侧已知事实**（改动时用得上）：
- 产品字符串只解析前 3 段：`sifli_R<w>x<h>_E<enc>`，编码只认 `Ergb16` / `Ejpg<n>`；
- Windows 可选分辨率由 DLL 内置模式表决定（240x240 ~ 1920x1080 共 17 档），800x480 在表内可 1:1；
- 复制模式（源 2560x1600）下驱动不做缩放 → 内容只显示左上角，属 PC 驱动行为，设备端无解；
- `_Fps15_Bl128` 这类后缀 DLL 不解析，写了无效。

---

## 7. 实施顺序与验收标准

| 阶段 | 内容 | 预期 | 验收 |
|---|---|---|---|
| P1 | 去掉每帧 draw_rect | 40+ fps | `lcd_ctrl fps` ≥ 40；拖动窗口观察撕裂 |
| P2 | EPIC 合成 | CPU 占用 ↓，撕裂 ↓ | `list_thread` 看 main 线程耗时；拖动无明显撕裂 |
| P3 | JPEG 链路 | 55~60 fps | `udisp_stats` 收/显帧率一致且 ≥55；CPU < 10% |
| P4 | fb 迁 SRAM + 结构重构 | 延迟 ↓，可维护性 | `free`/`list_memheap` 余量健康，功能回归 |

**每步验证命令**：
```
lcd_ctrl fps 1        # 上屏帧率
lcd_ctrl log 1        # 单帧耗时（短时抓取）
lcd_ctrl no_draw 1    # 跳过绘制测纯收帧性能
udisp_stats           # （P2 后新增）收/显/丢三统计
```
