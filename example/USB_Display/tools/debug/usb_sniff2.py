# -*- coding: utf-8 -*-
# 通过 USB Hub IOCTL 递归扫描，找 SiFli 设备并从总线上读 Report/Config Descriptor
import ctypes, ctypes.wintypes as wt

kernel32 = ctypes.windll.kernel32
IOCTL_GET_NODE_INFO    = 0x00220408
IOCTL_GET_DESCRIPTOR   = 0x00220410
IOCTL_GET_CONN_NAME    = 0x00220414
IOCTL_GET_CONN_INFO_EX = 0x00220448

def open_path(p):
    h = kernel32.CreateFileW(p, 0xC0000000, 3, None, 3, 0, None)
    return None if h in (-1, 0xFFFFFFFFFFFFFFFF) else h

HUB_G = "f18a0e88-c30c-11d0-8815-00a0c906bed8"

def conn_info(h, port):
    inb = ctypes.create_string_buffer(4)
    ctypes.memmove(inb, ctypes.byref(ctypes.c_int(port)), 4)
    out = ctypes.create_string_buffer(1024); br = wt.DWORD()
    if not kernel32.DeviceIoControl(h, IOCTL_GET_CONN_INFO_EX, inb, 4, out, 1024, ctypes.byref(br), None):
        return None
    raw = out.raw[:br.value]
    if len(raw) < 24: return None
    return {"vid": int.from_bytes(raw[12:14], "little"),
            "pid": int.from_bytes(raw[14:16], "little"),
            "class": raw[8]}

def get_desc(h, port, dtype, wIndex, length):
    pkt = bytes([0x81, 0x06, 0x00, dtype, wIndex & 0xFF, 0x00, length & 0xFF, (length >> 8) & 0xFF])
    inb = ctypes.create_string_buffer(12)
    ctypes.memmove(inb, ctypes.byref(ctypes.c_int(port)), 4)
    ctypes.memmove(ctypes.byref(inb, 4), pkt, 8)
    out = ctypes.create_string_buffer(12 + length); br = wt.DWORD()
    if not kernel32.DeviceIoControl(h, IOCTL_GET_DESCRIPTOR, inb, 12, out, 12 + length, ctypes.byref(br), None):
        return None, kernel32.GetLastError()
    return out.raw[12:br.value], 0

def walk(hub_path, found, depth=0):
    h = open_path(hub_path)
    if not h:
        print("  " * depth + "hub open failed err=%d:" % kernel32.GetLastError(), hub_path)
        return
    out = ctypes.create_string_buffer(512); br = wt.DWORD()
    nports = 4
    if kernel32.DeviceIoControl(h, IOCTL_GET_NODE_INFO, None, 0, out, 512, ctypes.byref(br), None):
        nports = out.raw[6]  # HubDescriptor.bNumberOfPorts
    pad = "  " * depth
    print("%s[hub ports=%d]" % (pad, nports))
    for port in range(1, nports + 1):
        ci = conn_info(h, port)
        if not ci: continue
        if ci["class"] == 9:  # 下级 hub，递归
            inb2 = ctypes.create_string_buffer(4)
            ctypes.memmove(inb2, ctypes.byref(ctypes.c_int(port)), 4)
            out2 = ctypes.create_string_buffer(1024); br2 = wt.DWORD()
            if kernel32.DeviceIoControl(h, IOCTL_GET_CONN_NAME, inb2, 4, out2, 1024, ctypes.byref(br2), None):
                name = out2.raw[:br2.value][8:].decode("utf-16-le", "ignore").rstrip("\x00")
                if name.startswith("\\\\"):
                    pass
                elif name.startswith("\\??\\"):
                    name = "\\\\.\\" + name[4:]
                else:
                    name = "\\\\.\\" + name
                print("%s  -> 下级hub: %s" % (pad, name))
                walk(name, found, depth + 1)
            else:
                print("%s  -> GET_CONN_NAME err=%d" % (pad, kernel32.GetLastError()))
            continue
        if ci["vid"] == 0:
            continue
        print("%sport %d: vid=%04x pid=%04x class=%02x" % (pad, port, ci["vid"], ci["pid"], ci["class"]))
        if ci["vid"] == 0x303A:
            found.append((h, port, ci))
    # 注意：handle 保留给 found 使用

found = []
for inst in ["4&dc22705&0&0", "4&c69a899&0&0"]:
    walk("\\\\?\\usb#root_hub30#%s#{%s}" % (inst, HUB_G), found)
print("SiFli 连接数:", len(found))
for h, port, ci in found:
    print("=== port %d REPORT DESCRIPTOR (wIndex=1) ===" % port)
    d, err = get_desc(h, port, 0x22, 1, 128)
    if d:
        print("len=%d" % len(d))
        print(" ".join("%02X" % b for b in d))
    else:
        print("err=%d" % err)
    c, err = get_desc(h, port, 0x02, 0, 128)
    if c:
        print("=== port %d CONFIG DESCRIPTOR ===" % port)
        print(" ".join("%02X" % b for b in c))
