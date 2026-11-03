# -*- coding: utf-8 -*-
import ctypes
from ctypes import wintypes as wt
user32 = ctypes.windll.user32
class RAWINPUTDEVICELIST(ctypes.Structure):
    _fields_ = [("hDevice", wt.HANDLE), ("dwType", wt.DWORD)]
n = wt.UINT(0)
user32.GetRawInputDeviceList(None, ctypes.byref(n), ctypes.sizeof(RAWINPUTDEVICELIST))
lst = (RAWINPUTDEVICELIST * n.value)()
user32.GetRawInputDeviceList(lst, ctypes.byref(n), ctypes.sizeof(RAWINPUTDEVICELIST))
print("raw input devices:", n.value)
for i in range(n.value):
    h = lst[i].hDevice
    if not h: continue
    sz = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(h, 0x20000007, None, ctypes.byref(sz))
    name = ctypes.create_unicode_buffer(sz.value+1)
    user32.GetRawInputDeviceInfoW(h, 0x20000007, name, ctypes.byref(sz))
    mark = " <<<<<" if "303A" in name.value else ""
    print("type=%d %s%s" % (lst[i].dwType, name.value[:80], mark))
    if "303A" in name.value:
        # 查 usage
        class RID_DEV_INFO(ctypes.Structure):
            _fields_ = [("cbSize", wt.DWORD), ("dwType", wt.DWORD),
                        ("w1", wt.DWORD), ("w2", wt.DWORD), ("w3", wt.DWORD), ("w4", wt.DWORD)]
        d = RID_DEV_INFO(); d.cbSize = ctypes.sizeof(RID_DEV_INFO)
        dsz = wt.UINT(ctypes.sizeof(RID_DEV_INFO))
        user32.GetRawInputDeviceInfoW(h, 0x2000000b, ctypes.byref(d), ctypes.byref(dsz))  # RIDI_DEVICEINFO
        print("   usagePage=0x%04X usage=0x%04X" % (d.w3 & 0xFFFF, d.w4 & 0xFFFF))
