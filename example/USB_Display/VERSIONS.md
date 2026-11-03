# 版本基线（VERSIONS.md）

记录本工程验证时所用的 **SDK 主仓 / 驱动源码仓 / 工具链** 版本，用于日后回溯与回归比对。

> 改完 SDK、重新拉代码、或升级环境后，跑一次 `python tools\version_snapshot.py --check`，
> 它会告诉你"现在的代码还是不是当初验证过的那份"。

---

## 1. 摘要

| 组件 | 版本号 | 分支 | 提交 | 工作区 |
|---|---|---|---|---|
| SiFli-SDK | `version.txt` = **v2.5.0**（tag `rom_57x_v1.0` + 1723 commits） | `nuonuo` | `a0155f003` | **有改动** |
| win-idd（驱动源码） | `v1.0-28-gc2de0f5` | `main` | `c2de0f56` | **有改动（关键）** |
| SiFli Env | 1.2.0（`F:\SiFli\env_1.2`） | — | — | — |
| 交叉工具链 | gcc 14.2.1 | — | — | — |
| 目标板 | `sf32lb58-lcd_a128r32n1_qspi_hcpu` | — | — | — |
| 记录时间 | 2026-11-03 | | | |

---

## 2. 机器可读块

下面这块由 `tools\version_snapshot.py --write` 自动生成，**不要手改**。

<!-- BEGIN VERSION YAML -->
```yaml
recorded_at: 2026-11-03 19:24
project: USB_Display
board: sf32lb58-lcd_a128r32n1_qspi_hcpu

sdk:
  path: "F:/SiFli/SDK/SiFli-SDK/main"
  branch: "nuonuo"
  commit: "a0155f00329f463da7d156b16cf9324881252db0"
  commit_short: "a0155f003"
  describe: "rom_57x_v1.0-1723-ga0155f003-dirty"
  nearest_tag: "rom_57x_v1.0"
  commit_date: "2026-11-03 19:06:14 +0800"
  remote_origin: "https://github.com/loradmyself/SiFli-SDK.git"
  remote_upstream: "https://github.com/OpenSiFli/SiFli-SDK.git"
  dirty: true
  dirty_files:
    - "M tools/win-idd"
  version_txt: "v2.5.0"
  version_dot: "MAJOR=2; MINOR=5; REV=0"

win_idd:
  path: "F:/SiFli/SDK/SiFli-SDK/main/tools/win-idd"
  branch: "main"
  commit: "c2de0f56f778904d663d8e2dc5a71b4a6b270ecc"
  commit_short: "c2de0f56f"
  describe: "v1.0-28-gc2de0f5-dirty"
  nearest_tag: "v1.0"
  commit_date: "2026-07-12 21:58:30 +0800"
  remote_origin: "https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display"
  dirty: true
  dirty_files:
    - "M bin/xfz1986_usb_graphic_windos10_driver-v2.0/xfz1986_usb_graphic.inf"
    - "M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/Driver.cpp"
    - "M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/enc_raw_rgb.h"
    - "M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/idd_xfz1986_usb_graphic.vcxproj"
    - "M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/usb_transf.cpp"
    - "?? .vscode/"
    - "?? driver_tmp/"
    - "?? install_driver.bat"
    - "?? sifli_cert.pfx"
    - "?? sign_driver.bat"
    - "?? win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/x64/"
    - "?? win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/xfz1986_usb_graphic.cat"
    - "?? win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/xfz1986_usb_graphic.dll"
    - "?? win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/xfz1986_usb_graphic_fixed.inf"
    - "?? win10_idd_xfz1986_usb_graphic_driver/x64/"

env:
  env_root: "F:/SiFli/env_1.2"
  env_ver: ""
```
<!-- END VERSION YAML -->

---

## 3. 为什么用这个格式

一条版本记录要同时满足三个用途：**人看、机器比、能复现**。所以分三层：

| 层 | 字段 | 为什么必须有 |
|---|---|---|
| 定位 | `branch` + `commit`（全哈希） | 分支会走、tag 会挪，只有 40 位哈希能唯一确定一棵树 |
| 人读 | `describe`（如 `rom_57x_v1.0-1723-ga0155f003-dirty`） | 一眼看出"离哪个 tag 多远、有没有改过" |
| 防坑 | `dirty` + `dirty_files` | **commit 相同 ≠ 内容相同**，有未提交改动时只记哈希等于自欺欺人 |

三个必须遵守的约定：

1. **提交只记全哈希，短哈希只做显示用。** 短哈希在前缀冲突时会歧义。
2. **`dirty: true` 时一定要附改动文件清单。** 否则别人 checkout 同一个 commit 也复现不出你的现象。
3. **嵌套仓库要单独记。** 本工程有两层 git（SDK 主仓 + `tools/win-idd`），父仓看到的只是一个
   gitlink 指针，看不到子仓的分支和改动。

---

## 4. 当前状态里两个必须注意的点

### 4.1 SDK 的 git 根在 `main\`，不是 `SiFli-SDK\`

```
F:\SiFli\SDK\SiFli-SDK\          ← 不是 git 仓库（只有个 main 目录）
F:\SiFli\SDK\SiFli-SDK\main\     ← 真正的仓库根，.version / version.txt 也在这
```

在 `SiFli-SDK\` 下执行 git 命令会得到 `fatal: not a git repository`，别以为版本丢了。

### 4.2 `tools\win-idd` 的改动**没有提交**

这块是驱动 DLL 的来源，而 460 KB 那个可用版本正是基于这些**未提交改动**编出来的：

```
 M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/Driver.cpp
 M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/usb_transf.cpp
 M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/enc_raw_rgb.h
 M win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/idd_xfz1986_usb_graphic.vcxproj
 M bin/xfz1986_usb_graphic_windos10_driver-v2.0/xfz1986_usb_graphic.inf
```

也就是说：**只记录 `c2de0f56` 这个 commit，换台机器 checkout 出来的是上游原始代码，编不出能用的 DLL。**
两个解决办法，二选一：

**A. 提交到自己的分支（推荐）**

```bash
cd F:\SiFli\SDK\SiFli-SDK\main\tools\win-idd
git checkout -b sifli-usb-display
git add -A
git commit -m "feat: 全模式表 + 1024x600 + decision_runtime_policy + rgb888 修复（SiFli USB Display 用）"
```
之后版本记录里只写这个新 commit 就够了。

**B. 导出 patch 一起存档**

```bash
cd F:\SiFli\SDK\SiFli-SDK\main\tools\win-idd
git diff > ..\..\..\example\USB_Display\driver\win-idd-local.patch
```
记录里注明"需先 `git apply driver\win-idd-local.patch`"。

> 现状是 A 和 B 都还没做，所以 `VERSIONS.md` 里 `win_idd.dirty` 只能是 `true`。
> 想彻底干净，做掉 A 再跑一次 `--write`。

---

## 5. 常用命令

```bash
# 生成/刷新 VERSIONS.md 里的 YAML 块
python tools\version_snapshot.py --write

# 校验当前代码是否仍是记录的那份（CI / 回归前跑）
python tools\version_snapshot.py --check

# 只打印当前版本
python tools\version_snapshot.py
```

`--check` 的输出形如：

```
  [OK ] sdk      branch  recorded=nuonuo current=nuonuo
  [OK ] sdk      commit  recorded=a0155f00329f... current=a0155f00329f...
  [OK ] sdk      dirty   recorded=true current=true
  [DIFF] win_idd commit  recorded=c2de0f56f778... current=9ab1234c...
[!] 有 1 处与记录不符
```

### 5.1 脚本拷到别处也能用

脚本默认"从当前目录 / 脚本所在目录向上找 `.version` + `version.txt`"，所以放在 `tools\` 下运行时
不用任何参数。把它复制到 SDK 树外（例如 `D:\work\`）或在别的终端里运行时，按下面的顺序指定：

```powershell
# ① 写用户配置，一劳永逸（存 %USERPROFILE%\.sifli\sdk_path，纯文本一行）
python version_snapshot.py --set-sdk F:\SiFli\SDK\SiFli-SDK\main

# ② 用环境变量（用户级，新开终端自动继承）
setx SIFLI_SDK_PATH "F:\SiFli\SDK\SiFli-SDK\main"

# ③ 单次指定
python version_snapshot.py --sdk F:\SiFli\SDK\SiFli-SDK\main --check
```

查找优先级：`--sdk` → 环境变量（`SIFLI_SDK_PATH` / `SIFLI_SDK_ROOT` / `SIFLI_SDK` /
`SIFLI_SDK_HOME` / `SDK_PATH`）→ 从 cwd 向上 → 从脚本目录向上 → `~\.sifli\sdk_path` →
`ENV_ROOT` 猜测。传父目录（`SiFli-SDK`）会自动下探到 `main`。
配置文件排在上向查找之后，是为了让"跑在哪个树里就用哪个"，全局配置只在找不到时兜底。

工程目录（本文件所在处）单独定位：`--project` → 环境变量 `SIFLI_DISPLAY_PROJECT` → 脚本的上级目录
→ 从 cwd 向上找 `VERSIONS.md` → `SDK\example\USB_Display`。

不确定命中了哪条时：

```bash
python tools\version_snapshot.py --paths
```

`[i]` 开头的诊断行都走 `stderr`，所以 `> v.yaml` 重定向拿到的仍是干净 YAML。

手工核对时可以直接用：

```bash
git -C F:\SiFli\SDK\SiFli-SDK\main log -1 --format="%H %ad %s" --date=iso
git -C F:\SiFli\SDK\SiFli-SDK\main describe --tags --always --dirty
git -C F:\SiFli\SDK\SiFli-SDK\main status --porcelain
```

---

## 6. 维护习惯建议

| 时机 | 动作 |
|---|---|
| 每次要交付、发版、写验证报告前 | `python tools\version_snapshot.py --write`，然后提交 `VERSIONS.md` |
| 拉完新代码、切分支后 | `--check`，看一眼有没有偏离 |
| 出现"之前好好的现在不行了" | 先 `--check`，排除环境漂移，再查代码 |
| 升级 SDK 之后 | 旧的 VERSIONS.md 归档（如 `docs\versions\2026-11-03.md`），重新记录 |
