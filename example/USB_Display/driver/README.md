# PC 端驱动说明（xfz1986_usb_graphic / IddCx）

本目录是 **可以直接安装使用** 的完整驱动包，也是当前验证过的版本。

---

## 1. 目录里有什么

| 文件 | 大小 | 作用 |
|---|---|---|
| `xfz1986_usb_graphic.dll` | 460,288 B | UMDF 用户态 IddCx 驱动本体，**真正干活的部分** |
| `xfz1986_usb_graphic.inf` | 2.6 KB | 安装信息，把 `USB\VID_303A&PID_2986&MI_00` 绑到本驱动 |
| `xfz1986_usb_graphic.cat` | 2.6 KB | 目录签名文件，`Inf2Cat` 生成、`signtool` 签名 |

- `DriverVer=11/03/2026,1.2.2.0`
- 设备管理器里显示名：`xfz1986_usb_graphic(demo)`
- 只匹配显示子接口（`&MI_00`）；触摸子接口（`&MI_01`）走 Windows 自带 HID 驱动，不需要 INF

---

## 2. 和 `main/tools/win-idd` 是什么关系

**一句话：编译依赖它，安装不依赖它。**

| 阶段 | 是否需要 win-idd 仓库 |
|---|---|
| 生成 DLL（改代码、重编） | **需要** —— 源码、解决方案、依赖库都在里面 |
| 生成 cat + 签名 | 不需要（只需要 WDK 的 `Inf2Cat.exe` / `signtool.exe`） |
| 安装 / 使用 | **不需要** —— 拷这 3 个文件到任何机器就能装 |

当前这个 DLL 就是 win-idd 仓库里的编译产物，原件路径：

```
main/tools/win-idd/win10_idd_xfz1986_usb_graphic_driver/
    idd_xfz1986_usb_graphic/x64/Release/xfz1986_usb_graphic/xfz1986_usb_graphic.dll
```

（2025-09-20 编译，460,288 B。仓库里另外两个 `bin/.../xfz1986_usb_graphic.dll` 是 77 KB 和 145 KB 的旧版，**不要用**——
77 KB 的官方 v2.0 不会创建 IddCx 监视器，装上去就是"设备正常、屏幕全黑"。）

源码与工程文件位置：

```
main/tools/win-idd/win10_idd_xfz1986_usb_graphic_driver/
├── idd_xfz1986_usb_graphic.sln              ← VS 解决方案
└── idd_xfz1986_usb_graphic/
    ├── idd_xfz1986_usb_graphic.vcxproj      ← 驱动工程（UMDF）
    ├── Driver.cpp                            ← 适配器/监视器/模式表/EDID
    ├── usb_transf.cpp                        ← USB 传输、编码策略
    ├── enc_base.cpp / enc_jpg.cpp / enc_raw_rgb.cpp
    ├── tiny_jpeg.c
    └── include/ lib/                         ← IddCx / WDF 头与库
```

---

## 3. 从源码生成 DLL 的流程

### 3.1 环境要求

| 项 | 要求 | 本机现状 |
|---|---|---|
| Visual Studio | 2022，勾选"使用 C++ 的桌面开发" | **未安装**（所以没重编，直接用了 9 月产物） |
| Windows SDK | 10.0.28000.0 | 已装 |
| WDK | 10.0.28000.0 | 已装（`C:\Program Files (x86)\Windows Kits\10\`） |

工程配置（`idd_xfz1986_usb_graphic.vcxproj`）：

```
PlatformToolset            = WindowsUserModeDriver10.0   （必须装 WDK 的 VS 扩展）
WindowsTargetPlatformVersion = 10.0.28000.0
ConfigurationType          = DynamicLibrary
DriverTargetPlatform       = Universal
UMDF_VERSION_MAJOR/MINOR   = 2 / 25
```

### 3.2 编译

图形界面：打开 `idd_xfz1986_usb_graphic.sln` → 配置选 **Release / x64** → 生成。

命令行（"x64 Native Tools Command Prompt for VS 2022"）：

```bat
msbuild win10_idd_xfz1986_usb_graphic_driver\idd_xfz1986_usb_graphic.sln ^
        /p:Configuration=Release /p:Platform=x64
```

产物：

```
idd_xfz1986_usb_graphic\x64\Release\xfz1986_usb_graphic.dll
```

把新 DLL 覆盖到本目录即可（**不要**改文件名，INF 里写死了 `xfz1986_usb_graphic.dll`）。

### 3.3 相对上游版本的 4 处关键改动

这几处就是"为什么必须用这个 460 KB 版本"的原因：

1. **`Driver.cpp` → `s_SampleDefaultModes[]`**
   补齐默认模式表（含 `1024x600@60`）。设备没上报模式时，靠这张表给系统兜底。
2. **`Driver.cpp` → `s_SampleMonitors[]` 内置一段 EDID**
   注释写着 `// Modified EDID from Dell S2719DGF`。
   副作用：这块虚拟屏在 Windows 里显示为 **`S2719DGF`**，设备 ID 是 `DISPLAY\DELD0E6\...`。
   功能无影响，只是名字看起来像 Dell（第 8 节讲怎么改）。
3. **`usb_transf.cpp` → `decision_runtime_policy()`**
   按设备上报的产品字符串（`R1024x600_Ergb16` 之类）决定编码方式与缓冲限制。
4. **`enc_raw_rgb.h` → rgb888 返回值修复**
   让 `R..rgb32` 模式能正确编码。

---

## 4. 生成 cat + 签名

改过 INF 或换了 DLL，`.cat` 的哈希就失效，**必须重签**，否则安装会被拒绝。

### 4.1 生成 catalog

```bat
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe" ^
    /driver:"<本目录绝对路径>" /os:10_X64 /verbose
```

> 用 `x86\Inf2Cat.exe`（不是 x64 目录），否则可能报架构不匹配。

### 4.2 选证书

两条路，二选一：

**A. 自签（当前 `driver/` 用的就是这个）**

证书主题：`CN=SiFli USB Display Test Cert`，已导入 `LocalMachine\My` + `Root` + `TrustedPublisher`。
需要系统开着测试签名：

```powershell
bcdedit /set testsigning on     # 首次，需重启
```

**B. 复用仓库里的商业证书**

```
main/tools/win-idd/sifli_cert.pfx     密码 sifli123
```

先双击导入（或 `Import-PfxCertificate`）到"受信任的发布者"，就不需要测试签名模式。

### 4.3 签名

```bat
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\signtool.exe" sign ^
    /sm /s My /n "SiFli USB Display Test Cert" /fd SHA256 /a xfz1986_usb_graphic.cat
```

> 注意 `signtool` 用 **x86** 版本配合 `/sm`（机器存储）；用 x64 版本时路径和存储参数要相应调整。

**一条命令搞定 4.1~4.3 + 安装**：

```powershell
powershell -ExecutionPolicy Bypass -File ..\tools\install_driver.ps1
```

---

## 5. 安装 / 卸载

### 5.1 安装

```powershell
# 管理员 PowerShell
pnputil /add-driver "F:\SiFli\SDK\SiFli-SDK\main\example\USB_Display\driver\xfz1986_usb_graphic.inf" /install
```

或直接跑 `..\tools\install_driver.ps1`。

板子插着的情况下，`pnputil` 会直接把驱动绑到 `MI_00`；没插着也没关系，下次插入自动匹配。

### 5.2 验证

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_303A*" } |
    Select-Object InstanceId, Class, FriendlyName, Status
```

期望：

```
USB\VID_303A&PID_2986\2025010101          USB         USB Composite Device            OK
USB\VID_303A&PID_2986&MI_00\...           Display     xfz1986_usb_graphic(demo)       OK
USB\VID_303A&PID_2986&MI_01\...           HIDClass    USB 输入设备                    OK
HID\VID_303A&PID_2986&MI_01\...           HIDClass    符合 HID 标准的触摸屏           OK
```

### 5.3 卸载 / 清理旧包

系统里可能残留多个 `oemNN.inf`，`pnputil` 总是用最新的那个，旧包会让"改了 INF 却不生效"。

```powershell
pnputil /enum-drivers | Select-String -Pattern "oem\d+\.inf|xfz1986" -Context 0,3
pnputil /delete-driver oemNN.inf /uninstall /force
```

`/force` 会在设备使用中强删，删完重新插拔板子。

---

## 6. 本目录 INF 相对上游的改动清单

| 位置 | 改动 | 原因 |
|---|---|---|
| `[Version]` | 加 `CatalogFile = xfz1986_usb_graphic.cat` | 不加就不校验签名，安装报错 |
| `[Version]` | `DriverVer=11/03/2026,1.2.2.0` | 版本要比旧包高，否则不替换 |
| `[Standard.NTamd64]` | 匹配串加 `&MI_00` | 加触摸后变成复合设备，父节点装不上 |
| `[SourceDisksFiles]` | 原文写成 `[SourceDiskFiles]` | **拼写错误会导致 Inf2Cat 报错** |
| `[SourceDisksNames]` | 原文写成 `[SourceDiskNames]` | 同上 |
| `[Standard.NTamd64]` | 保留 `PID_2987` 单功能匹配（注释掉裸匹配） | 回退无触摸固件时改回注释即可 |

---

## 7. 常见错误对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| `pnputil` 报 `0x800B0109` / 签名无效 | cat 哈希失效（改过 INF 没重签）或证书没进受信任存储 | 重跑 Inf2Cat + signtool；把证书导入 `TrustedPublisher` |
| 装上了但设备管理器黄叹号 | 测试签名没开 | `bcdedit /set testsigning on` + 重启 |
| 设备正常，但屏幕全黑、串口零流量 | 用错 DLL（77 KB 官方版不建监视器） | 换回 460,288 B 版本 |
| 显示正常，触摸没有 | 触摸是 HID 子接口，与本驱动无关 | 查 `MI_01` 是否枚举；再跑 `tools\diag_usb_display.py` |
| 换了新 INF 但不生效 | 系统里还有旧 `oemNN.inf` | 见 5.3 清理 |
| `Inf2Cat` 报 `[SourceDiskFiles]` 段错误 | INF 段名拼写 | 见第 6 节 |

---

## 8. 想把显示名称从 "S2719DGF" 改成 "SiFli USB Display"

Windows 会拿驱动上报的 EDID 去匹配监视器记录，所以它在显示设置里叫 `S2719DGF`，
设备 ID 是 `DISPLAY\DELD0E6\...`（这也是触摸绑定里写的那个路径）。

要改名，改 `Driver.cpp` → `s_SampleMonitors[]` 里那段 EDID：

- `0xFC` 描述符后面是 **13 字节**的显示器名称文本（当前 `53 32 37 31 39 44 47 46 0A 20 20 20 20` = `"S2719DGF\n    "`）
- 换成自己的名字，**必须保持 13 字节**（不足补 `0x20`，最后可以是 `0x0A`）
- 改完 EDID 校验和（最后一个字节 `0x2C`）要重算：整段 128 字节求和取模 256 应为 0

改完重新编译 → 重签 cat → 重装驱动 → 在设备管理器里卸载旧的"通用即插即用监视器"节点让它重新枚举。

> 改名后 `DISPLAY\xxxx` 这个 ID 会变，**触摸绑定会失效**，记得重跑一次 `python ..\tools\touch_map.py apply`。
