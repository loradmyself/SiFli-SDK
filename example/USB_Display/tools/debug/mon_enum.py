import ctypes
from ctypes import wintypes as wt
user32 = ctypes.windll.user32
MONITORENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(wt.RECT), ctypes.c_double)

class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT),
                ("dwFlags", wt.DWORD), ("szDevice", wt.WCHAR*32)]

def cb(hmon, hdc, lprect, lparam):
    mi = MONITORINFOEXW(); mi.cbSize = ctypes.sizeof(MONITORINFOEXW)
    if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
        print("%s primary=%s rect=(%d,%d)-(%d,%d)" % (mi.szDevice, bool(mi.dwFlags & 1),
              mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom))
    return 1
user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
