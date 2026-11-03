# -*- coding: utf-8 -*-
"""
记录 / 校验本工程依赖的版本基线（SDK 主仓 + 嵌套的驱动源码仓 + 工具链）。

用法:
    python version_snapshot.py                     # 打印当前版本 YAML
    python version_snapshot.py --write             # 写入 VERSIONS.md 的标记块
    python version_snapshot.py --check             # 与 VERSIONS.md 记录比对，不一致返回 1

    # 定位（不依赖脚本放在哪里）
    python version_snapshot.py --paths             # 显示 SDK / 工程是怎么被找到的
    python version_snapshot.py --sdk D:\\SiFli\\SDK\\SiFli-SDK\\main
    python version_snapshot.py --set-sdk           # 把当前找到的 SDK 路径写入用户配置，长期生效
    python version_snapshot.py --project D:\\work\\USB_Display

SDK 根的查找顺序（先命中先用）:
    1. --sdk <path> / --sdk=<path> 命令行参数
    2. 环境变量 SIFLI_SDK_PATH / SIFLI_SDK_ROOT / SIFLI_SDK / SIFLI_SDK_HOME / SDK_PATH
    3. 从当前工作目录向上查找
    4. 从脚本所在目录向上查找
    5. 用户配置文件 ~/.sifli/sdk_path（由 --set-sdk 写入）—— 当作全局兜底默认
    6. ENV_ROOT 下猜测 <ENV_ROOT>/../SDK/SiFli-SDK/main

为什么需要它：git commit 相同不代表内容相同（有未提交改动时），
所以除 commit 外必须同时记录 dirty 状态与改动文件清单。
"""
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

BEGIN = "<!-- BEGIN VERSION YAML -->"
END = "<!-- END VERSION YAML -->"

SDK_ENV_VARS = ("SIFLI_SDK_PATH", "SIFLI_SDK_ROOT", "SIFLI_SDK",
                "SIFLI_SDK_HOME", "SDK_PATH")
PROJECT_ENV_VARS = ("SIFLI_DISPLAY_PROJECT", "SIFLI_PROJECT_DIR")
CONFIG_PATH = Path.home() / ".sifli" / "sdk_path"

DEFAULT_PROJECT_TAIL = ("example", "USB_Display")


# ---------------------------------------------------------------- 基础工具

def sh(repo, *args):
    try:
        p = subprocess.run(["git", "-C", str(repo), *args],
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        return p.stdout.strip() if p.returncode == 0 else ""
    except (FileNotFoundError, OSError):
        return ""


def info(msg):
    print("[i] " + msg, file=sys.stderr)


def warn(msg):
    print("[!] " + msg, file=sys.stderr)


# ---------------------------------------------------------------- 路径定位

def is_sdk_root(p: Path):
    """严格：.version + version.txt；宽松：git 仓 + rtconfig.py + example/ + tools/"""
    if not p or not p.is_dir():
        return False
    if (p / ".version").exists() and (p / "version.txt").exists():
        return True
    return ((p / "rtconfig.py").exists() and (p / "example").is_dir()
            and (p / "tools").is_dir() and (p / ".git").exists())


def normalize_sdk(p: Path):
    """允许传父目录（SiFli-SDK 而不是 SiFli-SDK\\main）。"""
    if not p:
        return None
    p = p.expanduser().resolve()
    if is_sdk_root(p):
        return p
    # SiFli-SDK\  ->  SiFli-SDK\main\
    for sub in ("main", "sdk", "SiFli-SDK/main"):
        cand = p / sub
        if is_sdk_root(cand):
            return cand.resolve()
    return None


def ancestors(start: Path):
    start = start.expanduser().resolve()
    return [start] + list(start.parents)


def cli_value(flag):
    """支持 --flag value 和 --flag=value 两种写法。"""
    for i, a in enumerate(sys.argv):
        if a == flag and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if a.startswith(flag + "="):
            return a.split("=", 1)[1]
    return None


def resolve_sdk_root():
    """返回 (path, source)；找不到返回 (None, 尝试记录列表)。"""
    tried = []

    # 1. 命令行（--sdk，或 --set-sdk 后面直接跟路径）
    v = cli_value("--sdk") or cli_value("--set-sdk")
    if v and not v.startswith("-"):
        p = normalize_sdk(Path(v))
        tried.append("--sdk %s" % v)
        if p:
            return p, "--sdk"

    # 2. 环境变量
    for name in SDK_ENV_VARS:
        v = os.environ.get(name)
        if not v:
            continue
        p = normalize_sdk(Path(v))
        tried.append("%s=%s" % (name, v))
        if p:
            return p, "env %s" % name

    # 3/4. 向上查找（工作目录优先，其次脚本目录）
    here = Path(__file__).resolve()
    for start, label in ((Path.cwd(), "cwd"), (here.parent, "script")):
        for p in ancestors(start):
            if is_sdk_root(p):
                return p.resolve(), "向上查找 %s（%s -> %s）" % (label, start, p.resolve())
        tried.append("向上查找 %s（%s）未命中" % (label, start))

    # 5. 用户配置文件（全局兜底默认）
    if CONFIG_PATH.exists():
        v = CONFIG_PATH.read_text(encoding="utf-8", errors="replace").strip()
        p = normalize_sdk(Path(v)) if v else None
        tried.append("config %s -> %s" % (CONFIG_PATH, v))
        if p:
            return p, "config " + str(CONFIG_PATH)

    # 6. ENV_ROOT 猜测
    env_root = os.environ.get("ENV_ROOT")
    if env_root:
        guess = Path(env_root).parent / "SDK" / "SiFli-SDK" / "main"
        if is_sdk_root(guess):
            return guess.resolve(), "ENV_ROOT 猜测 %s" % guess
        tried.append("ENV_ROOT 猜测 %s 不成立" % guess)

    return None, tried


def resolve_project(sdk_root: Path):
    """返回 (path, source)；工程目录即 VERSIONS.md 所在目录。"""
    tried = []

    v = cli_value("--project")
    if v:
        p = Path(v).expanduser().resolve()
        tried.append("--project %s" % v)
        if (p / "VERSIONS.md").exists() or p.is_dir():
            return p, "--project"

    for name in PROJECT_ENV_VARS:
        v = os.environ.get(name)
        if v:
            p = Path(v).expanduser().resolve()
            tried.append("%s=%s" % (name, v))
            if p.is_dir():
                return p, "env %s" % name

    here = Path(__file__).resolve()
    # 脚本通常放在 <project>/tools/ 下
    if (here.parent.parent / "VERSIONS.md").exists():
        return here.parent.parent, "脚本位置"

    for p in ancestors(Path.cwd()):
        if (p / "VERSIONS.md").exists():
            return p, "向上查找 cwd"

    fallback = sdk_root.joinpath(*DEFAULT_PROJECT_TAIL)
    if fallback.is_dir():
        return fallback, "SDK 默认例程目录"

    return Path.cwd(), "当前工作目录（兜底）"


def print_paths(sdk_root, sdk_src, project, project_src):
    info("SDK   : %s   <- %s" % (sdk_root, sdk_src))
    info("工程  : %s   <- %s" % (project, project_src))
    info("配置文件: %s%s" % (CONFIG_PATH, "" if CONFIG_PATH.exists() else "（未创建）"))
    for name in SDK_ENV_VARS:
        if os.environ.get(name):
            info("环境变量: %s=%s" % (name, os.environ[name]))


# ---------------------------------------------------------------- 采集

def repo_info(path: Path, name: str, project: Path = None):
    info_d = {"name": name, "path": str(path).replace("\\", "/")}
    if not path.exists() or not sh(path, "rev-parse", "--is-inside-work-tree"):
        info_d["state"] = "not-a-git-repo"
        return info_d

    info_d["branch"] = sh(path, "branch", "--show-current") or "(detached)"
    info_d["commit"] = sh(path, "rev-parse", "HEAD")
    info_d["commit_short"] = info_d["commit"][:9] if info_d["commit"] else ""
    info_d["describe"] = sh(path, "describe", "--tags", "--always", "--dirty")
    info_d["nearest_tag"] = sh(path, "describe", "--tags", "--abbrev=0")
    info_d["commit_date"] = sh(path, "log", "-1", "--format=%ad", "--date=iso")

    for remote in ("origin", "upstream"):
        url = sh(path, "remote", "get-url", remote)
        if url:
            info_d["remote_" + remote] = url

    dirty = [l for l in sh(path, "status", "--porcelain").splitlines() if l.strip()]
    # 工程目录自身的改动不算"SDK 版本漂移"，否则每次写文档都会污染基线
    if project:
        try:
            rel = project.resolve().relative_to(path.resolve()).as_posix() + "/"
            dirty = [l for l in dirty if rel not in l.replace("\\", "/")]
        except ValueError:
            pass
    info_d["dirty"] = bool(dirty)
    info_d["dirty_files"] = [l.strip() for l in dirty[:20]]
    if len(dirty) > 20:
        info_d["dirty_files"].append("... (%d more)" % (len(dirty) - 20))

    for key, fn in (("version_txt", "version.txt"), ("version_dot", ".version")):
        f = path / fn
        if f.exists():
            # .version 可能是多行 KEY=VALUE，压成单行以免破坏 YAML
            vals = [l.strip() for l in f.read_text(encoding="utf-8", errors="replace").splitlines()
                    if l.strip()]
            if vals:
                info_d[key] = "; ".join(vals)
    return info_d


def render(sdk_root: Path, project: Path):
    sdk = repo_info(sdk_root, "sdk", project)
    win_idd = repo_info(sdk_root / "tools" / "win-idd", "win_idd", project)

    lines = []
    lines.append("recorded_at: %s" % datetime.now().strftime("%Y-%m-%d %H:%M"))
    lines.append("project: USB_Display")
    lines.append("board: sf32lb58-lcd_a128r32n1_qspi_hcpu")
    lines.append("")
    for d in (sdk, win_idd):
        lines.append("%s:" % d["name"])
        for k, v in d.items():
            if k == "name":
                continue
            if k == "dirty_files":
                if not v:
                    lines.append("  dirty_files: []")
                else:
                    lines.append("  dirty_files:")
                    for f in v:
                        lines.append("    - \"%s\"" % f.replace('"', '\\"'))
                continue
            if isinstance(v, bool):
                lines.append("  %s: %s" % (k, "true" if v else "false"))
            else:
                lines.append("  %s: \"%s\"" % (k, v))
        lines.append("")
    lines.append("env:")
    lines.append("  env_root: \"%s\"" % os.environ.get("ENV_ROOT", "F:/SiFli/env_1.2"))
    lines.append("  env_ver: \"%s\"" % os.environ.get("ENV_VER", ""))
    return "\n".join(lines).rstrip() + "\n"


def parse_block(text):
    data, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z_]+):\s*$", line)
        if m:
            cur = m.group(1)
            data[cur] = {}
            continue
        m = re.match(r"^  ([A-Za-z_]+):\s*\"?([^\"]*)\"?$", line)
        if m and cur:
            data[cur][m.group(1)] = m.group(2)
    return data


# ---------------------------------------------------------------- 主流程

def main():
    sdk_root, sdk_src = resolve_sdk_root()

    if sdk_root is None:
        warn("找不到 SDK 根目录，已尝试：")
        for t in (sdk_src or []):
            warn("    - " + str(t))
        print("""
提示：下面任意一种方式都可以指定（推荐第一种，一次生效，任何终端都能用）
  1) 写入用户配置（当前终端已能自动找到 SDK 时才可用）
        python version_snapshot.py --set-sdk
     等价于手工在任意终端执行：
        setx SIFLI_SDK_PATH "F:\\SiFli\\SDK\\SiFli-SDK\\main"
  2) 单次指定
        python version_snapshot.py --sdk F:\\SiFli\\SDK\\SiFli-SDK\\main
  3) 本次会话临时生效（PowerShell）
        $env:SIFLI_SDK_PATH = "F:\\SiFli\\SDK\\SiFli-SDK\\main"
""", file=sys.stderr)
        return 2

    project, project_src = resolve_project(sdk_root)

    if "--paths" in sys.argv:
        print_paths(sdk_root, sdk_src, project, project_src)
        return 0

    if "--set-sdk" in sys.argv:
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_PATH.write_text(str(sdk_root).replace("\\", "/") + "\n", encoding="utf-8")
        print("[ok] 已写入 %s" % CONFIG_PATH)
        print("     %s" % sdk_root)
        print("     之后在任何目录、任何终端运行本脚本都会用它。")
        print("     如需撤离：删除该文件，或 python version_snapshot.py --set-sdk --sdk 其它路径")
        return 0

    info("SDK   : %s   <- %s" % (sdk_root, sdk_src))
    info("工程  : %s   <- %s" % (project, project_src))

    yaml_text = render(sdk_root, project)
    doc = project / "VERSIONS.md"

    if "--write" in sys.argv:
        if not doc.exists():
            warn("%s 不存在，请先创建（含标记块）" % doc)
            return 2
        content = doc.read_text(encoding="utf-8")
        if BEGIN not in content or END not in content:
            warn("VERSIONS.md 缺少 %s / %s 标记" % (BEGIN, END))
            return 2
        new = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END),
                     BEGIN + "\n```yaml\n" + yaml_text + "```\n" + END,
                     content, flags=re.S)
        doc.write_text(new, encoding="utf-8")
        print("[ok] 已写入 %s" % doc)
        return 0

    if "--check" in sys.argv:
        if not doc.exists():
            warn("VERSIONS.md 不存在：%s" % doc)
            return 2
        content = doc.read_text(encoding="utf-8")
        m = re.search(re.escape(BEGIN) + r"(.*?)" + re.escape(END), content, flags=re.S)
        if not m:
            warn("VERSIONS.md 缺少标记块")
            return 2
        recorded = parse_block(m.group(1))
        current = parse_block(yaml_text)
        bad = 0
        for sec in ("sdk", "win_idd"):
            for key in ("branch", "commit", "dirty"):
                r = recorded.get(sec, {}).get(key, "?")
                c = current.get(sec, {}).get(key, "?")
                flag = "OK " if r == c else "DIFF"
                if r != c:
                    bad += 1
                print("  [%s] %-8s %-7s recorded=%s current=%s" % (flag, sec, key, r, c))
        print("\n%s" % ("[ok] 版本基线一致" if bad == 0 else "[!] 有 %d 处与记录不符" % bad))
        return 0 if bad == 0 else 1

    sys.stdout.write(yaml_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
