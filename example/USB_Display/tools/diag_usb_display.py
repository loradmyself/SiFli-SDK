# -*- coding: utf-8 -*-
"""
USB Display 环境自检（PC 侧）

一次性打印：
  1. 桌面显示器拓扑（GDI 设备名 / 分辨率 / 位置 / monitor 设备接口路径）
  2. VID_303A&PID_2986 的 HID 触摸设备（Raw Input 路径 + 句柄）
  3. HKLM\\SOFTWARE\\Microsoft\\Wisp\\Pen\\Digimon 里的触摸->显示器绑定
  4. 结论：触摸是否已绑定到 1024x600 虚拟屏

用法: python diag_usb_display.py
"""
import ctypes
import winreg
from ctypes import wintypes as wt

user32 = ctypes.windll.user32

RIM_TYPEHID = 2
RIDI_DEVICENAME = 0x20000007
EDD_GET_DEVICE_INTERFACE_NAME = 1
TOUCH_KEY = "VID_303A&PID_2986&MI_01"
DIGIMON = r"SOFTWARE\Microsoft\Wisp\Pen\Digimon"


class RAWINPUTDEVICELIST(ctypes.Structure):
    _fields_ = [("hDevice", wt.HANDLE), ("dwType", wt.DWORD)]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT),
                ("dwFlags", wt.DWORD), ("szDevice", wt.WCHAR * 32)]


class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("DeviceName", wt.WCHAR * 32),
                ("DeviceString", wt.WCHAR * 128), ("StateFlags", wt.DWORD),
                ("DeviceID", wt.WCHAR * 128), ("DeviceKey", wt.WCHAR * 128)]


def raw_hid_devices():
    n = wt.UINT(0)
    user32.GetRawInputDeviceList(None, ctypes.byref(n), ctypes.sizeof(RAWINPUTDEVICELIST))
    if n.value == 0:
        return []
    arr = (RAWINPUTDEVICELIST * n.value)()
    user32.GetRawInputDeviceList(arr, ctypes.byref(n), ctypes.sizeof(RAWINPUTDEVICELIST))
    out = []
    for i in range(n.value):
        if arr[i].dwType != RIM_TYPEHID:
            continue
        ln = wt.UINT(512)
        buf = ctypes.create_unicode_buffer(512)
        r = user32.GetRawInputDeviceInfoW(arr[i].hDevice, RIDI_DEVICENAME, buf,
                                          ctypes.byref(ln))
        if r and r != 0xFFFFFFFF:
            out.append((arr[i].hDevice, buf.value))
    return out


def monitors():
    res = []
    PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HANDLE, wt.HANDLE,
                              ctypes.POINTER(wt.RECT), wt.LPARAM)

    def cb(hmon, hdc, lprc, lp):
        mi = MONITORINFOEXW()
        mi.cbSize = ctypes.sizeof(MONITORINFOEXW)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            d = DISPLAY_DEVICEW()
            d.cb = ctypes.sizeof(DISPLAY_DEVICEW)
            devid = ""
            if user32.EnumDisplayDevicesW(mi.szDevice, 0, ctypes.byref(d),
                                          EDD_GET_DEVICE_INTERFACE_NAME):
                devid = d.DeviceID
            r = mi.rcMonitor
            res.append({"gdi": mi.szDevice, "devid": devid,
                        "w": r.right - r.left, "h": r.bottom - r.top,
                        "pos": (r.left, r.top), "primary": bool(mi.dwFlags & 1)})
        return True

    user32.EnumDisplayMonitors(None, None, PROC(cb), 0)
    return res


def digimon_map():
    out = {}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, DIGIMON, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            i = 0
            while True:
                try:
                    name, val, _ = winreg.EnumValue(k, i)
                except OSError:
                    break
                out[name] = val
                i += 1
    except FileNotFoundError:
        pass
    return out


def main():
    mons = monitors()
    hids = [(h, p) for h, p in raw_hid_devices() if TOUCH_KEY.lower() in p.lower()]
    mapping = digimon_map()

    print("=" * 78)
    print("桌面显示器")
    print("=" * 78)
    for m in mons:
        print("  %-13s %4dx%-4d @%-12s %-8s %s" %
              (m["gdi"], m["w"], m["h"], str(m["pos"]),
               "PRIMARY" if m["primary"] else "", m["devid"]))

    print()
    print("=" * 78)
    print("HID 触摸设备 (VID_303A&PID_2986&MI_01)")
    print("=" * 78)
    if not hids:
        print("  [!] 未找到 —— 板子没连、固件没跑，或 MI_01 未枚举")
    for h, p in hids:
        print("  handle=0x%X" % h)
        print("  path  =%s" % p)

    print()
    print("=" * 78)
    print("触摸 -> 显示器 绑定 (Wisp\\Pen\\Digimon)")
    print("=" * 78)
    if not mapping:
        print("  (空) —— 触摸未绑定，Windows 会按默认规则送到主屏")
    for k, v in mapping.items():
        print("  %s" % k)
        print("    -> %s" % v)

    # ---- 结论 ----
    print()
    print("=" * 78)
    print("结论")
    print("=" * 78)
    if not hids:
        print("  [X] 触摸设备未就绪")
        return
    if not mapping:
        print("  [X] 未绑定。运行:  python tools/touch_map.py apply")
        return

    touch_path = hids[0][1]
    hit = mapping.get("20-" + touch_path) or mapping.get(touch_path)
    if not hit:
        print("  [!] 有绑定记录，但没有匹配当前触摸路径的项（设备路径变了？重跑 apply）")
        for k in mapping:
            if "303A" in k.upper():
                print("      现存的 303A 记录: %s" % k)
        return

    target = next((m for m in mons if m["devid"].lower() == hit.lower()), None)
    if target:
        print("  [OK] 已绑定到 %s  %dx%d @%s" %
              (target["gdi"], target["w"], target["h"], str(target["pos"])))
        if (target["w"], target["h"]) == (1024, 600):
            print("       目标就是虚拟副屏，配置正确")
        else:
            print("       [!] 目标分辨率不是 1024x600，可能绑错屏")
    else:
        print("  [!] 绑定的显示器 '%s' 当前不存在（显示器被拔掉/换了口？）" % hit)


if __name__ == "__main__":
    main()
