# -*- coding: utf-8 -*-
"""Clean standard-pcap parse: exact ACL credit accounting + payload truth."""
import struct
from pathlib import Path

BASE = Path(r"F:\SiFli\SDK\SiFli-SDK\main\example\Dual_Bluetooth_Headset_Walkie-Talkie\project\log\卡顿log")
STEM = "窗口2_COM47_(2026-10-20-13-50-03)"
pcap = (BASE / (STEM + ".pcap")).read_bytes()

# standard pcap global header 24 bytes
magic = pcap[:4]
print("pcap magic:", magic.hex())
off = 24
pkts = []   # (ts_usec_raw, payload)
while off + 16 <= len(pcap):
    ts_sec, ts_usec, incl, orig = struct.unpack_from("<IIII", pcap, off)
    off += 16
    if off + incl > len(pcap):
        break
    data = pcap[off:off + incl]
    off += incl
    # Frontline wrapper: 06 01 <len16 LE> <payload[len]>
    if data[:2] == b"\x06\x01" and len(data) >= 4:
        ln = struct.unpack_from("<H", data, 2)[0]
        body = data[4:4 + ln]
    else:
        body = data
    pkts.append((ts_sec, ts_usec, body))

print(f"packets={len(pkts)}  tail_bytes={len(pcap)-off}")

def tsfmt(t):
    h = int(t // 3600); m = int(t % 3600 // 60); s = t % 60
    return f"{h:02d}:{m:02d}:{s:06.3f}"

# body structure (from hex study): <u16 type> <u32 zero> <u16 ctr> <hci...>
# type 0x0001 = HCI command, 0x0013 = NCP event, others = events by code
recs = []
for ts_sec, ts_usec, b in pkts:
    r = {"b": b, "t": ts_usec / 1e6 if ts_usec < 1e6 else ts_usec}  # placeholder
    recs.append(r)

# bintime for wall-clock
bintime = (BASE / (STEM + ".bintime")).read_bytes()
times = []
for o in range(0, len(bintime) - 15, 16):
    y, mo, q, d, h, mi, s, ms = struct.unpack_from("<8H", bintime, o)
    times.append(h * 3600 + mi * 60 + s + ms / 1000.0)
print("bintime entries:", len(times), "pcap pkts:", len(pkts))

def parse(b):
    r = {}
    if len(b) < 4:
        return {"k": "SHORT"}
    # try structures
    typ = struct.unpack_from("<H", b, 0)[0]
    r["type"] = typ
    if typ == 0x0001:  # HCI command
        r["k"] = "CMD"
        r["hex"] = b.hex()
    elif b[2:4] == b"\x13\x05":
        r["k"] = "NCP"
        j = b.find(b"\x13\x05")
        nh = b[j + 2]
        q = j + 3
        ncp = []
        for _ in range(nh):
            if q + 4 > len(b):
                break
            hdl, cnt = struct.unpack_from("<HH", b, q)
            ncp.append((hdl & 0xFFF, cnt))
            q += 4
        r["ncp"] = ncp
    else:
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
        # ACL?
        if len(b) >= 12 and b[8:10] in (b"\x80\x20", b"\x81\x20"):
            r["k"] = "ACL"
            r["hdl"] = 0x80 if b[8] == 0x80 else 0x81
            r["cid"] = struct.unpack_from("<H", b, 12)[0] if len(b) >= 14 else -1
            r["hex"] = b[8:22].hex()
        elif typ in (0x0E00, 0x0F00) or b[:1] in (b"\x0e", b"\x0f"):
            r["k"] = "EVT"
            r["hex"] = b.hex()
        else:
            r["k"] = "OTH"
            r["hex"] = b.hex()
    return r

for i, (tsec, tusec, b) in enumerate(pkts):
    p = parse(b)
    p["t"] = times[i] if i < len(times) else 0
    p["i"] = i
    recs[i] = p

from collections import Counter
print("kinds:", dict(Counter(r["k"] for r in recs)))

# sample a few raw bodies of each kind
seen = set()
for r in recs:
    if r["k"] not in seen:
        seen.add(r["k"])
        print(f"  sample {r['k']}: {r['b'][:28].hex() if 'b' in r else ''}")

# ---- global credit accounting (controller: 4 ACL buffers total)
outst = 0
max_o = 0
over = []
evts = []
for r in recs:
    if r["k"] in ("MEDIA", "MEDIA2", "ACL"):
        outst += 1
        evts.append((r["t"], "TX", outst))
        if outst > max_o:
            max_o = outst
        if outst > 4:
            over.append((r["t"], outst))
    elif r["k"] == "NCP":
        tot = sum(c for _, c in r.get("ncp", []))
        outst -= tot
        evts.append((r["t"], "NCP", outst))

print(f"\ncredit: max_outstanding={max_o}  moments_out>4: {len(over)}")
for t, o in over[:20]:
    print(f"   {tsfmt(t)} outstanding={o}")

# max outstanding per second over the whole capture
print("\n--- max outstanding per second ---")
buck = {}
for t, k, o in evts:
    b = int(t)
    buck[b] = max(buck.get(b, 0), o)
for b in sorted(buck):
    if buck[b] >= 3:
        print(f"   {tsfmt(b)} max_out={buck[b]}")

# NCP latency: time from each TX to the next NCP
print("\n--- NCP latency spikes (TX -> next NCP) > 40ms ---")
pending = []
for r in recs:
    if r["k"] in ("MEDIA", "MEDIA2", "ACL"):
        pending.append(r["t"])
    elif r["k"] == "NCP" and pending:
        t0 = pending.pop(0)
        lat = (r["t"] - t0) * 1000
        if lat > 40:
            print(f"   {tsfmt(t0)} -> {tsfmt(r['t'])}  latency={lat:.1f}ms")
