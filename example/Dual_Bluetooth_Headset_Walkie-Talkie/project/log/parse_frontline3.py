# -*- coding: utf-8 -*-
"""Raw byte search for corrupt-frame RTP ts/seq in .cfa and pcaps; loose RTP scan without filters."""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')


def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkts.append(data[off:off + incl])
        off += incl
    return pkts


with open(CFA, 'rb') as f:
    cfa = f.read()

print("== raw search in .cfa ==")
for label, pat in [
    ('ts 5992960 BE', struct.pack('>I', 5992960)),
    ('ts 3708160 BE', struct.pack('>I', 3708160)),
    ('rtp seq94+ts', struct.pack('>BH', 0x60, 94) + struct.pack('>I', 5992960)),
    ('rtp seq288+ts', struct.pack('>BH', 0x60, 288) + struct.pack('>I', 3708160)),
    ('CID 74 5f', b'\x74\x5f'),
]:
    hits = []
    i = 0
    while len(hits) < 8:
        j = cfa.find(pat, i)
        if j < 0:
            break
        hits.append(j)
        i = j + 1
    print(f"  {label}: {len(hits)} hits {['0x%X' % h for h in hits]}")
    for h in hits[:3]:
        print(f"     ctx: {cfa[max(0,h-16):h+24].hex(' ')}")

print("\n== loose RTP scan in .cfa (any M-bit, any ssrc, print non-0x9C sync) ==")
n = 0
bad = []
for m in range(2):
    b0 = 0x80  # V=2
for j in range(len(cfa) - 14):
    if cfa[j] in (0x80, 0x81, 0x90, 0x98) and (cfa[j + 1] & 0x7F) == 0x60:
        ts = struct.unpack_from('>I', cfa, j + 4)[0]
        ssrc = struct.unpack_from('>I', cfa, j + 8)[0]
        fcnt = cfa[j + 12]
        sync = cfa[j + 13]
        # RTP from our device: ts multiple of 1280, ssrc 0 or 0x01000000, fcnt 1..15
        if (ts % 1280) == 0 and ssrc in (0, 0x01000000) and 1 <= fcnt <= 15:
            n += 1
            if sync != 0x9C:
                bad.append((j, ts, ssrc, fcnt, sync))
print(f"  loose RTP-like={n} bad_sync={len(bad)}")
for (j, ts, ssrc, fcnt, sync) in bad[:20]:
    seq = struct.unpack_from('>H', cfa, j + 2)[0]
    print(f"  BAD off=0x{j:X} seq={seq} ts={ts} ssrc=0x{ssrc:08X} fcnt={fcnt} sync=0x{sync:02X}")
    print(f"     ctx: {cfa[max(0,j-16):j+40].hex(' ')}")

print("\n== loose RTP scan in all pcaps ==")
for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
    pkts = parse_pcap(path)
    n = 0
    bad = []
    for pi, pkt in enumerate(pkts):
        for j in range(len(pkt) - 14):
            if pkt[j] in (0x80, 0x81, 0x90) and (pkt[j + 1] & 0x7F) == 0x60:
                ts = struct.unpack_from('>I', pkt, j + 4)[0]
                ssrc = struct.unpack_from('>I', pkt, j + 8)[0]
                fcnt = pkt[j + 12]
                sync = pkt[j + 13]
                if (ts % 1280) == 0 and ssrc in (0, 0x01000000) and 1 <= fcnt <= 15:
                    n += 1
                    if sync != 0x9C:
                        bad.append((pi, j, ts, ssrc, fcnt, sync))
                    break
    nm = os.path.basename(path).encode('ascii', 'replace').decode()
    print(f"  {nm}: rtp={n} bad={len(bad)}")
    for b in bad[:6]:
        pi, j, ts, ssrc, fcnt, sync = b
        seq = struct.unpack_from('>H', pkts[pi], j + 2)[0]
        print(f"     BAD pkt#{pi} off=0x{j:X} seq={seq} ts={ts} sync=0x{sync:02X}")
        print(f"        {pkts[pi][max(0,j-16):j+40].hex(' ')}")
