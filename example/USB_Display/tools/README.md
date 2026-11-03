# 工具集使用说明

本目录的工具都是 **PC 侧（Windows）** 用的，Python 脚本只用标准库，不需要 pip 装东西。

| 脚本 | 一句话 |
|---|---|
| `touch_map.py` | 把触摸屏绑定到指定的那块显示器（多显示器环境必做） |
| `diag_usb_display.py` | 一键自检：显示器拓扑 / 触摸设备 / 绑定状态 |
| `install_driver.ps1` | 驱动重签（Inf2Cat + signtool）+ 安装 |
| `version_snapshot.py` | 记录 / 校验 SDK 与驱动源码的版本基线 |
| `debug/*.py` | 排查用的底层探针，平时不用 |

---

## 0. 环境要求

| 项 | 说明 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| Python | 3.8 以上（本机 3.13），**只用标准库** |
| 权限 | `touch_map.py` 要写 HKLM 注册表 → **管理员**；`diag_usb_display.py` 只读，普通权限即可；`install_driver.ps1` 必须管理员 |

路径统一按工程根目录算，例如：

```bash
cd F:\SiFli\SDK\SiFli-SDK\main\example\USB_Display
python tools\diag_usb_display.py
```

---

## 1. `touch_map.py` —— 触摸屏绑定到指定显示器

### 1.1 为什么需要它

Windows 只有在"触摸设备 = 某个显示器"的绑定存在时，才会把手指坐标送到那块屏。
没有绑定记录时，**默认全送主显示器**。现象就是：

> 手指在 3 号屏（虚拟副屏）上滑，鼠标指针在 1 号屏（主屏）上乱跑；把虚拟屏设成主屏后触摸就"正常"了。

这不是固件问题，改坐标/镜像都没用，必须做绑定。

### 1.2 它做了什么

复刻 Windows 自带 `MultiDigiMon.exe`（控制面板 → 平板电脑设置 → 配置）的行为：

1. 写注册表：

```
HKLM\SOFTWARE\Microsoft\Wisp\Pen\Digimon
    值名 = "20-" + 触摸设备接口路径   ← 前缀 20- 不能省，省了系统直接忽略
    值   = 显示器设备接口路径
```

2. 调 `user32` 私有导出 **ordinal 2532 = `NtUserSetDisplayMapping(HANDLE touchDev, HMONITOR)`**
   通知系统立即应用，**不需要重启、不需要注销**。

> `TabletInputService` 是拒绝被停止的服务，所以别想着"重启服务让它生效"，走上面这个 API 才是正路。

### 1.3 用法

```bash
python tools\touch_map.py             # 只列候选，什么都不改（先跑这个确认拓扑）
python tools\touch_map.py apply       # 绑定并立即生效
python tools\touch_map.py apply --reg-only   # 只写注册表，下次重启生效
```

`apply` 的实际输出：

```
=== HID 触摸候选 ===
  handle=0x11407DD  \\?\HID#VID_303A&PID_2986&MI_01#8&e917dbb&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}
=== 显示器 ===
  \\.\DISPLAY1   1707x1067 @(0, 0)     primary=True  \\?\DISPLAY#CSO1615#...
  \\.\DISPLAY2   1920x1080 @(-1920, 0) primary=False \\?\DISPLAY#SAM100E#...
  \\.\DISPLAY59  1024x600  @(2560, 0)  primary=False \\?\DISPLAY#DELD0E6#...

绑定：
  触摸 \\?\HID#VID_303A&PID_2986&MI_01#...
  显示 \\?\DISPLAY#DELD0E6#... (\\.\DISPLAY59 1024x600)
[ok] 注册表已写入 Digimon
[ok] 已立即生效（NtUserSetDisplayMapping 返回成功）
```

### 1.4 选屏规则（重要）

脚本默认：**候选 = 所有非主屏，取面积最小的一块**。

这对当前这台机器是对的（虚拟屏 1024×600 是最小的副屏）。
如果你的机器上有更小的副屏，或者同时接了多个 USB 副屏，就会选错，需要改脚本里的
`monitors()` 过滤条件——最稳的办法是直接按设备路径写死：

```python
# 在 main() 里把 target 换成固定值
target = next(m for m in mon if "DISPLAY#DELD0E6#" in m["devid"].upper())
```

> 虚拟屏的设备 ID 是 `DISPLAY\DELD0E6\...`（因为驱动内置了 Dell S2719DGF 的 EDID），
> 换机器/换驱动版本后这个 ID 可能不同，所以建议先用无参模式看一眼再决定。

### 1.5 什么时候要重跑

| 场景 | 是否需要重跑 |
|---|---|
| 拔插板子到**同一个** USB 口 | 一般不需要（设备路径不变） |
| 换到**另一个** USB 口 | **需要**（instance id 变了） |
| 换一台电脑 | **需要** |
| 重启 Windows | 不需要（注册表持久化） |
| 改了虚拟屏的 EDID / 设备名 | **需要**（显示器路径变了） |
| 换了显示器的接线顺序、改主屏 | 建议重跑一次确认 |

### 1.6 失败排查

| 输出 | 原因 | 处理 |
|---|---|---|
| `[!] 没找到触摸设备` | 板子没连、固件没跑、或 `MI_01` 没枚举 | 先跑 `diag_usb_display.py` 看设备在不在 |
| `[!] 没有找到副屏` | 只有一块屏在工作 | 先在显示设置里把虚拟屏设为"扩展" |
| `[!] 目标显示器没有设备接口路径` | `EnumDisplayDevices` 失败，监视器节点异常 | 设备管理器里卸载"通用即插即用监视器"后重新扫描 |
| `[!] 当前系统没有 NtUserSetDisplayMapping(2532)` | 系统版本较老 | 加了 `--reg-only` 写入，重启生效 |
| `NtUserSetDisplayMapping 返回失败` | 句柄无效（设备刚被重枚举） | 重插板子后重跑 |
| 绑定成功但指针还是乱跑 | 可能有多块屏分辨率相同、绑错屏 | 先跑无参模式核对 `devid`，按 1.4 写死 |

---

## 2. `diag_usb_display.py` —— 环境自检

只读，不改任何东西。用来快速回答"现在到底哪一环不对"。

```bash
python tools\diag_usb_display.py
```

正常输出：

```
==============================================================================
桌面显示器
==============================================================================
  \\.\DISPLAY1  1707x1067 @(0, 0)       PRIMARY  \\?\DISPLAY#CSO1615#...
  \\.\DISPLAY2  1920x1080 @(-1920, 0)            \\?\DISPLAY#SAM100E#...
  \\.\DISPLAY59 1024x600  @(2560, 0)             \\?\DISPLAY#DELD0E6#...

==============================================================================
HID 触摸设备 (VID_303A&PID_2986&MI_01)
==============================================================================
  handle=0x11407DD
  path  =\\?\HID#VID_303A&PID_2986&MI_01#8&e917dbb&0&0000#{4d1e55b2-...}

==============================================================================
触摸 -> 显示器 绑定 (Wisp\Pen\Digimon)
==============================================================================
  20-\\?\HID#VID_303A&PID_2986&MI_01#...
    -> \\?\DISPLAY#DELD0E6#...

==============================================================================
结论
==============================================================================
  [OK] 已绑定到 \\.\DISPLAY59  1024x600 @(2560, 0)
       目标就是虚拟副屏，配置正确
```

三种异常结论：

| 结论 | 含义 | 下一步 |
|---|---|---|
| `[X] 触摸设备未就绪` | 系统根本没看到 `MI_01` | 查固件/驱动/换 USB 口 |
| `[X] 未绑定` | 设备在，但没做过绑定 | `touch_map.py apply` |
| `[!] 有绑定记录，但没有匹配当前触摸路径的项` | 设备路径变了（换口/换机） | 重跑 `apply` |
| `[!] 绑定的显示器当前不存在` | 绑到一块已经拔掉的屏 | 重跑 `apply` |

**排查顺序建议**：先看这段输出，再决定往固件侧还是 PC 侧查，能省掉大量来回。

---

## 3. `install_driver.ps1` —— 驱动重签 + 安装

给新机器部署、或改过 INF 之后用。

```powershell
# 管理员
powershell -ExecutionPolicy Bypass -File tools\install_driver.ps1
```

流程（脚本内部顺序）：

1. 校验管理员权限
2. 打印 `bcdedit` 里的 `testsigning` 状态（只是提示，不会替你改）
3. 用最新的 Windows Kits 里的 `Inf2Cat.exe` 重新生成 `.cat`
4. 找证书 `CN=SiFli USB Display Test Cert`；没有就 `New-SelfSignedCertificate` 新建，
   并确保它已导入 `LocalMachine\My` / `Root` / `TrustedPublisher`
5. `signtool sign /fd SHA256` 签 `.cat`
6. `pnputil /add-driver ... /install`

可调参数：

```powershell
-File tools\install_driver.ps1 -DriverDir "D:\somewhere\driver" -InfName "xxx.inf"
```

注意：

- 首次部署要先 `bcdedit /set testsigning on` 并**重启**，否则自签 cat 会被拒。
- 脚本不处理"旧 `oemNN.inf` 残留"，那种情况要手动 `pnputil /delete-driver`（见 `driver/README.md` 5.3）。
- 脚本输出是英文的——`.ps1` 在 Windows PowerShell 5.1 下读 UTF-8 中文容易乱码，故意避开。

---

## 4. `version_snapshot.py` —— 版本基线记录 / 校验

配套文档：工程根的 `VERSIONS.md`。

```bash
python tools\version_snapshot.py              # 打印当前版本 YAML
python tools\version_snapshot.py --write      # 写入 VERSIONS.md 的标记块
python tools\version_snapshot.py --check      # 与 VERSIONS.md 比对，不一致返回 1

python tools\version_snapshot.py --paths      # 我到底找到的是哪个 SDK / 哪个工程？
python tools\version_snapshot.py --sdk F:\SiFli\SDK\SiFli-SDK\main    # 单次指定
python tools\version_snapshot.py --set-sdk    # 把找到的路径写成用户配置，长期生效
```

它同时记录两层仓库：**SDK 主仓**（`main\`）和 **嵌套的驱动源码仓**（`main\tools\win-idd`）。

### 4.1 脚本放哪儿都能跑（SDK 根怎么找）

脚本不绑死自己的位置，按下面顺序找 SDK 根，**先命中先用**：

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | `--sdk <path>` | 支持传父目录（`SiFli-SDK` 会自动下探到 `main`） |
| 2 | 环境变量 `SIFLI_SDK_PATH` / `SIFLI_SDK_ROOT` / `SIFLI_SDK` / `SIFLI_SDK_HOME` / `SDK_PATH` | 任意一个命中即可 |
| 3 | 从**当前工作目录**向上找 | 在 SDK 里任何子目录下运行都能命中 |
| 4 | 从**脚本所在目录**向上找 | 脚本留在 `tools\` 里时的常规路径 |
| 5 | 用户配置文件 `C:\Users\<你>\.sifli\sdk_path` | 由 `--set-sdk` 写入，全局兜底默认 |
| 6 | `<ENV_ROOT>\..\SDK\SiFli-SDK\main` | 有 `ENV_ROOT` 时的最后兜底 |

局部上下文（当前所在的树）优先于全局配置，所以同时存在多个 SDK 副本时，跑在哪个里面就用哪个。

判定标准：既有 `.version` + `version.txt`（严格），或 git 仓 + `rtconfig.py` + `example\` + `tools\`（宽松，兼容新版本 SDK）。

**工程目录**（`VERSIONS.md` 所在处）另外单独定位：`--project` → 环境变量 `SIFLI_DISPLAY_PROJECT` →
脚本的上级目录（脚本放在 `tools\` 下时就是工程根）→ 从 cwd 向上找 `VERSIONS.md` → `SDK\example\USB_Display`。

想知道实际命中了哪一条：

```bash
$ python version_snapshot.py --paths
[i] SDK   : F:\SiFli\SDK\SiFli-SDK\main   <- 向上查找 cwd（F:\SiFli\SDK\...\example\USB_Display -> ...\main）
[i] 工程  : F:\SiFli\SDK\SiFli-SDK\main\example\USB_Display   <- 脚本位置
[i] 配置文件: C:\Users\YBL\.sifli\sdk_path（未创建）
```

### 4.2 拷到别的目录 / 别的终端

典型场景：把 `version_snapshot.py` 复制到 `D:\work\` 用，那里不属于任何 SDK 树，向上查找必然失败。
三个办法，按推荐顺序：

```powershell
# ① 一次性写入用户配置（之后 anywhere 都能跑，包括重开终端）
python version_snapshot.py --set-sdk F:\SiFli\SDK\SiFli-SDK\main

# ② 不想有配置文件，就用环境变量（用户级，新开的终端自动继承）
setx SIFLI_SDK_PATH "F:\SiFli\SDK\SiFli-SDK\main"

# ③ 只影响当前这次调用
python version_snapshot.py --sdk F:\SiFli\SDK\SiFli-SDK\main --check
```

`--set-sdk` 写的是 `%USERPROFILE%\.sifli\sdk_path`，纯文本一行，想撤就删掉这个文件。
注意 `setx` 只对**之后新开**的终端生效，当前窗口要继续用就得 `$env:SIFLI_SDK_PATH = "..."`。

`--check` 只比 `branch` / `commit` / `dirty` 三项，输出 `[OK ]` 或 `[DIFF]`：

```
  [OK ] sdk      branch  recorded=nuonuo current=nuonuo
  [OK ] sdk      commit  recorded=a0155f00329f... current=a0155f00329f...
  [DIFF] win_idd commit  recorded=c2de0f56f778... current=9ab1234c...
[!] 有 1 处与记录不符
```

两个细节：

- **工程目录自身的改动会被过滤**（工程目录相对 SDK 的路径前缀，如 `example/USB_Display/`），否则每写一次
  文档 `dirty` 就变一次，基线永远对不上。工程不在 SDK 树内时不做过滤。
- **`dirty: true` 表示"内容与 commit 不一致"**，此时光有哈希不能复现，必须看 `dirty_files` 清单。
  当前 `tools\win-idd` 就有 5 个文件未提交，那正是 460 KB DLL 的来源——详见 `VERSIONS.md` 第 4.2 节。

> 诊断信息（`[i] SDK : ...`）走 `stderr`，YAML 走 `stdout`，所以 `python version_snapshot.py > v.yaml`
> 拿到的仍是干净的 YAML。

---

## 5. `debug/` —— 底层探针

平时用不到，怀疑"到底是设备没发、还是系统没收"时用。

| 脚本 | 用途 | 用法 |
|---|---|---|
| `hid_read_test.py` | 绕开 Windows 触摸栈，直接 `ReadFile` 读 HID 报告 | `python tools\debug\hid_read_test.py`，能打出 8 字节报告说明**设备侧没问题** |
| `rawinput_probe.py` | 监听 Raw Input 事件，看 Windows 有没有把它当输入设备 | `python tools\debug\rawinput_probe.py` |
| `rawinput_enum.py` | 只枚举 Raw Input 设备列表（快照） | `python tools\debug\rawinput_enum.py` |
| `usb_sniff2.py` | 走 Hub IOCTL 抓 USB 描述符、端口连接状态 | `python tools\debug\usb_sniff2.py` |
| `mon_enum.py` | 极简版 GDI 显示器枚举（只有名字和坐标） | `python tools\debug\mon_enum.py` |

判断逻辑：

```
hid_read_test 读得到报告  →  设备侧 OK，问题在 PC 侧（描述符/绑定）
hid_read_test 读不到      →  问题在固件侧（端点/发送/描述符）
```

---

## 6. 典型工作流

### A. 全新机器首次部署

```powershell
# 1. 开测试签名（只需一次），重启
bcdedit /set testsigning on

# 2. 装驱动
powershell -ExecutionPolicy Bypass -File tools\install_driver.ps1

# 3. 插板子，确认设备树
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_303A*" } |
    Select-Object InstanceId, Class, FriendlyName, Status

# 4. 自检 + 绑定
python tools\diag_usb_display.py
python tools\touch_map.py apply
```

### B. 换了 USB 口 / 换了电脑

```bash
python tools\diag_usb_display.py     # 看是否 [X] 未绑定
python tools\touch_map.py apply
```

### C. 屏幕黑的排查顺序

```
1. 串口有没有 "USB frame #"        → 没有：固件/端点问题
2. diag_usb_display.py 有没有 1024x600 那块屏 → 没有：驱动/监视器问题
3. 有屏但没画面                      → 驱动 DLL 版本（必须 460,288 B）
4. 有画面但触摸乱跑                  → touch_map.py apply
```

### D. 触摸方向不对（指针到了对的屏，但位置偏/镜像）

PC 侧不用动，改固件 `src/usb_display_config.h`：

```c
#define TOUCH_SWAP_XY   0   /* 1 = 交换 X/Y */
#define TOUCH_MIRROR_X  0   /* 1 = X 轴镜像 */
#define TOUCH_MIRROR_Y  0   /* 1 = Y 轴镜像 */
```

改完重新编译烧录即可，映射关系不受影响。
