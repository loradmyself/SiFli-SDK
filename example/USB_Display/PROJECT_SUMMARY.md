# SiFli USB Display 副屏 + 触摸 —— 工程总结

- 最后更新：2026-11-03
- 目标板：`sf32lb58-lcd_a128r32n1_qspi_hcpu`（SF32LB58x，NAND）
- 屏幕：`HTM_H070A20_RGB_A01`，1024×600，RGB565
- 主机：Windows 10/11 x64

---

## 0. 现在能做到什么

一根 USB 线接上板子，Windows 里多出一块 **1024×600 的扩展屏**（IddCx 虚拟显示器，可扩展/复制），
并且**可以直接用手指在那块屏上操作**——触摸是 Windows 原生的 HID 数字化仪（触摸指针），
不是模拟鼠标，也不需要额外客户端软件。

```
 PC(IddCx 虚拟显示)  ──USB──  SF32LB58x
   ├ 接口0 vendor  : 显示数据  EP0x81 IN / EP0x02 OUT (bulk)
   └ 接口1 HID     : 触摸上报  EP0x83 IN (interrupt, 1ms)
```

---

## 1. 系统组成

| 层 | 实现 | 关键点 |
|---|---|---|
| 固件 | CherryUSB **复合设备**（2 接口） | 接口 0 vendor display，接口 1 HID digitizer |
| PC 显示 | `xfz1986_usb_graphic`（IddCx UMDF 驱动） | **必须用 `driver/` 里的 460KB 修改版 DLL**，官方 77KB 版不会创建监视器 |
| PC 触摸 | Windows 自带 hidusb / input.inf | 只要 HID 报告描述符合规，免驱认成"符合 HID 标准的触摸屏" |
| 触摸绑定 | `Wisp\Pen\Digimon` + `NtUserSetDisplayMapping` | 多显示器下必须显式绑定，否则永远落在主屏 |

---

## 2. 目录结构

```
USB_Display/
├── driver/                        PC 端可用驱动（已验证可用，2026-11-03）
│   ├── xfz1986_usb_graphic.inf    匹配 USB\VID_303A&PID_2986&MI_00，DriverVer=11/03/2026,1.2.2.0
│   ├── xfz1986_usb_graphic.cat    Inf2Cat 生成 + 自签证书签名
│   ├── xfz1986_usb_graphic.dll    460,288 B 修改版（能创建监视器、全模式表）
│   └── README.md                  **驱动生成/签名/安装说明，与 win-idd 的依赖关系**
├── src/
│   ├── usb_display_config.h       分辨率 / 触摸方向 —— 换屏只改这里
│   ├── usb_display_device.c       复合设备描述符 + HID 触摸报告描述符 + 报告发送
│   ├── usb_touch.c / usb_touch.h  触摸采集线程、`touch_test` 调试命令、统计
│   ├── usb_config.h               端点缓冲等
│   └── main.c                     帧缓冲、LCDC 推送
├── project/proj.conf              LCD 型号、CherryUSB DISPLAY + HID 开关
├── tools/
│   ├── touch_map.py               触摸屏 → 指定显示器 绑定（含立即生效）
│   ├── diag_usb_display.py        环境自检：拓扑 / 触摸设备 / 绑定状态
│   ├── install_driver.ps1         驱动重签（Inf2Cat+signtool）+ pnputil 安装
│   ├── README.md                  **工具使用说明（含排查对照表）**
│   └── debug/                     排查用底层探针（HID 直读 / RawInput / USB 抓描述符）
└── PROJECT_SUMMARY.md             本文件
```

---

## 3. 关键参数（改东西前先看这张表）

| 项 | 值 |
|---|---|
| VID / PID | `0x303A` / `0x2986` |
| 产品字符串 | 由 `DISPLAY_WIDTH`/`DISPLAY_HEIGHT`/`DISPLAY_BPP` 自动生成 |
| 分辨率 / 像素格式 | 1024×600 / RGB565（`usb_display_config.h`） |
| 接口 0 | vendor class `0xFF`，`EP 0x81 IN` + `EP 0x02 OUT`，bulk，HS MPS 512 |
| 接口 1 | HID class `0x03`，`EP 0x83 IN`，interrupt，MPS 8，bInterval 1ms |
| 触摸报告 | 8 字节：`[0]ReportID=1 [1]tip\|in_range [2]contact_id [3:5]X16 [5:7]Y16 [7]contact_count` |
| 触摸逻辑范围 | 0..1023 / 0..599（与显示一致，见 `TOUCH_PANEL_WIDTH/HEIGHT`） |
| 驱动 INF 匹配串 | `USB\VID_303A&PID_2986&MI_00`（复合设备的显示子接口） |
| 签名 | 测试签名 + 自签证书 `CN=SiFli USB Display Test Cert` |

> 注意：`TOUCH_IN_EP` 必须是 **EP3**（0x83）。MUSB 的端点方向由端点号决定
> （`usb_dc_musb.c` 里 OUT 清 `USB_TXCSRH1_MODE`、IN 置位），
> 若触摸用 0x82 会和显示的 `0x02`(EP2 OUT) 撞同一个硬件端点，后注册的会把方向改掉，
> 表现就是"显示突然不收包了"。

---

## 4. 今天解决的 6 个问题

### 4.1 Windows 认不出触摸屏
- **现象**：设备管理器里是"符合 HID 标准的触摸屏"，但 Raw Input 设备列表里没有它，
  `SM_MAXIMUMTOUCHES=0`、`GetPointerDevices=0`，屏幕点不动。
- **根因**：报告描述符里 **X(0x30)/Y(0x31) 没有切到 Generic Desktop 页(0x01)**，
  在 Digitizer 页下被解析成 Tip Pressure / Barrel Pressure，集合也就不是触摸屏。
- **修复**：Finger 集合里 X/Y 前插 `05 01`，之后 `05 0D` 恢复；同时按微软触摸屏规范补
  In Range(0x32)、Contact ID(0x51)、顶层 Contact Count(0x54)、Finger 集合改 Physical。
- **验证**：Raw Input 出现 VID_303A 设备、`SM_MAXIMUMTOUCHES` 非 0。

### 4.2 满屏彩色横纹
- **根因**：`main.c` 的 `display_frame()` 里"整屏推送 `draw_rect`"那一步被注释（P1 遗留），
  合成好的帧从没送到 LCDC；开机时又把未初始化的 `screen_fb`（自定义 section 不清零）推上屏。
- **修复**：恢复整屏推送；`main()` 里先 `memset(screen_fb,0)` + CleanDCache 再推黑屏。
- **验证**：上电先黑屏，PC 出画面后正常。

### 4.3 驱动装好、PC 却没画面
- **现象**：设备管理器一切正常，串口零流量。
- **排查**：系统事件日志发现 WUDFHost 曾崩溃（10110/10111）；IddCx ETW 显示连 swapchain 都没有。
- **根因**：官方 77KB 的 `xfz1986_usb_graphic.dll` 根本不创建 IddCx 监视器
  （configured 之后就静止）。
- **修复**：换用 win-idd 仓库里 9 月编译的 460KB 修改版（全模式表、`decision_runtime_policy`
  强制加载全部模式、`enc_raw_rgb.h` rgb888 返回值修复），INF 升到 `1.2.2.0` 并重签。
- **验证**：dxdiag 显示 Current Mode 1024x600@60、Topology Extend；ETW 里
  AcquireBuffer / FinishProcessingFrame 正常增长。

### 4.4 configured 之后零推流
- **根因**：见上文 3 的 `TOUCH_IN_EP` 端点冲突。
- **修复**：`0x82` → `0x83`。
- **验证**：串口 `USB frame` / `bulk_out rate` 恢复；触摸和显示可同时工作。

### 4.5 触摸有上报、但屏幕无反应
- **排查链**：设备端统计证明 EP3 发送全通 → 直接 `ReadFile` 能读到 8 字节正确报告 →
  Raw Input 设备表无 VID_303A → 断定是"系统没把它当触摸屏"（回到 4.1）。
- **教训**：`HIDP_CAPS` 字段顺序是 `Usage` 在前、`UsagePage` 在后（`hidpi.h` 实测），
  别把 0x0004 / 0x000D 误读成 usage page 错。

### 4.6 触摸动作跑到 1 号屏（本次最后一个坑）
- **现象**：手指在 3 号屏（虚拟屏）上滑，指针在 1 号屏（主屏）动；把虚拟屏设成主屏后触摸就"正常"了。
- **根因**：Windows 从未把这块数字化仪绑定到任何显示器（`Wisp\Pen\Digimon` 为空），
  未绑定时默认送给主屏。
- **关键格式**（复刻 `MultiDigiMon.exe`）：
  ```
  HKLM\SOFTWARE\Microsoft\Wisp\Pen\Digimon
    "20-" + <触摸设备接口路径>  =  <显示器设备接口路径>
  ```
  值名**必须**带 `20-` 前缀；我第一版写成裸路径，系统直接忽略，所以完全没生效。
- **立即生效**：`user32` 私有导出 **ordinal 2532 = `NtUserSetDisplayMapping(HANDLE touch, HMONITOR)`**，
  返回 1 表示成功，不需要重启。
- **无效尝试（别再走）**：直接改设备枚举键里的 `ContainerID`，想让触摸和虚拟屏同容器——
  PnP 重新枚举时会被系统重新算回原值。
- **修复**：`tools/touch_map.py apply`。
- **验证**：`tools/diag_usb_display.py` 输出 `[OK] 已绑定到 \\.\DISPLAY59 1024x600`。

---

## 5. 从零复现

### 5.1 固件

```bash
cd project
# 先按 SDK 要求设置工具链环境（ENV_VER / ENV_ROOT / PKGS_ROOT），再：
scons --board=sf32lb58-lcd_a128r32n1_qspi_hcpu -j8
```
产物在 `project/build_<board>/` 下，烧录后插入 USB。

换屏只改 `src/usb_display_config.h` 的 `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` / `DISPLAY_BPP`，
产品字符串和帧缓冲大小会自动跟着变。

### 5.2 PC 驱动

```powershell
# 管理员
bcdedit /set testsigning on      # 首次，需重启
powershell -ExecutionPolicy Bypass -File tools\install_driver.ps1
```

脚本做四件事：`Inf2Cat` 重生成 cat → 确保自签证书在 `LocalMachine\My/Root/TrustedPublisher`
→ `signtool` 签名 → `pnputil /add-driver /install`。

> **驱动是怎么来的**：`driver/xfz1986_usb_graphic.dll` 取自
> `main/tools/win-idd/win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/x64/Release/`
> 下 2025-09-20 的编译产物（460,288 B）。**编译依赖 win-idd 仓库 + VS2022 + WDK 10.0.28000.0，
> 安装使用完全不依赖它**，拷 `driver/` 三个文件到任何机器即可。
> 从源码重编、改 EDID 名字、签名证书选择、旧驱动包清理等完整流程见 **[driver/README.md](driver/README.md)**。

装完确认设备管理器里：
- `USB\VID_303A&PID_2986&MI_00` = `xfz1986_usb_graphic(demo)`（类：显示适配器）
- `HID\VID_303A&PID_2986&MI_01\...` = `符合 HID 标准的触摸屏`

### 5.3 触摸绑定（多显示器必做）

```bash
python tools/touch_map.py            # 先看拓扑，确认哪块是虚拟屏
python tools/touch_map.py apply      # 自动挑"非主屏中面积最小"的那块绑定
```

### 5.4 验证

```bash
python tools/diag_usb_display.py
```
期望输出：显示器表里有 1024×600 的那块，HID 触摸设备在列，绑定指向它，结论 `[OK]`。

---

## 6. 工具说明

完整用法、参数说明、输出解读、失败排查对照表见 **[tools/README.md](tools/README.md)**。

| 脚本 | 作用 | 典型用法 |
|---|---|---|
| `touch_map.py` | 把触摸设备绑定到指定显示器（写注册表 + 立即生效） | `python tools/touch_map.py apply` |
| `diag_usb_display.py` | 一键自检：显示拓扑 / 触摸设备 / 绑定状态 | `python tools/diag_usb_display.py` |
| `install_driver.ps1` | 驱动重签 + 安装 | 以管理员运行 |

`tools/debug/` 里是今天排查时用的底层探针，留着备用：

| 脚本 | 用途 |
|---|---|
| `hid_read_test.py` | 绕开 Windows 触摸栈，直接 `ReadFile` 读 HID 报告，判断"设备到底发了没有" |
| `rawinput_probe.py` / `rawinput_enum.py` | 监听 / 枚举 Raw Input 设备，判断 Windows 有没有把设备当输入设备 |
| `usb_sniff2.py` | 走 Hub IOCTL 抓描述符与端口连接状态（总线级） |
| `mon_enum.py` | 只列 GDI 显示器与坐标（最小化版） |

`touch_map.py` 的选屏规则：候选 = 非主屏，取面积最小的一块（也就是 1024×600 的虚拟屏）。
如果机器上还有别的副屏且面积更小，或有多个 USB 副屏，需要改这个规则或直接在脚本里写死设备路径。

---

## 7. 踩坑清单

1. **MUSB 端点方向**：端点号 = 硬件端点，同一号不能同时收/发。撞号症状极隐蔽——显示静默 NAK。
2. **HID 触摸屏 X/Y 必须在 Generic Desktop(0x01) 页**，这是微软规范硬要求。
3. **`Wisp\Pen\Digimon` 键名要 `20-` 前缀**，写裸路径等于没写。
4. **`TabletInputService` 不能 Restart**（会被拒绝），映射要靠 `NtUserSetDisplayMapping` 立即生效，
   或者干脆重启。
5. **WUDFHost 的 `OutputDebugString` 抓不到**（DBWIN Local/Global 都无效），UMDF 侧调试走 ETW。
6. **dxdiag 对 display-only 适配器的监视器信息会张冠李戴**，拓扑以 GDI/CCD 枚举为准。
7. **改 INF 一定要重签 cat**，否则哈希失效、安装被拒。
8. 本机 `reg.exe` 被安全策略拦，用 `Set-ItemProperty`；`Add-Type` 也被拦，PowerShell 里别指望内联 C#。

---

## 8. 已知限制 / 后续可做

- **单点触摸**：当前 1 个 Finger 集合、Contact Count 恒为 1。要多点需扩成多个 Finger 集合 +
  按 contact 分配 ID，并保证每帧 Contact Count 与实际一致。
- **虚拟屏在系统里显示为 `S2719DGF`**：驱动 `Driver.cpp` 的 `s_SampleMonitors[]` 内置了一段
  `// Modified EDID from Dell S2719DGF`，Windows 据此把监视器记为 `DISPLAY\DELD0E6\...`。
  不影响功能；要改见 [driver/README.md](driver/README.md) 第 8 节（换 EDID 0xFC 名称 + 重算校验和
  + 重编重签，**改完触摸绑定会失效，要重跑 touch_map.py**）。
- **触摸绑定按设备接口路径记录**：换 USB 口、换机器后路径变，需要重跑 `touch_map.py apply`。
  量产时可考虑开机自启一个检测程序。
- **驱动是测试签名**：正式发布要走 WHQL/EV 签名。
- 系统里还留着几个过时的驱动包（`oemNN.inf`），确认无回退需求后可以
  `pnputil /delete-driver oemNN.inf /uninstall` 清掉。
