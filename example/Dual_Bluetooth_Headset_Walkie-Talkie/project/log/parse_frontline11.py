# -*- coding: utf-8 -*-
"""Raw search for the missing slots' RTP timestamps in every capture file.
A match anywhere (even if framing is garbled) means the data was on the wire."""
import struct, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))

slots = [
    (2534400, 'gap1 ssrc0 seq1979'),
    (3191040, 'gap2 ssrc0 seq2492'),
    (3192320, 'gap2 ssrc0 seq2493'),
    (4305920, 'gap3 ssrc1 seq95'),
    (4785920, 'gap4 ssrc0 seq3738'),
]

def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkts.append(data[off:off + incl])
        off += incl
    return pkts

files = [os.path.join(LOGDIR, '电流和卡顿.cfa')] + sorted(glob.glob(os.path.join(LOGDIR, '*.pcap')))
for path in files:
    name = os.path.basename(path)
    if path.endswith('.pcap'):
        data = b''.join(parse_pcap(path))
    else:
        with open(path, 'rb') as f:
            data = f.read()
    line = [name]
    for ts, label in slots:
        pat = struct.pack('>I', ts)
        n = data.count(pat)
        line.append(f"{label.split()[1]}:{n}")
    print('  '.join(line))
