# -*- coding: utf-8 -*-
"""Are the 'missing' packets really absent from the whole .cfa (drop) or do
they appear out-of-order later (delay)? Also check the pcaps."""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

# all RTP occurrences with (seq, ts, ssrc) regardless of validity filters
def find_all(data, label):
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

cfa_hits = find_all(cfa, 'cfa')

targets = [
    (0x00000000, 1979),
    (0x00000000, 2492),
    (0x00000000, 2493),
    (0x01000000, 95),
    (0x00000000, 3738),
]

print("== .cfa search for the 5 'missing' (ssrc,seq) anywhere in file ==")
for ssrc, seq in targets:
    hits = [(o, ts) for (o, s, ts, ss) in cfa_hits if s == seq and ss == ssrc]
    print(f"ssrc=0x{ssrc:08X} seq={seq}: {len(hits)} occurrence(s)")
    for o, ts in hits[:5]:
        print(f"   off 0x{o:X} ts={ts}")

print("\n== same search in all pcaps ==")
def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkts.append((ts_sec, ts_usec, data[off:off + incl]))
        off += incl
    return pkts

for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
    pkts = parse_pcap(path)
    name = os.path.basename(path)
    for ssrc, seq in targets:
        found = 0
        for pi, (s, u, pkt) in enumerate(pkts):
            for (o, pseq, pts, pssrc) in find_all(pkt, name):
                if pseq == seq and pssrc == ssrc:
                    found += 1
                    if found <= 2:
                        print(f"  {name}: ssrc=0x{ssrc:08X} seq={seq} in pkt#{pi} off {o} ts={pts}")
        if found:
            print(f"  {name}: ssrc=0x{ssrc:08X} seq={seq}: {found} hit(s)")
        else:
            print(f"  {name}: ssrc=0x{ssrc:08X} seq={seq}: absent")
