# -*- coding: utf-8 -*-
"""Check whether corrupt/spliced packets exist in host-side pcap (HCI log snoop)
with or without the 'get_new_dlc_id' splice. Also scan ALL pcaps for the string."""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))
PROBE = b'get_new_dlc_id'

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

print("== scan all pcaps for 'get_new_dlc_id' ==")
for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
    pkts = parse_pcap(path)
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    n_hit = 0
    for pi, (s, u, pkt) in enumerate(pkts):
        j = pkt.find(PROBE)
        if j >= 0:
            n_hit += 1
            if n_hit <= 4:
                print(f"  {name} pkt#{pi} t={s}.{u:06d} splice at offset {j}: {pkt[max(0,j-8):j+32]!r}")
    print(f"  {name}: {len(pkts)} pkts, {n_hit} containing probe")

print("\n== check the 8 corrupt timestamps in pcaps ==")
targets = [5992960, 3708160, 925440, 1509120, 2068480, 2781440, 3452160, 14726400]
for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
    pkts = parse_pcap(path)
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    for pi, (s, u, pkt) in enumerate(pkts):
        for ts in targets:
            pat = struct.pack('>I', ts)
            j = pkt.find(pat)
            while j >= 0:
                if j >= 4 and pkt[j - 4] == 0x80:
                    seq = struct.unpack_from('>H', pkt, j - 2)[0]
                    ssrc = struct.unpack_from('>I', pkt, j + 4)[0]
                    spliced = PROBE in pkt[j - 4: j - 4 + 643]
                    # check internal syncs
                    bad = [k for k in range(10) if pkt[j - 4 + 13 + k * 63] != 0x9C]
                    print(f"  {name} pkt#{pi} ts={ts} seq={seq} ssrc=0x{ssrc:08X} "
                          f"splice={spliced} bad_frames={bad}")
                j = pkt.find(pat, j + 1)
