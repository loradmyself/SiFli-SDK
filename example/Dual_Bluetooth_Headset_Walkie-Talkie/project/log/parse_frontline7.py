# -*- coding: utf-8 -*-
"""Loss re-evaluation under the console-interleave artifact hypothesis (v2).

Walk media packets in FILE order per (ssrc, epoch). On a small forward seq
jump, dump the raw bytes between the two packets: if they contain log-text
runs / framing garbage instead of a clean record, the 'loss' is a reassembly
break artifact, not a real drop.
"""
import struct, os, re

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

print(f"cfa size = {len(cfa)}")

pkts = []  # (off, seq, ts, ssrc, fcnt) in FILE order
i = 0
while True:
    j = cfa.find(b'\x80\x60', i)
    if j < 0:
        break
    if j + 643 <= len(cfa):
        seq = struct.unpack_from('>H', cfa, j + 2)[0]
        ts = struct.unpack_from('>I', cfa, j + 4)[0]
        ssrc = struct.unpack_from('>I', cfa, j + 8)[0]
        fcnt = cfa[j + 12]
        if (ts % 1280) == 0 and ssrc in (0, 0x01000000) and 1 <= fcnt <= 15:
            pkts.append((j, seq, ts, ssrc, fcnt))
    i = j + 2

print(f"media packets (file order): {len(pkts)}")

# log-text signature: runs with letters + space/= /: typical of log lines
logish = re.compile(rb'[A-Za-z_][A-Za-z0-9_ ,.:=/()\-\r\n]{10,}')


def scan_text(region):
    out = []
    for m in logish.finditer(region):
        s = m.group()
        if b'=' in s or b'_' in s or b' ' in s or b':' in s:
            out.append((m.start(), s))
    return out


per = {}
n_gap = 0
for (off, seq, ts, ssrc, fcnt) in pkts:
    key = ssrc
    prev = per.get(key)
    per[key] = (off, seq, ts)
    if prev is None:
        continue
    poff, pseq, pts = prev
    d = (seq - pseq) & 0xFFFF
    if d == 1:
        continue
    if d > 100 or d == 0:
        # stream restart (new epoch) or dup — not a loss
        continue
    n_gap += 1
    lo, hi = poff + 643, off
    region = cfa[lo:hi]
    print(f"\nGAP[{n_gap}] ssrc=0x{ssrc:08X} seq {pseq+1}..{seq-1} ({d-1} missing) "
          f"ts {pts}->{ts} file 0x{lo:X}..0x{hi:X} ({hi-lo}B)")
    hits = scan_text(region)
    if hits:
        for o, s in hits[:8]:
            print(f"  TEXT @+{o}: {s[:70]!r}")
    else:
        print(f"  head: {region[:40].hex(' ')}")
        print(f"  tail: {region[-40:].hex(' ')}")
        if not region:
            print("  EMPTY -> packets adjacent, genuinely absent")

print(f"\ntotal small forward gaps: {n_gap}")
