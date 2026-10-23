# -*- coding: utf-8 -*-
"""Session match + per-twin presence: is the .cfa missing a record that the
COM24 pcaps contain (tool discrepancy) or is it absent everywhere (real)?"""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

def rtps(data):
    out = []
    i = 0
    while True:
        j = data.find(b'\x80\x60', i)
        if j < 0:
            break
        if j + 12 <= len(data):
            seq = struct.unpack_from('>H', data, j + 2)[0]
            ts = struct.unpack_from('>I', data, j + 4)[0]
            ssrc = struct.unpack_from('>I', data, j + 8)[0]
            out.append((j, seq, ts, ssrc))
        i = j + 2
    return out

print("== all ssrc=0x01000000 seq=95 in .cfa (ts list) ==")
for (o, seq, ts, ssrc) in rtps(cfa):
    if ssrc == 0x01000000 and seq == 95:
        print(f"  off 0x{o:X} ts={ts}")

print("\n== .cfa: any RTP with ts=4305920 ==")
for (o, seq, ts, ssrc) in rtps(cfa):
    if ts == 4305920:
        print(f"  off 0x{o:X} seq={seq} ssrc=0x{ssrc:08X}")

print("\n== pcaps: any RTP with ts=4305920 ==")
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

for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
    name = os.path.basename(path)
    n = 0
    for pi, pkt in enumerate(parse_pcap(path)):
        for (o, seq, ts, ssrc) in rtps(pkt):
            if ts == 4305920:
                n += 1
                print(f"  {name} pkt#{pi}: seq={seq} ssrc=0x{ssrc:08X} off={o}")
    if not n:
        print(f"  {name}: none")

print("\n== .cfa: seq3363 ssrc0 (the twin of gap3) ==")
for (o, seq, ts, ssrc) in rtps(cfa):
    if ssrc == 0 and seq == 3363:
        print(f"  off 0x{o:X} ts={ts}")
