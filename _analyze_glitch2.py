# -*- coding: utf-8 -*-
"""Round 2: credit accounting, seq-556 skip forensics, window B detail."""
import struct
from pathlib import Path
from collections import Counter, defaultdict

BASE = Path(r"F:\SiFli\SDK\SiFli-SDK\main\example\Dual_Bluetooth_Headset_Walkie-Talkie\project\log\卡顿log")
STEM = "窗口2_COM47_(2026-10-20-13-50-03)"

pcap = (BASE / (STEM + ".pcap")).read_bytes()
bintime = (BASE / (STEM + ".bintime")).read_bytes()

times = []
for off in range(0, len(bintime) - 15, 16):
    y, mo, q, d, h, mi, s, ms = struct.unpack_from("<8H", bintime, off)
    times.append(h * 3600 + mi * 60 + s + ms / 1000.0)

frames = []
off = 0
while off + 6 <= len(pcap):
    if pcap[off:off + 2] != b"\x06\x01":
        nxt = pcap.find(b"\x06\x01", off + 1)
        if nxt < 0:
            break
        off = nxt
        continue
    ln, fseq = struct.unpack_from("<HH", pcap, off + 2)
    if off + 6 + ln > len(pcap):
        break
    frames.append(pcap[off + 6: off + 6 + ln])
    off += 6 + ln

def ts(i):
    t = times[i]
    h = int(t // 3600); m = int(t % 3600 // 60); s = int(t % 60); ms = int(round((t % 1) * 1000))
    return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"

def s_of(i):
    return times[i]

# ---- classify records
recs = []
for i, p in enumerate(frames):
    r = {"i": i, "raw": p, "t": s_of(i)}
    if p[:1] == b"\x13":
        # NCP-style record: 13 00 00 00 xx xx 13 05 01 hdl_lo hdl_hi cnt_lo cnt_hi ctr...
        # find inner event
        j = p.find(b"\x13\x05")
        if j >= 0 and j + 7 <= len(p):
            nh = p[j + 2]
            q = j + 3
            ncp = []
            for _ in range(nh):
                if q + 4 > len(p):
                    break
                hdl, cnt = struct.unpack_from("<HH", p, q)
                ncp.append((hdl & 0xFFF, cnt))
                q += 4
            r.update(kind="NCP", ncp=ncp)
        else:
            r["kind"] = "T13"
    elif p[:1] == b"\x0e":
        r["kind"] = "CMDSTAT"
        r["hex"] = p[:24].hex()
    elif p[:1] == b"\x0f":
        r["kind"] = "CMDSTAT0f"
        r["hex"] = p[:24].hex()
    elif p[:1] == b"\x04":
        r["kind"] = "RAW04"
        r["hex"] = p[:24].hex()
    elif p[:2] in (b"\x80\x20", b"\x81\x20", b"\x82\x20"):
        hdl = p[0] & 0x0F
        r["hdl"] = 0x80 + (hdl - 0)  # 0x80/0x81 from low nibble? p[0]=0x80/0x81
        r["hdl"] = p[0]  # 0x80 / 0x81
        r["kind"] = "ACL"
        # L2CAP at offset 4: len, cid
        if len(p) >= 8:
            l2len, cid = struct.unpack_from("<HH", p, 4)
            r["cid"] = cid
            r["l2len"] = l2len
        i2 = p.find(b"\x41\x00\x80\x60")
        if i2 >= 0 and i2 + 10 <= len(p):
            r.update(kind="MEDIA", rtp_seq=struct.unpack_from(">H", p, i2 + 4)[0],
                     rtp_ts=struct.unpack_from(">I", p, i2 + 6)[0], rtp_off=i2)
    else:
        r["kind"] = "OTH"
        r["hex"] = p[:24].hex()
    recs.append(r)

print("kind counts:", dict(Counter(r["kind"] for r in recs)))

# ---- hunt Read_Buffer_Size command complete (opcode 0x1005) in CMDSTAT records
print("\n===== HCI events (0e/0f records) first 20 =====")
n = 0
for r in recs:
    if r["kind"] in ("CMDSTAT", "CMDSTAT0f", "RAW04", "OTH"):
        print(f"{ts(r['i'])} {r['kind']:10s} {r['hex']}")
        n += 1
        if n >= 24:
            break

# search any record containing opcode 05 10 (Read_Buffer_Size) or 02 10 (Read_Local)
print("\n===== records containing '05 10' (Read_Buffer_Size cmd/complete) =====")
for r in recs:
    if b"\x05\x10" in r["raw"]:
        print(f"{ts(r['i'])} {r['kind']:6s} {r['raw'][:32].hex()}")

# ---- credit accounting
print("\n===== credit accounting (ACL out vs NCP back) =====")
balance = defaultdict(int)  # per handle, from NCP only
sent = defaultdict(int)
events = []
for r in recs:
    if r["kind"] == "NCP":
        for hdl, cnt in r["ncp"]:
            balance[hdl] += cnt
            events.append((r["t"], "NCP", hdl, cnt))
    elif r["kind"] in ("ACL", "MEDIA"):
        hdl = r["hdl"]
        sent[hdl] += 1
        events.append((r["t"], "TX", hdl, -1))
    elif r["kind"] in ("CMDSTAT", "CMDSTAT0f"):
        events.append((r["t"], "EVT", 0, 0))

print("sent per handle:", dict(sent))
print("ncp returned per handle:", dict(balance))
run = defaultdict(int)
minrun = defaultdict(lambda: 0)
neg_points = []
for t, k, hdl, d in events:
    if k == "NCP":
        run[hdl] += d
    elif k == "TX":
        run[hdl] -= 1
        if run[hdl] < minrun[hdl]:
            minrun[hdl] = run[hdl]
        if run[hdl] <= -1:
            neg_points.append((t, hdl, run[hdl]))
print("min running (ncp-sent) per handle:", dict(minrun))
if neg_points:
    print(f"points where ncp-sent <= -1: {len(neg_points)}")
    for t, hdl, v in neg_points[:12]:
        h = int(t // 3600); m = int(t % 3600 // 60); s = t % 60
        print(f"   {h:02d}:{m:02d}:{s:06.3f} hdl=0x{hdl:03x} balance={v}")

# ---- gap windows: what's inside
def dump(t0, t1, label):
    print(f"\n===== {label} =====")
    prev = None
    for r in recs:
        if r["t"] < t0 or r["t"] > t1:
            continue
        gap = f"+{(r['t']-prev)*1000:6.1f}ms" if prev else " " * 8
        prev = r["t"]
        if r["kind"] == "MEDIA":
            print(f"{ts(r['i'])}{gap} MEDIA hdl=0x{r['hdl']:02x} rtp={r['rtp_seq']:4d} ts={r['rtp_ts']}")
        elif r["kind"] == "ACL":
            cid = r.get("cid", -1)
            names = {1: "SIG", 6: "SDP"}
            nm = names.get(cid, f"cid{cid:04x}")
            print(f"{ts(r['i'])}{gap} ACL   hdl=0x{r['hdl']:02x} {nm:8s} len={len(r['raw'])} {r['raw'][8:20].hex()}")
        elif r["kind"] == "NCP":
            print(f"{ts(r['i'])}{gap} NCP   {r['ncp']}")
        else:
            print(f"{ts(r['i'])}{gap} {r['kind']:10s} {r.get('hex','')[:40]}")

h = lambda hh, mm, ss, ms=0: hh * 3600 + mm * 60 + ss + ms / 1000.0

dump(h(13, 51, 27, 200), h(13, 51, 27, 700), "gap 13:51:27.2-27.7 (97ms gap rtp=459)")
dump(h(13, 51, 30, 100), h(13, 51, 30, 300), "skip 13:51:30.1-30.3 (missing seq 556)")
dump(h(13, 51, 34, 300), h(13, 51, 36, 0), "B1 13:51:34.3-36.0 (connect cmd + page)")
dump(h(13, 51, 36, 0), h(13, 51, 38, 0), "B2 13:51:36.0-38.0 (AVDTP setup)")
dump(h(13, 51, 38, 0), h(13, 51, 42, 0), "B3 13:51:38.0-42.0 (HFP/SSP tail)")

# ---- SBC payload forensics for seq 549-560
print("\n===== SBC payload check rtp 549-560 =====")
for r in recs:
    if r["kind"] == "MEDIA" and 549 <= r["rtp_seq"] <= 560:
        p = r["raw"]
        o = r["rtp_off"]
        # RTP hdr 12 bytes after 41 00 80 60 (which covers cid+2 rtp bytes): payload starts o+2+12
        sbc_start = o + 2 + 12
        body = p[sbc_start:]
        # A2DP SBC: 1-byte payload header then frames
        hdr = body[0] if body else -1
        nframes = hdr & 0x0F
        # find sync words 0x9c
        pos = 1
        syncs = []
        for k in range(20):
            idx = body.find(b"\x9c", pos)
            if idx < 0:
                break
            syncs.append(idx)
            pos = idx + 1
        print(f"rtp={r['rtp_seq']} {ts(r['i'])} sbc_hdr=0x{hdr:02x} nframes={nframes} first32={body[:32].hex()} sync_at={syncs[:12]}")
