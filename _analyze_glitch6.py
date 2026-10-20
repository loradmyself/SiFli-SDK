# -*- coding: utf-8 -*-
"""Definitive parse: record ctr contiguity, credit accounting, seq-556 hole."""
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
recs = []
while off + 16 <= len(pcap):
    ts_sec, ts_usec, incl, orig = struct.unpack_from("<IIII", pcap, off)
    off += 16
    data = pcap[off:off + incl]; off += incl
    if data[:2] == b"\x06\x01" and len(data) >= 4:
        ln = struct.unpack_from("<H", data, 2)[0]
        body = data[4:4 + ln]
    else:
        body = data
    recs.append(body)

def parse(b):
    r = {"b": b}
    if len(b) < 8:
        r["k"] = "SHORT"; return r
    ctr = struct.unpack_from("<H", b, 0)[0]
    typ = struct.unpack_from("<H", b, 2)[0]
    dirb, sub = b[6], b[7]
    r.update(ctr=ctr, typ=typ, dir=dirb, sub=sub)
    if sub == 2:  # ACL
        i = b.find(b"\x41\x00\x80\x60")
        if i >= 0 and i + 10 <= len(b):
            r.update(k="MEDIA", hdl=0x80, seq=struct.unpack_from(">H", b, i + 4)[0],
                     rtp=struct.unpack_from(">I", b, i + 6)[0])
            return r
        i = b.find(b"\x64\x00\x80\x60")
        if i >= 0 and i + 10 <= len(b):
            r.update(k="MEDIA2", hdl=0x81, seq=struct.unpack_from(">H", b, i + 4)[0],
                     rtp=struct.unpack_from(">I", b, i + 6)[0])
            return r
        r["k"] = "ACL"
        r["hdl"] = 0x80 if (typ & 0xFFF) == 0x80 else ((typ & 0xFFF) == 0x81 and 0x81 or (typ & 0xFFF))
        # cid at b[12:14]
        r["cid"] = struct.unpack_from("<H", b, 12)[0] if len(b) >= 14 else -1
    elif sub == 4:  # event
        # event starts at b[8:]
        ev = b[8]
        r["evt"] = ev
        if ev == 0x13:
            r["k"] = "NCP"
            nh = b[10]
            q = 11
            ncp = []
            for _ in range(nh):
                if q + 4 > len(b): break
                hdl, cnt = struct.unpack_from("<HH", b, q)
                ncp.append((hdl & 0xFFF, cnt)); q += 4
            r["ncp"] = ncp
        else:
            r["k"] = f"EVT{ev:02x}"
    elif sub == 1:
        r["k"] = "CMD"
    else:
        r["k"] = "OTH"
    return r

parsed = []
for i, b in enumerate(recs):
    p = parse(b)
    p["t"] = times[i] if i < len(times) else 0
    p["i"] = i
    parsed.append(p)

print("kinds:", dict(Counter(p["k"] for p in parsed)))

def tsfmt(i):
    t = parsed[i]["t"]
    return f"{int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:06.3f}"

# ---- ctr contiguity
print("\n--- record ctr gaps (sniffer/log drops?) ---")
gaps = []
prev = None
for p in parsed:
    if prev is not None and p["ctr"] != (prev + 1) & 0xFFFF:
        gaps.append((p["i"], prev, p["ctr"], (p["ctr"] - prev - 1) & 0xFFFF))
    prev = p["ctr"]
print(f"ctr gaps: {len(gaps)}  total missing records: {sum(g[3] for g in gaps)}")
for gi, pv, nx, miss in gaps[:25]:
    print(f"   {tsfmt(gi)} ctr {pv} -> {nx} (missing {miss})")

# ---- credit accounting
print("\n--- ACL credit accounting (advertised buffers = 4) ---")
outst = 0; max_o = 0; over = []
for p in parsed:
    if p["k"] in ("MEDIA", "MEDIA2", "ACL"):
        outst += 1
        if outst > max_o: max_o = outst
        if outst > 4:
            over.append((p["i"], outst))
    elif p["k"] == "NCP":
        outst -= sum(c for _, c in p.get("ncp", []))
print(f"final outstanding={outst}  max={max_o}  moments>4: {len(over)}")
for i, o in over[:25]:
    print(f"   {tsfmt(i)} outstanding={o}")

# NCP latency
print("\n--- TX->NCP latency > 40ms ---")
pend = []
for p in parsed:
    if p["k"] in ("MEDIA", "MEDIA2", "ACL"):
        pend.append(p)
    elif p["k"] == "NCP" and pend:
        q = pend.pop(0)
        lat = (p["t"] - q["t"]) * 1000
        if lat > 40:
            print(f"   {tsfmt(q['i'])} {q['k']} -> NCP latency {lat:.1f}ms")

# ---- full window around seq 555-557 with ctr
print("\n--- all records 13:51:30.10-30.30 (ctr, kind) ---")
t0 = 13 * 3600 + 51 * 60 + 30.10
t1 = 13 * 3600 + 51 * 60 + 30.30
for p in parsed:
    if t0 <= p["t"] <= t1:
        extra = ""
        if p["k"] == "MEDIA":
            extra = f" seq={p['seq']} ts={p['rtp']}"
        if p["k"] == "NCP":
            extra = f" {p.get('ncp')}"
        print(f"{tsfmt(p['i'])} ctr={p['ctr']:5d} {p['k']:8s}{extra}  {p['b'][:24].hex()}")
