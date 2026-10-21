# -*- coding: utf-8 -*-
"""Forensic parse of Frontline custom pcap + bintime for the walkie-talkie glitch."""
import struct
import sys
from pathlib import Path

BASE = Path(r"F:\SiFli\SDK\SiFli-SDK\main\example\Dual_Bluetooth_Headset_Walkie-Talkie\project\log\卡顿log")
STEM = "窗口2_COM47_(2026-10-20-13-50-03)"

pcap = (BASE / (STEM + ".pcap")).read_bytes()
bintime = (BASE / (STEM + ".bintime")).read_bytes()

# ---- bintime: 16-byte records of 8*u16: y, m, ?, d, H, M, S, ms
times = []
for off in range(0, len(bintime) - 15, 16):
    y, mo, q, d, h, mi, s, ms = struct.unpack_from("<8H", bintime, off)
    times.append((h, mi, s, ms, y, mo, d))

# ---- pcap frames: 06 01 <len16 LE> <seq16 LE> <payload len>
frames = []
off = 0
bad = 0
while off + 6 <= len(pcap):
    m0, m1 = pcap[off], pcap[off + 1]
    if (m0, m1) != (0x06, 0x01):
        # try to resync on next 06 01
        nxt = pcap.find(b"\x06\x01", off + 1)
        if nxt < 0:
            break
        bad += 1
        off = nxt
        continue
    ln, fseq = struct.unpack_from("<HH", pcap, off + 2)
    if off + 6 + ln > len(pcap):
        bad += 1
        break
    payload = pcap[off + 6: off + 6 + ln]
    frames.append((fseq, payload))
    off += 6 + ln

print(f"frames={len(frames)} timestamps={len(times)} bad={bad} bytes_left={len(pcap)-off}")

def ts_str(i):
    h, mi, s, ms = times[i][0], times[i][1], times[i][2], times[i][3]
    return f"{h:02d}:{mi:02d}:{s:02d}.{ms:03d}"

def sec_of(i):
    h, mi, s, ms = times[i][:4]
    return h * 3600 + mi * 60 + s + ms / 1000.0

# ---- classify
def classify(p):
    """return dict"""
    d = {"kind": "UNK", "raw": p}
    if len(p) >= 8:
        hdl_fl, acl_len = struct.unpack_from("<HH", p, 0)
        hdl = hdl_fl & 0x0FFF
        l2len, cid = struct.unpack_from("<HH", p, 4)
        d["hdl"] = hdl
        d["cid"] = cid
        d["pb"] = (hdl_fl >> 12) & 3
        d["acl_len"] = acl_len
        d["l2len"] = l2len
    # media: RTP over CID 0x0041
    i = p.find(b"\x41\x00\x80\x60")
    if i >= 0 and i + 10 <= len(p):
        seq = struct.unpack_from(">H", p, i + 4)[0]
        tsv = struct.unpack_from(">I", p, i + 6)[0]
        d.update(kind="MEDIA", rtp_seq=seq, rtp_ts=tsv, rtp_off=i)
        return d
    if len(p) >= 8:
        cid = d.get("cid", -1)
        if cid == 0x0001:
            d["kind"] = "SIG"
        elif cid == 0x0006:
            d["kind"] = "SDP"
        elif cid in (0x0040, 0x0041, 0x0042, 0x0043):
            # dynamic cid, no RTP marker -> AVDTP signaling
            d["kind"] = f"AVDTPSIG-cid{cid:04x}"
        elif cid in (0x0044, 0x0045, 0x0046, 0x0047, 0x0048):
            d["kind"] = f"RFCOMM-cid{cid:04x}"
        elif cid > 0x003F:
            d["kind"] = f"DYN-cid{cid:04x}"
    if p[:1] == b"\x04" and len(p) >= 2:
        ev = p[1]
        d["kind"] = f"EVT-{ev:02x}"
        d["evt"] = ev
        if ev == 0x13 and len(p) >= 7:
            d["kind"] = "NCP"
            nh = p[2]
            d["ncp"] = []
            q = 3
            for _ in range(nh):
                if q + 4 > len(p):
                    break
                hdlv, cnt = struct.unpack_from("<HH", p, q)
                d["ncp"].append((hdlv & 0xFFF, cnt))
                q += 4
    return d

cls = []
for idx, (fseq, p) in enumerate(frames):
    c = classify(p)
    c["i"] = idx
    c["fseq"] = fseq
    c["t"] = times[idx] if idx < len(times) else (0, 0, 0, 0, 0, 0, 0)
    cls.append(c)

# magic histogram
from collections import Counter
mag = Counter((f[1][:2].hex() if f[1] else "empty") for f in frames)
print("payload-head histogram (top 10):", mag.most_common(10))

kinds = Counter(c["kind"] for c in cls)
print("kinds:", dict(kinds))

# media streams by handle
media = [c for c in cls if c["kind"] == "MEDIA"]
print(f"media packets: {len(media)}")
by_hdl = Counter(c.get("hdl") for c in media)
print("media by handle:", dict(by_hdl))
if media:
    print("rtp seq range:", min(c["rtp_seq"] for c in media), "-", max(c["rtp_seq"] for c in media))

def dump_window(t0, t1, label, show_all=False, media_every=True):
    print(f"\n===== {label}: window {t0:.3f} - {t1:.3f} =====")
    prev = None
    for c in cls:
        s = sec_of(c["i"])
        if s < t0 or s > t1:
            continue
        stamp = ts_str(c["i"])
        gap = (s - prev) if prev is not None else 0.0
        prev = s
        mark = f" +{gap*1000:6.1f}ms" if gap > 0.04 else " " * 11
        if c["kind"] == "MEDIA":
            print(f"{stamp}{mark} MEDIA hdl=0x{c.get('hdl',0):03x} rtp={c['rtp_seq']:5d} ts={c['rtp_ts']:8d} len={len(c['raw'])}")
        elif c["kind"] == "NCP":
            print(f"{stamp}{mark} NCP   {c.get('ncp')}")
        elif c["kind"] == "EVT-0e" or c["kind"] == "EVT-13" or c["kind"].startswith("EVT"):
            p = c["raw"]
            print(f"{stamp}{mark} {c['kind']:6s} {p[:18].hex()}")
        else:
            if show_all or gap > 0.04 or c["kind"] != "SIG":
                hx = c["raw"][:16].hex()
                print(f"{stamp}{mark} {c['kind']:16s} hdl=0x{c.get('hdl',0):03x} len={len(c['raw']):4d} {hx}")

# find largest media gaps overall
print("\n===== media inter-packet gaps > 45ms =====")
prev_s = None
for c in media:
    s = sec_of(c["i"])
    if prev_s is not None:
        gap = (s - prev_s) * 1000
        if gap > 45:
            print(f"gap {gap:7.1f}ms  {ts_str(c['i'])}  hdl=0x{c.get('hdl',0):03x} rtp={c['rtp_seq']}  prev_ts={ts_str(c['i']-1) if c['i']>0 else ''}")
    prev_s = s

def secs(hh, mm, ss, ms=0):
    return hh * 3600 + mm * 60 + ss + ms / 1000.0

# Window A: around 13:51:30 sync error / missing seq 556
dump_window(secs(13, 51, 28, 500), secs(13, 51, 31, 500), "A: 13:51:28.5-31.5 (SBC sync err / missing 556)")

# Window B: second headset connect
dump_window(secs(13, 51, 33, 0), secs(13, 51, 43, 0), "B: 13:51:33-43 (second headset connect)")
