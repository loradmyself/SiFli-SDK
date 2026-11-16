# -*- coding: utf-8 -*-
"""Discriminate real device-side drop vs capture-side loss for the 4 gaps.

Each encoded media packet is fanned out to both headsets (same RTP ts, two
ssrc/seq). If the missing copy's twin (same ts, other ssrc) IS present in the
.cfa, the device emitted only one dump -> real per-copy send failure.
If BOTH copies are missing at that ts -> shared cause (capture gap or the
encoder/sender skipped the packet entirely).
Also dump full record structure of each gap region.
"""
import struct, os

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

# inventory all media packets (file order): (off, seq, ts, ssrc)
pkts = []
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
            pkts.append((j, seq, ts, ssrc))
    i = j + 2

by_ts = {}
for p in pkts:
    by_ts.setdefault(p[2], []).append(p)

# the 4 gaps: (ssrc, missing_seq, ts_before, ts_after)
gaps = [
    (0x00000000, 1979, 2533120, 2535680),
    (0x00000000, 2492, 3189760, 3193600),
    (0x00000000, 2493, 3189760, 3193600),
    (0x01000000, 95, 4304640, 4307200),
    (0x00000000, 3738, 4784640, 4787200),
]

print("== expected missing timestamps (interp between neighbours) and twin check ==")
for ssrc, mseq, tsa, tsb in gaps:
    # RTP ts steps by 1280 per packet; the missing packet(s) sit between
    exp_ts = tsa + 1280
    while exp_ts < tsb:
        present = by_ts.get(exp_ts, [])
        twin = [p for p in present if p[3] != ssrc]
        mine = [p for p in present if p[3] == ssrc]
        print(f"gap ssrc=0x{ssrc:08X} seq {mseq}: missing ts {exp_ts}: "
              f"copies present = {len(present)} "
              f"(twin ssrc {'0x%08X' % twin[0][3] if twin else '-'})")
        if twin:
            print(f"  -> TWIN PRESENT at off 0x{twin[0][0]:X} seq {twin[0][1]}: "
                  f"device emitted twin copy; this copy's dump absent -> per-copy drop")
        exp_ts += 1280

print("\n== record-structure walk of each gap region ==")
MARK = b'\x45\x4d'
for ssrc, mseq, tsa, tsb in gaps:
    # region bounds from file-order walk (approx: between neighbour records)
    # find the RTP of tsa and tsb on this ssrc
    a = next((p for p in pkts if p[2] == tsa and p[3] == ssrc), None)
    b = next((p for p in pkts if p[2] == tsb and p[3] == ssrc), None)
    if not a or not b:
        print(f"seq {mseq}: anchors not found")
        continue
    lo, hi = a[0] + 643, b[0]
    region = cfa[lo:hi]
    print(f"\nseq {mseq} region 0x{lo:X}..0x{hi:X} ({hi-lo}B):")
    # split by 45 4d / 44 4d record markers and show first bytes of each record
    marks = []
    k = 0
    while k < len(region) - 2:
        if region[k:k+2] in (b'\x45\x4d', b'\x44\x4d') and k + 8 < len(region):
            marks.append(k)
            k += 2
        else:
            k += 1
    for idx, m in enumerate(marks):
        end = marks[idx + 1] if idx + 1 < len(marks) else len(region)
        rec = region[m:end]
        print(f"  rec@+{m} len={len(rec)} head={rec[:24].hex(' ')}")
