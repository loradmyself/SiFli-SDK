# -*- coding: utf-8 -*-
"""Compare fan-out copies payload-only (skip RTP seq/ssrc), find real divergence,
then fingerprint the garbage tail (entropy, pattern) across all 8 bad packets."""
import struct, os

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

FRAME_LEN = 63


def cmp_copies(ts):
    pat = struct.pack('>I', ts)
    idxs = []
    i = 0
    while True:
        j = cfa.find(pat, i)
        if j < 0:
            break
        if j >= 4 and cfa[j - 4] == 0x80:
            idxs.append(j - 4)
        i = j + 1
    if len(idxs) < 2:
        print(f"ts {ts}: found {len(idxs)} copies")
        return
    a = cfa[idxs[0]: idxs[0] + 643]
    b = cfa[idxs[1]: idxs[1] + 643]
    sa = struct.unpack_from('>I', a, 8)[0]
    sb = struct.unpack_from('>I', b, 8)[0]
    # payload compare from byte 12 (fcnt) onward
    diff = None
    for k in range(12, 643):
        if a[k] != b[k]:
            diff = k
            break
    print(f"\nts {ts}: copyA ssrc=0x{sa:08X} seq={struct.unpack_from('>H',a,2)[0]}  "
          f"copyB ssrc=0x{sb:08X} seq={struct.unpack_from('>H',b,2)[0]}")
    if diff is None:
        print("  payloads IDENTICAL from byte 12")
    else:
        print(f"  payload first differs at offset {diff}")
        print(f"    A[{diff}:{diff+48}]: {a[diff:diff+48].hex(' ')}")
        print(f"    B[{diff}:{diff+48}]: {b[diff:diff+48].hex(' ')}")
        # classify garbage region of the bad copy
        for name, x in (('A', a), ('B', b)):
            if x[13] != 0x9C:
                bad = x
                badname = name
        # which one is bad at first SBC frame?
        aok = all(a[13 + k * FRAME_LEN] == 0x9C for k in range(10) if 13 + k * FRAME_LEN < 643)
        bok = all(b[13 + k * FRAME_LEN] == 0x9C for k in range(10) if 13 + k * FRAME_LEN < 643)
        print(f"  all-syncs: A {'OK' if aok else 'BAD'}  B {'OK' if bok else 'BAD'}")
        bad = a if not aok else b
        badname = 'A' if not aok else 'B'
        # find bad start
        for k in range(10):
            pos = 13 + k * FRAME_LEN
            if bad[pos] != 0x9C:
                print(f"  bad copy {badname}: garbage starts at SBC frame {k} (off {pos}), "
                      f"runs to end ({643 - pos} bytes)")
                print(f"    garbage[:64]: {bad[pos:pos+64].hex(' ')}")
                # how many 0x9C appear in garbage? (stale SBC frames?)
                g = bad[pos:643]
                n9c = sum(1 for idx in range(0, len(g), FRAME_LEN) if idx < len(g) and g[idx] == 0x9C)
                print(f"    frame-aligned 0x9C in garbage: {n9c}/{(len(g)+FRAME_LEN-1)//FRAME_LEN}")
                # is garbage a repetition of something? check for period
                for per in (63, 126, 12, 16, 32, 64, 256):
                    if len(g) > 2 * per and g[:per] == g[per:2 * per]:
                        print(f"    ** garbage REPEATS with period {per}")
                        break
                break
    return idxs


# all 8 known bad timestamps from the audit
for ts in (5992960, 3708160, 925440, 1509120, 2068480, 2781440, 3452160, 14726400):
    cmp_copies(ts)
