# -*- coding: utf-8 -*-
"""
Windows 触摸屏 -> 指定显示器 绑定工具（等价于 控制面板->平板电脑设置->配置）

原理（复刻 MultiDigiMon.exe）：
  1) 注册表 HKLM\\SOFTWARE\\Microsoft\\Wisp\\Pen\\Digimon
     值名 = "20-" + 触摸设备接口路径
     数据 = 显示器设备接口路径
  2) 立即生效：user32 私有导出 ordinal 2532 = NtUserSetDisplayMapping(HANDLE touchDevice, HMONITOR hMon)

用法:
  python touch_map.py            仅列出可绑定的触摸设备 / 显示器
  python touch_map.py apply      绑定 VID_303A 触摸屏到 1024x600 虚拟屏（写入注册表+立即生效）
  python touch_map.py apply --reg-only   只写注册表（重启后生效）
"""
import ctypes
import sys
import winreg
from ctypes import wintypes as wt

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

RIM_TYPEHID = 2
RIDI_DEVICENAME = 0x20000007
EDD_GET_DEVICE_INTERFACE_NAME = 1
TOUCH_VID_PID = "VID_303A&PID_2986&MI_01"


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
            res.append({"hmon": hmon, "gdi": mi.szDevice, "devid": devid,
                        "w": r.right - r.left, "h": r.bottom - r.top,
                        "pos": (r.left, r.top),
                        "primary": bool(mi.dwFlags & 1)})
        return True

    user32.EnumDisplayMonitors(None, None, PROC(cb), 0)
    return res


def set_display_mapping(hdev, hmon):
    """调用 user32!ordinal 2532 -> NtUserSetDisplayMapping，成功返回 True"""
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_char_p]
    addr = kernel32.GetProcAddress(user32._handle,
                                   ctypes.cast(ctypes.c_void_p(2532), ctypes.c_char_p))
    if not addr:
        return None  # 该 Windows 版本没有此导出
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, wt.HANDLE, wt.HANDLE)
    return proto(addr)(hdev, hmon) == 1


def save_registry(touch_path, mon_path):
    with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\Microsoft\Wisp\Pen\Digimon", 0,
                            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as k:
        winreg.SetValueEx(k, "20-" + touch_path, 0, winreg.REG_SZ, mon_path)


def main():
    hid = [(h, p) for h, p in raw_hid_devices() if TOUCH_VID_PID.lower() in p.lower()]
    mon = monitors()

    print("=== HID 触摸候选 ===")
    for h, p in hid:
        print("  handle=0x%X  %s" % (h or 0, p))
    print("=== 显示器 ===")
    for m in mon:
        print("  %-14s %dx%-4d @%-10s primary=%-5s %s" %
              (m["gdi"], m["w"], m["h"], str(m["pos"]), m["primary"], m["devid"]))

    if len(sys.argv) < 2 or sys.argv[1] != "apply":
        print("\n（仅列出。加 apply 参数执行绑定）")
        return 1

    if not hid:
        print("\n[!] 没找到触摸设备（VID_303A&PID_2986&MI_01），设备未连接？")
        return 2

    # 目标显示器：分辨率最小且非主屏的那块（虚拟屏 1024x600）
    cand = [m for m in mon if not m["primary"]]
    if not cand:
        print("\n[!] 没有找到副屏")
        return 2
    target = min(cand, key=lambda m: m["w"] * m["h"])
    if not target["devid"]:
        print("\n[!] 目标显示器没有设备接口路径（EnumDisplayDevices 失败）")
        return 2

    touch_h, touch_path = hid[0]
    print("\n绑定：\n  触摸 %s\n  显示 %s (%s %dx%d)" %
          (touch_path, target["devid"], target["gdi"], target["w"], target["h"]))

    save_registry(touch_path, target["devid"])
    print("[ok] 注册表已写入 Digimon")

    if "--reg-only" not in sys.argv:
        r = set_display_mapping(touch_h, target["hmon"])
        if r is None:
            print("[!] 当前系统没有 NtUserSetDisplayMapping(2532)，需重启生效")
        elif r:
            print("[ok] 已立即生效（NtUserSetDisplayMapping 返回成功）")
        else:
            print("[!] NtUserSetDisplayMapping 返回失败，重启后生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
