# -*- coding: utf-8 -*-
"""Locate known-corrupt frames' RTP (seq,ts) in .cfa and pcaps; dump bytes; also full ESMEvent."""
import struct, os, re, glob, datetime

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
BES = os.path.join(LOGDIR, '电流和卡顿.bes')

print("========== ALL ESMEvent lines ==========")
with open(BES, 'rb') as f:
    text = f.read().decode('gbk', 'replace')
in_event = False
for ln in text.splitlines():
    if ln.startswith('[ESMEvent]'):
        in_event = True
        continue
    if in_event and ln.startswith('['):
        break
    if in_event and not ln.startswith('{'):
        print(ln)

# known corrupt frames from UI Errors tab: 7920 9538 11018 12759 14207 15089 20979
# known RTP ids: (20979: seq 94 ts 5992960), (15089: seq 288 ts 3708160)
targets = [
    ('20979', 94, 5992960),
    ('15089', 288, 3708160),
]

with open(CFA, 'rb') as f:
    cfa = f.read()

def dump(label, blob, off, pre=24, post=64):
    seg = blob[off - pre: off + post]
    print(f"  {label} off=0x{off:X}: {seg.hex(' ')}")

print("\n========== search .cfa for target RTP ==========")
for name, seq, ts in targets:
    pat = struct.pack('>BHH', 0x80, 0x60, seq) + struct.pack('>I', ts)
    j = cfa.find(pat)
    print(f"frame {name}: seq={seq} ts={ts} -> {'FOUND @0x%X' % j if j >= 0 else 'NOT FOUND'}")
    if j >= 0:
        dump('cfa', cfa, j)
        # what is at sync pos
        print(f"     fcnt={cfa[j+12]} sync={cfa[j+13]:02X} ssrc={struct.unpack_from('>I', cfa, j+8)[0]:08X}")

print("\n========== search .cfa for ANY RTP with bad sync ==========")
i = 0
n = 0
while True:
    j = cfa.find(b'\x80\x60', i)
    if j < 0 or j + 14 > len(cfa):
        break
    seq = struct.unpack_from('>H', cfa, j + 2)[0]
    ts = struct.unpack_from('>I', cfa, j + 4)[0]
    ssrc = struct.unpack_from('>I', cfa, j + 8)[0]
    fcnt = cfa[j + 12]
    sync = cfa[j + 13]
    if (ts % 1280) == 0 and ssrc in (0, 0x01000000):
        n += 1
        if sync != 0x9C:
            print(f"  BAD off=0x{j:X} seq={seq} ts={ts} ssrc=0x{ssrc:08X} fcnt={fcnt} sync=0x{sync:02X}")
            dump('cfa', cfa, j)
    i = j + 2
print(f"  total RTP-like={n}")

print("\n========== search pcaps for target RTP ==========")
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
    pkts = parse_pcap(path)
    for pi, pkt in enumerate(pkts):
        for name, seq, ts in targets:
            pat = struct.pack('>BHH', 0x80, 0x60, seq) + struct.pack('>I', ts)
            j = pkt.find(pat)
            if j >= 0:
                print(f"  {os.path.basename(path).encode('ascii','replace').decode()} pkt#{pi} frame{name}:")
                dump('pcap', pkt, j)
                print(f"     fcnt={pkt[j+12]} sync={pkt[j+13]:02X} ssrc={struct.unpack_from('>I', pkt, j+8)[0]:08X}")
