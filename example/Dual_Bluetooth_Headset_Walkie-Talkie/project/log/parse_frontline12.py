# -*- coding: utf-8 -*-
"""Locate gap neighbours in the RAW .bin serial stream and dump the bytes
between them. Clean adjacency (only other records) => the dump was never on
the UART => real device-side drop. A byte hole / bintime gap => PC-side loss."""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))

# (missing_ts, ts_before, ts_after, ssrc) for the 4 .bin-absent slots
gaps = [
    (2534400, 2533120, 2535680, 0),
    (3191040, 3189760, 3192320, 0),   # after is also missing; chain
    (3192320, 3191040, 3193600, 0),
    (4785920, 4784640, 4787200, 0),
]

def find_ts(data, ts):
    pat = struct.pack('>I', ts)
    out = []
    i = 0
    while True:
        j = data.find(pat, i)
        if j < 0:
            break
        out.append(j)
        i = j + 1
    return out

for path in sorted(glob.glob(os.path.join(LOGDIR, '*.bin'))):
    with open(path, 'rb') as f:
        data = f.read()
    name = os.path.basename(path)
    for miss, tb, ta, ssrc in gaps:
        fb = find_ts(data, tb)
        fa = find_ts(data, ta)
        if not fb and not fa:
            continue
        print(f"\n{name}: gap miss ts={miss}")
        print(f"  before ts={tb} at {['0x%X' % x for x in fb]}")
        print(f"  after  ts={ta} at {['0x%X' % x for x in fa]}")
        if fb and fa:
            lo, hi = fb[-1] + 4, fa[0] - 4
            region = data[lo:hi]
            print(f"  between: {hi-lo} bytes")
            print(f"  head: {region[:64].hex(' ')}")
            print(f"  tail: {region[-64:].hex(' ')}")
            # text runs?
            import re
            for m in re.finditer(rb'[\x20-\x7e\t]{8,}', region):
                print(f"  TEXT: {m.group()[:60]!r}")
