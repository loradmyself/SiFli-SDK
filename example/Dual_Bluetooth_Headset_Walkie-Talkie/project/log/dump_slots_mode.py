# -*- coding: utf-8 -*-
"""Dump MAX_SLOTS_CHANGE (0x1B) / MODE_CHANGE (0x14) and per-handle num-completed rates."""
import struct, datetime, sys, collections

path = sys.argv[1] if len(sys.argv) > 1 else "窗口2_COM24_(2026-10-23-12-34-44).pcap"
data = open(path, 'rb').read()
off = 24
pkts = []
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
    off += 16
    pkts.append((ts_sec + ts_usec / 1e6, data[off:off + incl]))
    off += incl


def fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]


nc_rate = collections.defaultdict(lambda: collections.Counter())  # handle -> sec -> count
for t, pkt in pkts:
    if len(pkt) < 13:
        continue
    d = 'TX' if pkt[10] == 0x61 else 'RX'
    h4 = pkt[11]
    body = pkt[12:]
    if h4 == 0x04 and d == 'RX' and len(body) >= 2:
        evt, plen = body[0], body[1]
        p = body[2:]
        if evt == 0x1B and len(p) >= 4:
            hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF
            print(f"[{fmt(t)}] MAX_SLOTS_CHANGE 0x{hdl:03X} max_slots={p[3]}")
        elif evt == 0x14 and len(p) >= 5:
            hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF
            print(f"[{fmt(t)}] MODE_CHANGE 0x{hdl:03X} mode={p[3]} "
                  f"interval={struct.unpack_from('<H', p, 4)[0]} st={p[0]}")
        elif evt == 0x38:
            print(f"[{fmt(t)}] LINK_SUP_TO {p[:8].hex()}")
        elif evt in (0x13, 0x0B) and len(p) >= 1:
            n = p[0]
            i = 1
            for _ in range(n):
                if i + 4 <= len(p):
                    hdl = struct.unpack_from('<H', p, i)[0] & 0x0FFF
                    cnt = struct.unpack_from('<H', p, i + 2)[0]
                    nc_rate[hdl][int(t)] += cnt
                    i += 4

print("-- num-completed packets/sec per handle --")
for hdl in sorted(nc_rate):
    secs = nc_rate[hdl]
    print(f"  handle 0x{hdl:03X}:")
    for s in sorted(secs):
        print(f"    {datetime.datetime.fromtimestamp(s).strftime('%H:%M:%S')}  n={secs[s]}")
