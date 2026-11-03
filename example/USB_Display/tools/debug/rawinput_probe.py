# 监听 Windows Raw Input：验证 HID 触摸报告是否到达应用层
import ctypes, time, sys
from ctypes import wintypes as wt

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WM_INPUT = 0x00FF
WM_DESTROY = 0x0002
RIDEV_INPUTSINK = 0x00000100
RID_INPUT = 0x10000003
RIM_INPUT = 0x0
RIM_INPUTSINK = 0x1

LOG_PATH = r"F:\SiFli\rawinput.log"
DURATION = 150  # 秒后自动退出
log = open(LOG_PATH, "w", encoding="utf-8", buffering=1)

def out(msg):
    log.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))

class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_longlong)

class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HANDLE), ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HBRUSH), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HANDLE)]

class MSG(ctypes.Structure):
    _fields_ = [("hwnd", wt.HWND), ("message", wt.UINT), ("wParam", ctypes.c_size_t),
                ("lParam", ctypes.c_longlong), ("time", wt.DWORD),
                ("pt", wt.POINT), ("lPrivate", wt.DWORD)]

class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD), ("hDevice", wt.HANDLE), ("wParam", ctypes.c_size_t)]

class RAWHID(ctypes.Structure):
    _fields_ = [("dwSizeHid", wt.DWORD), ("dwCount", wt.DWORD), ("bRawData", ctypes.c_byte * 64)]

seen_devices = {}
dev_count = [0]

def get_dev_path(hdev):
    if hdev in seen_devices:
        return seen_devices[hdev]
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(hdev, 0x20000007, None, ctypes.byref(size))  # RIDI_DEVICENAME
    buf = ctypes.create_unicode_buffer(size.value + 1)
    user32.GetRawInputDeviceInfoW(hdev, 0x20000007, buf, ctypes.byref(size))
    seen_devices[hdev] = buf.value
    return buf.value

def wndproc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER))
        buf = ctypes.create_string_buffer(size.value)
        r = user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER))
        if r:
            hdr = ctypes.cast(buf, ctypes.POINTER(RAWINPUTHEADER)).contents
            path = get_dev_path(hdr.hDevice)
            if hdr.dwType == 2:  # RIM_TYPEHID
                rhid = ctypes.cast(ctypes.byref(buf, ctypes.sizeof(RAWINPUTHEADER)), ctypes.POINTER(RAWHID)).contents
                n = rhid.dwSizeHid * rhid.dwCount
                data = bytes(bytearray(rhid.bRawData)[:n])
                dev_count[0] += 1
                out("HID %s raw=%s" % (path.split("{")[-1][:24], data.hex()))
            else:
                out("type=%d dev=%s" % (hdr.dwType, path[:60]))
        return 0
    elif msg == WM_DESTROY:
        return 0
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

wndproc_cb = WNDPROC(wndproc)

hinst = kernel32.GetModuleHandleW(None)
wc = WNDCLASSEXW()
wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
wc.lpfnWndProc = wndproc_cb
wc.hInstance = hinst
wc.lpszClassName = "RawInputProbe"
if not user32.RegisterClassExW(ctypes.byref(wc)):
    out("RegisterClass failed %d" % ctypes.GetLastError())
    sys.exit(1)

hwnd = user32.CreateWindowExW(0, "RawInputProbe", "probe", 0, 0, 0, 0, 0, None, None, hinst, None)
if not hwnd:
    out("CreateWindow failed %d" % ctypes.GetLastError())
    sys.exit(1)

# 注册：触摸屏(0x0D/0x04)、digitizer(0x0D/0x01)、鼠标(0x01/0x02 作对照)
rids = (RAWINPUTDEVICE * 3)(
    RAWINPUTDEVICE(0x0D, 0x04, RIDEV_INPUTSINK, hwnd),
    RAWINPUTDEVICE(0x0D, 0x01, RIDEV_INPUTSINK, hwnd),
    RAWINPUTDEVICE(0x01, 0x02, RIDEV_INPUTSINK, hwnd),
)
if not user32.RegisterRawInputDevices(ctypes.byref(rids), 3, ctypes.sizeof(RAWINPUTDEVICE)):
    out("RegisterRawInputDevices failed %d" % ctypes.GetLastError())
    sys.exit(1)

out("=== raw input listener started (usages: touchscreen/digitizer/mouse) ===")

msg = MSG()
end = time.time() + DURATION
while time.time() < end:
    r = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
    if r <= 0:
        break
    user32.TranslateMessage(ctypes.byref(msg))
    user32.DispatchMessageW(ctypes.byref(msg))
    if msg.message == WM_INPUT and dev_count[0] >= 400:
        break

out("=== done, hid_reports=%d ===" % dev_count[0])
log.close()
