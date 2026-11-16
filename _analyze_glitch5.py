# -*- coding: utf-8 -*-
"""Clean parse v2: proper body layout + credit accounting."""
import struct
from pathlib import Path
from collections import Counter

BASE = Path(r"F:\SiFli\SDK\SiFli-SDK\main\example\Dual_Bluetooth_Headset_Walkie-Talkie\project\log\卡顿log")
STEM = "窗口2_COM47_(2026-10-20-13-50-03)"
pcap = (BASE / (STEM + ".pcap")).read_bytes()
bintime = (BASE / (STEM + ".bintime")).read_bytes()

times = []
for o in range(0, len(bintime) - 15, 16):
    y, mo, q, d, h, mi, s, ms = struct.unpack_from("<8H", bintime, o)
    times.append(h * 3600 + mi * 60 + s + ms / 1000.0)

off = 24
pkts = []
while off + 16 <= len(pcap):
    ts_sec, ts_usec, incl, orig = struct.unpack_from("<IIII", pcap, off)
    off += 16
    data = pcap[off:off + incl]
    off += incl
    if data[:2] == b"\x06\x01" and len(data) >= 4:
        ln = struct.unpack_from("<H", data, 2)[0]
        body = data[4:4 + ln]
    else:
        body = data
    pkts.append(body)
print("pkts:", len(pkts))

# dump bodies of first 30 and around specific windows
def tsfmt(i):
    t = times[i]
    return f"{int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:06.3f}"

print("\n--- first 30 bodies ---")
for i in range(30):
    print(f"{tsfmt(i)} {pkts[i].hex()}")

print("\n--- bodies around 13:51:30.15-30.25 ---")
for i, b in enumerate(pkts):
    t = times[i]
    if t and abs(t - (13 * 3600 + 51 * 60 + 30.2)) < 0.06:
        print(f"{tsfmt(i)} {b[:40].hex()}")
