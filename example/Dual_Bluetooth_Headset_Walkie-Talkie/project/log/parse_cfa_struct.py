# -*- coding: utf-8 -*-
"""Determine .cfa capture type: parse record framing and classify record contents.
Also verify insert-vs-overwrite definitively for the 8 corrupt cases."""
import struct, os

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')
with open(CFA, 'rb') as f:
    cfa = f.read()

print(f"cfa size = {len(cfa)}")
print("head 128 bytes:", cfa[:128].hex(' '))

# find the record marker 45 4d 04 08
MARK = b'\x45\x4d\x04\x08'
idxs = []
i = 0
while len(idxs) < 8:
    j = cfa.find(MARK, i)
    if j < 0:
        break
    idxs.append(j)
    i = j + 1
print(f"\nfirst record markers at: {['0x%X' % x for x in idxs]}")
for x in idxs[:4]:
    print(f"  @0x{x:X}: {cfa[x:x+40].hex(' ')}")

# distances between markers -> record framing
dists = [idxs[k+1] - idxs[k] for k in range(len(idxs)-1)]
print("distances:", dists)

# definitive insert/overwrite check
print("\n===== definitive insert-vs-overwrite on .cfa copies =====")
def cmp_case(ts):
    pat = struct.pack('>I', ts)
    rtps = []
    i = 0
    while True:
        j = cfa.find(pat, i)
        if j < 0:
            break
        if j >= 4 and cfa[j-4] == 0x80:
            rtps.append(j - 4)
        i = j + 1
    if len(rtps) < 2:
        print(f"ts {ts}: only {len(rtps)} copies")
        return
    a = cfa[rtps[0]:rtps[0]+643]
    b = cfa[rtps[1]:rtps[1]+643]
    # find insert point = first diff in payload (skip 2-3 seq, 8-11 ssrc)
    diff = None
    for k in range(12, 643):
        if a[k] != b[k]:
            diff = k
            break
    if diff is None:
        print(f"ts {ts}: identical payloads")
        return
    str_len = 23
    # test INSERT: b[diff:diff+23]==STR and b[diff+23:]==a[diff:620]
    ins_ok = (b[diff:diff+str_len] == b'get_new_dlc_id,id = 1\r\n') and (b[diff+str_len:643] == a[diff:620])
    # test OVERWRITE: b[diff:diff+23]==STR and b[diff+23:]==a[diff+23:]
    ovr_ok = (b[diff:diff+str_len] == b'get_new_dlc_id,id = 1\r\n') and (b[diff+str_len:643] == a[diff+str_len:643])
    print(f"ts {ts}: diff@{diff}  INSERT={ins_ok}  OVERWRITE={ovr_ok}")
    if not ins_ok and not ovr_ok:
        # where does b rejoin a?
        for s in range(0, 64):
            if b[diff+str_len:diff+str_len+32] == a[diff+s:diff+s+32]:
                print(f"   rejoins at a-shift +{s}")
                break
        else:
            print(f"   tail: b[600:643]={b[600:643].hex(' ')}")
            print(f"         a[600:643]={a[600:643].hex(' ')}")
            print(f"         a[577:620]={a[577:620].hex(' ')}")

for ts in (5992960, 3708160, 925440, 1509120, 2068480, 2781440, 3452160, 14726400):
    cmp_case(ts)
