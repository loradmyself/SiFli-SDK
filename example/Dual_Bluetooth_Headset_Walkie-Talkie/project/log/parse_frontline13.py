# -*- coding: utf-8 -*-
"""Quantify gap holes in raw .bin and check .bintime for capture discontinuities.
If expected bytes (missing dump sizes) exceed what's between the neighbours,
the .bin has a hole -> PC-side capture loss. Also parse .bintime records."""
import struct, os, glob, re

LOGDIR = os.path.dirname(os.path.abspath(__file__))

gaps = [
    ('gap1', 2534400, 2533120, 2535680, 2),   # (miss, before, after, n_expected_copies)
    ('gap2', 3191040, 3189760, 3192320, 2),
    ('gap2b', 3192320, 3191040, 3193600, 2),
    ('gap4', 4785920, 4784640, 4787200, 2),
]

def find_ts(data, ts):
    pat = struct.pack('>I', ts)
    out, i = [], 0
    while True:
        j = data.find(pat, i)
        if j < 0:
            return out
        out.append(j)
        i = j + 1

for path in sorted(glob.glob(os.path.join(LOGDIR, '*.bin'))):
    with open(path, 'rb') as f:
        data = f.read()
    name = os.path.basename(path)
    for label, miss, tb, ta, ncop in gaps:
        fb, fa = find_ts(data, tb), find_ts(data, ta)
        if not (fb and fa):
            continue
        lo, hi = fb[-1], fa[0]
        span = hi - lo
        # ts sits 4 bytes into RTP; before-packet remaining payload ~= 643-8
        # after-packet headers before ts ~= H4(1)+ACL(4)+L2CAP(4)+RTP(4) ~= 13+4
        expected_min = (643 - 8) + ncop * 652 + 17
        hole = expected_min - span
        region = data[lo:hi]
        ntext = len(re.findall(rb'[\x20-\x7e]{6,}', region))
        print(f"{name} {label}: span={span}B expected>={expected_min}B "
              f"-> hole~{hole}B  text_runs={ntext}")

print("\n== .bintime structure (first file) ==")
bt = sorted(glob.glob(os.path.join(LOGDIR, '*.bintime')))[0]
with open(bt, 'rb') as f:
    b = f.read()
print(f"{os.path.basename(bt)}: {len(b)} bytes")
print("head:", b[:64].hex(' '))
print("head ascii:", re.sub(rb'[^\x20-\x7e]', b'.', b[:64]))
# guess record size
for rec in (8, 12, 16, 20, 24, 32):
    if len(b) % rec == 0:
        vals = [struct.unpack_from('<I', b, i)[0] for i in range(0, min(256, len(b)), rec)]
        print(f"  rec={rec}: first u32s: {['%d' % v for v in vals[:8]]}")
