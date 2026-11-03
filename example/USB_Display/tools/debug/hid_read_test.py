# -*- coding: utf-8 -*-
# 直接打开 SiFli 触摸 HID 设备读取原始报告（绕过 Raw Input / Digitizer 层）
import ctypes, ctypes.wintypes as wt, time, sys
k32 = ctypes.windll.kernel32
GENERIC_READ  = 0x80000000
GENERIC_WRITE = 0x40000000
SHARE = 0x3          # FILE_SHARE_READ|WRITE
OPEN_EXISTING = 3

path = "\\\\?\\HID#VID_303A&PID_2986&MI_01#8&e917dbb&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"
h = k32.CreateFileW(path, GENERIC_READ, SHARE, None, OPEN_EXISTING, 0, None)
if h == -1 or h == 0xFFFFFFFFFFFFFFFF:
    err = k32.GetLastError()
    print("CreateFile FAILED err=%d" % err)
    if err == 5:
        print("access denied - try running as admin or close other readers")
    sys.exit(1)
print("opened OK, reading for 90s... TOUCH THE PANEL NOW")

# 查询 caps 得到报告长度
buf = ctypes.create_string_buffer(64)
rd  = wt.DWORD()
count = 0
end = time.time() + 90
while time.time() < end:
    ok = k32.ReadFile(h, buf, 64, ctypes.byref(rd), None)
    if ok and rd.value > 0:
        count += 1
        data = buf.raw[:rd.value]
        print("%s READ %d bytes: %s" % (time.strftime("%H:%M:%S"), rd.value, data.hex()))
        if count >= 40:
            break
    else:
        err = k32.GetLastError()
        if err == 0x3E5:  # ERROR_IO_PENDING shouldn't happen w/o overlapped
            pass
        else:
            print("ReadFile err=%d" % err)
            time.sleep(0.05)
k32.CloseHandle(h)
print("total reads: %d" % count)
