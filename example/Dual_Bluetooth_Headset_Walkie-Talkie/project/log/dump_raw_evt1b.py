# -*- coding: utf-8 -*-
"""Raw dump of event code 0x1B and 0x14 packets from serial pcap."""
import struct, sys

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
pkts = []
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
    off += 16
    pkts.append((ts_sec + ts_usec / 1e6, data[off:off + incl]))
    off += incl

for i, (t, pkt) in enumerate(pkts):
    if len(pkt) < 13:
        continue
    d = 'TX' if pkt[10] == 0x61 else 'RX'
    h4 = pkt[11]
    body = pkt[12:]
    if h4 == 0x04 and d == 'RX' and len(body) >= 2:
        evt, plen = body[0], body[1]
        if evt in (0x1B, 0x14, 0x3E):
            print(f"#{i} t={t:.3f} evt=0x{evt:02X} plen={plen} raw={body[2:2 + min(plen, 20)].hex()}")
