# -*- coding: utf-8 -*-
"""Identify source of stale tail bytes in corrupt fan-out copies."""
import struct, os

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

FRAME_LEN = 63

# corrupt copy seq94 ts5992960 at off 0x65400F (rtp), clean copy at 0x653B98-10+32 adj
# find both RTPs for ts 5992960
ts = 5992960
pat = struct.pack('>I', ts)
idxs = []
i = 0
while True:
    j = cfa.find(pat, i)
    if j < 0:
        break
    idxs.append(j)
    i = j + 1
print("ts 5992960 occurrences:", ['0x%X' % x for x in idxs])

# RTP starts 4 bytes before ts (byte0=0x80,1=0x60,2-3=seq,4-7=ts)
rtps = [j - 4 for j in idxs if j >= 4 and cfa[j - 4] == 0x80]
print("RTP offs:", ['0x%X' % x for x in rtps])

a = cfa[rtps[0]: rtps[0] + 643]  # first (h80 clean)
b = cfa[rtps[1]: rtps[1] + 643]  # second (h81 corrupt)
print(f"clean  seq={struct.unpack_from('>H', a, 2)[0]} ssrc=0x{struct.unpack_from('>I', a, 8)[0]:08X}")
print(f"corrupt seq={struct.unpack_from('>H', b, 2)[0]} ssrc=0x{struct.unpack_from('>I', b, 8)[0]:08X}")

# where do they first differ?
diff = None
for k in range(643):
    if a[k] != b[k]:
        diff = k
        break
print(f"first differing byte offset in L2CAP payload: {diff}")
print(f"  clean  [{diff}:{diff+32}]: {a[diff:diff+32].hex(' ')}")
print(f"  corrupt[{diff}:{diff+32}]: {b[diff:diff+32].hex(' ')}")

tail = b[diff:643]
print(f"tail len={len(tail)}")

# search the tail (first 32 bytes) in the whole .cfa to find its origin
probe = tail[:32]
i = 0
hits = []
while True:
    j = cfa.find(probe, i)
    if j < 0:
        break
    hits.append(j)
    i = j + 1
print(f"\ntail probe (32B) found at {len(hits)} locations: {['0x%X' % h for h in hits[:12]]}")

# for each hit, identify context: is it inside a media packet? compute relative offset
for h in hits[:12]:
    # search backwards for nearest RTP
    ctx = cfa[max(0, h - 700): h]
    rpos = ctx.rfind(b'\x80\x60')
    if rpos >= 0:
        rtp = max(0, h - 700) + rpos
        seq = struct.unpack_from('>H', cfa, rtp + 2)[0]
        rts = struct.unpack_from('>I', cfa, rtp + 4)[0]
        ssrc = struct.unpack_from('>I', cfa, rtp + 8)[0]
        rel = h - rtp
        print(f"  hit 0x{h:X}: in media pkt seq={seq} ts={rts} ssrc=0x{ssrc:08X} at payload+{rel}")
    else:
        print(f"  hit 0x{h:X}: not near an RTP header; ctx={cfa[h-16:h+16].hex(' ')}")

# Is the corrupt tail equal to the CLEAN copy's tail region shifted? (memcpy wrong src)
print("\n== does corrupt tail match clean copy at some shift? ==")
for shift in range(0, 63):
    if a[diff + shift: diff + shift + 32] == probe:
        print(f"  match at clean offset {diff + shift} (shift +{shift})")
        break
else:
    print("  no shift match within same packet")

# Does corrupt tail match the PREVIOUS packet's (seq 93) content?  find seq 93 same stream
seq93_pat = struct.pack('>BH', 0x60, 93) + struct.pack('>I', ts - 1280)
j = cfa.find(seq93_pat)
print(f"\nprev packet seq93 ts{ts-1280}: {'0x%X' % j if j >= 0 else 'not found'}")
if j >= 0:
    prev = cfa[j - 4: j - 4 + 643]
    # search probe in prev
    k = prev.find(probe)
    print(f"  probe in prev packet at offset: {k}")
