# -*- coding: utf-8 -*-
"""Round 3: framing sanity, global ACL credit timeline, stream-2 RTP headers."""
import struct
from pathlib import Path
from collections import Counter

BASE = Path(r"F:\SiFli\SDK\SiFli-SDK\main\example\Dual_Bluetooth_Headset_Walkie-Talkie\project\log\卡顿log")
STEM = "窗口2_COM47_(2026-10-20-13-50-03)"

pcap = (BASE / (STEM + ".pcap")).read_bytes()
bintime = (BASE / (STEM + ".bintime")).read_bytes()

print("first 64 bytes of pcap:", pcap[:64].hex())

times = []
for off in range(0, len(bintime) - 15, 16):
    y, mo, q, d, h, mi, s, ms = struct.unpack_from("<8H", bintime, off)
    times.append(h * 3600 + mi * 60 + s + ms / 1000.0)

frames = []
off = 0
skips = []
while off + 6 <= len(pcap):
    if pcap[off:off + 2] != b"\x06\x01":
        nxt = pcap.find(b"\x06\x01", off + 1)
        if nxt < 0:
            break
        skips.append((off, nxt - off))
        off = nxt
        continue
    ln, fseq = struct.unpack_from("<HH", pcap, off + 2)
    if off + 6 + ln > len(pcap):
        break
    frames.append((off, fseq, pcap[off + 6: off + 6 + ln]))
    off += 6 + ln

print(f"frames={len(frames)} skip_events={len(skips)} total_skipped_bytes={sum(n for _, n in skips)}")
print("first 5 skips (offset, bytes):", skips[:5])

def ts(i):
    t = times[i]
    h = int(t // 3600); m = int(t % 3600 // 60); s = int(t % 60); ms = int(round((t % 1) * 1000))
    return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"

# ---- global credit timeline: outstanding = sent - ncp_completed (controller advertises 4 ACL buffers)
sent_events = []
for i, (off, fseq, p) in enumerate(frames):
    t = times[i] if i < len(times) else 0
    if p[:2] in (b"\x80\x20", b"\x81\x20"):
        sent_events.append((t, 1, "TX", p[0]))
    elif p[:1] == b"\x13":
        j = p.find(b"\x13\x05")
        if j >= 0 and j + 7 <= len(p):
            nh = p[j + 2]
            q = j + 3
            tot = 0
            for _ in range(nh):
                if q + 4 > len(p):
                    break
                hdl, cnt = struct.unpack_from("<HH", p, q)
                tot += cnt
                q += 4
            sent_events.append((t, -tot, "NCP", 0))

sent_events.sort(key=lambda e: e[0])
outst = 0
min_o = 99
max_o = 0
neg_pts = []
running = []
for t, d, k, h in sent_events:
    outst += d
    if k == "TX":
        if outst > max_o:
            max_o = outst
        if outst > 4:
            neg_pts.append((t, outst))
    running.append((t, outst))
print(f"\nglobal outstanding: max={max_o}  (controller advertises 4)")
print(f"moments with outstanding > 4: {len(neg_pts)}")
for t, o in neg_pts[:15]:
    print(f"   {ts_of := (lambda t: f'{int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:06.3f}')(t)} outstanding={o}")

# outstanding profile around each glitch window
def prof(t0, t1, label):
    print(f"\n--- outstanding profile {label} ---")
    for t, o in running:
        if t0 <= t <= t1:
            print(f"   {ts(int(0)) if False else f'{int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:06.3f}'} out={o}")

h = lambda hh, mm, ss, ms=0: hh * 3600 + mm * 60 + ss + ms / 1000.0
# compact: max outstanding within each second of the connect window
print("\n--- max outstanding per 500ms bucket, 13:51:34-38 ---")
bucket = {}
for t, o in running:
    if h(13, 51, 34) <= t <= h(13, 51, 38):
        b = int((t - h(13, 51, 34)) * 2)
        bucket[b] = max(bucket.get(b, 0), o)
for b in sorted(bucket):
    t = h(13, 51, 34) + b * 0.5
    print(f"   {int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:05.2f} max_out={bucket[b]}")

print("\n--- max outstanding per 500ms bucket, 13:51:18-20 (360ms gap) ---")
bucket = {}
for t, o in running:
    if h(13, 51, 18) <= t <= h(13, 51, 20):
        b = int((t - h(13, 51, 18)) * 2)
        bucket[b] = max(bucket.get(b, 0), o)
for b in sorted(bucket):
    t = h(13, 51, 18) + b * 0.5
    print(f"   {int(t//3600):02d}:{int(t%3600//60):02d}:{t%60:05.2f} max_out={bucket[b]}")

# ---- stream-2 media RTP headers (CID 0x0064 on hdl 0x81)
print("\n--- hdl 0x81 media (CID 0x64) RTP headers ---")
n = 0
for i, (off, fseq, p) in enumerate(frames):
    if p[:2] == b"\x81\x20":
        j = p.find(b"\x64\x00\x80\x60")
        if j >= 0 and j + 12 <= len(p):
            seq = struct.unpack_from(">H", p, j + 4)[0]
            tsv = struct.unpack_from(">I", p, j + 6)[0]
            if n < 12 or seq in (0x00ff, 0x0010, 0x00a2):
                print(f"{ts(i)} seq={seq:5d} ts={tsv}")
            n += 1
print("total hdl-0x81 media:", n)

# ---- hdl 0x80 media around the 556 skip, full hex
print("\n--- hdl 0x80 media rtp 554-558 full first 20 body bytes ---")
for i, (off, fseq, p) in enumerate(frames):
    j = p.find(b"\x41\x00\x80\x60")
    if j >= 0 and j + 10 <= len(p):
        seq = struct.unpack_from(">H", p, j + 4)[0]
        if 553 <= seq <= 559:
            tsv = struct.unpack_from(">I", p, j + 6)[0]
            print(f"{ts(i)} seq={seq} ts={tsv} acl_len={struct.unpack_from('<H', p, 2)[0]} l2len={struct.unpack_from('<H', p, 4)[0] if len(p)>=8 else -1} body={p[j:j+24].hex()}")
