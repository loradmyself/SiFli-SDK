# -*- coding: utf-8 -*-
"""Extract SBC/AVDTP events from .bes; scan .cfa for media packets and bad sync."""
import struct, os, re

LOGDIR = os.path.dirname(os.path.abspath(__file__))
BES = os.path.join(LOGDIR, '电流和卡顿.bes')
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')

print("========== ESMEvent (SBC / loss / CRC / sync / gap) ==========")
with open(BES, 'rb') as f:
    text = f.read().decode('gbk', 'replace')
lines = text.splitlines()
in_event = False
for ln in lines:
    if ln.startswith('[ESMEvent]'):
        in_event = True
        continue
    if in_event and ln.startswith('['):
        break
    if in_event and re.search(r'SBC|loss|CRC|synch|gap|Corrupt|Truncat|packet', ln, re.I):
        print(ln)

print("\n========== ESMEvent (frames of interest 7920/9538/11018/12759/14207/15089/20979 + context) ==========")
targets = {7920, 9538, 11018, 12759, 14207, 15089, 20979}
in_event = False
for ln in lines:
    if ln.startswith('[ESMEvent]'):
        in_event = True
        continue
    if in_event and ln.startswith('['):
        break
    if in_event:
        m = re.match(r'\d+,\d+,(\d+),', ln)
        if m and int(m.group(1)) in targets:
            print(ln)

print("\n========== .cfa media scan ==========")
with open(CFA, 'rb') as f:
    data = f.read()
print(f"cfa size={len(data)}")

# find RTP (0x80 0x60) sequences with plausible structure
n_media = 0
bad = []
i = 0
seen = set()
while True:
    j = data.find(b'\x80\x60', i)
    if j < 0:
        break
    if j + 14 <= len(data):
        seq = struct.unpack_from('>H', data, j + 2)[0]
        rts = struct.unpack_from('>I', data, j + 4)[0]
        ssrc = struct.unpack_from('>I', data, j + 8)[0]
        fcnt = data[j + 12]
        sync = data[j + 13]
        if (rts % 1280) == 0 and fcnt <= 15 and (ssrc in (0, 0x01000000)):
            key = (j & ~0xF, seq, rts)
            if key not in seen:
                seen.add(key)
                n_media += 1
                if sync != 0x9C:
                    bad.append((j, seq, rts, ssrc, fcnt, sync))
    i = j + 2
print(f"media-like={n_media} bad_sync={len(bad)}")
for (j, seq, rts, ssrc, fcnt, sync) in bad[:20]:
    ctx = data[j - 16:j + 48]
    print(f"  off=0x{j:X} seq={seq} rtp_ts={rts} ssrc=0x{ssrc:08X} fcnt={fcnt} sync=0x{sync:02X}")
    print(f"    -16..+48: {ctx.hex(' ')}")
