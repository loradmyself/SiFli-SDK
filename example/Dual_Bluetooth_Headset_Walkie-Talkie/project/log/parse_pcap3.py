# -*- coding: utf-8 -*-
"""Full-scan all L2CAP CIDs for RTP media; hunt bad SBC sync; seq continuity; time windows."""
import struct, os, glob, datetime

LOGDIR = os.path.dirname(os.path.abspath(__file__))


def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkt = data[off:off + incl]
        off += incl
        pkts.append((ts_sec + ts_usec / 1e6, pkt))
    return pkts


def fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]


def scan_file(path):
    pkts = parse_pcap(path)
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    print(f"\n== {name}: {len(pkts)} pkts {fmt(pkts[0][0])} .. {fmt(pkts[-1][0])}")
    streams = {}   # (cid, ssrc) -> list of (t, seq, rtp_ts, fcnt, sync, pkt_idx, pay)
    bad = []
    for i, (t, pkt) in enumerate(pkts):
        # find L2CAP frames: scan for plausible len/CID pairs preceding RTP 0x80
        for off in range(2, min(len(pkt) - 30, 60)):
            l2len = struct.unpack_from('<H', pkt, off)[0]
            cid = struct.unpack_from('<H', pkt, off + 2)[0]
            pay = off + 4
            if l2len < 14 or l2len > 700:
                continue
            if pay + 14 > len(pkt):
                continue
            if pkt[pay] != 0x80:
                continue
            pt = pkt[pay + 1]
            if pt != 0x60:
                continue
            seq = struct.unpack_from('>H', pkt, pay + 2)[0]
            rtp_ts = struct.unpack_from('>I', pkt, pay + 4)[0]
            ssrc = struct.unpack_from('>I', pkt, pay + 8)[0]
            fcnt = pkt[pay + 12]
            sync = pkt[pay + 13]
            key = (cid, ssrc)
            streams.setdefault(key, []).append((t, seq, rtp_ts, fcnt, sync, i, pay, l2len))
            if sync != 0x9C or fcnt > 15:
                bad.append((t, cid, ssrc, seq, rtp_ts, fcnt, sync, i, pay, l2len, pkt))
            break  # one media payload per raw packet is enough
    for key in sorted(streams):
        cid, ssrc = key
        lst = streams[key]
        gaps = []
        for j in range(1, len(lst)):
            d = (lst[j][1] - lst[j - 1][1]) & 0xFFFF
            if d != 1 and d != 0:
                gaps.append((lst[j][0], lst[j - 1][1], lst[j][1], d))
        print(f"  cid=0x{cid:04X} ssrc=0x{ssrc:08X}: n={len(lst)} gaps={len(gaps)}")
        for g in gaps[:10]:
            print(f"      seq {g[1]}->{g[2]} (missing {g[3]-1}) @{fmt(g[0])}")
    print(f"  BAD sync/fcnt: {len(bad)}")
    for b in bad[:20]:
        t, cid, ssrc, seq, rtp_ts, fcnt, sync, i, pay, l2len, pkt = b
        print(f"   @{fmt(t)} pkt#{i} cid=0x{cid:04X} ssrc=0x{ssrc:08X} seq={seq} "
              f"rtp_ts={rtp_ts} fcnt={fcnt} sync=0x{sync:02X} l2len={l2len}")
        seg = pkt[pay:pay + 32]
        print(f"      rtp+head: {seg.hex(' ')}")
    return streams, bad, pkts


def window(pkts, t_start, t_end, label):
    print(f"\n---- window {label}: {fmt(t_start)}..{fmt(t_end)} ----")
    for i, (t, pkt) in enumerate(pkts):
        if t < t_start or t > t_end:
            continue
        for off in range(2, min(len(pkt) - 30, 60)):
            l2len = struct.unpack_from('<H', pkt, off)[0]
            cid = struct.unpack_from('<H', pkt, off + 2)[0]
            pay = off + 4
            if l2len < 14 or l2len > 700 or pay + 14 > len(pkt):
                continue
            if pkt[pay] != 0x80 or pkt[pay + 1] != 0x60:
                continue
            seq = struct.unpack_from('>H', pkt, pay + 2)[0]
            rtp_ts = struct.unpack_from('>I', pkt, pay + 4)[0]
            ssrc = struct.unpack_from('>I', pkt, pay + 8)[0]
            fcnt = pkt[pay + 12]
            sync = pkt[pay + 13]
            mark = " <<<BAD>>>" if sync != 0x9C else ""
            print(f"  {fmt(t)} pkt#{i} cid=0x{cid:04X} ssrc=0x{ssrc:08X} seq={seq:5d} "
                  f"rts={rtp_ts} fcnt={fcnt} sync=0x{sync:02X}{mark}")
            break


if __name__ == '__main__':
    for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
        streams, bad, pkts = scan_file(path)
        n = os.path.basename(path)
        # focus windows around sniffer corruption events (12:37:09.2-10.0 and 12:37:30.5-31.5)
        import time as _t
        def mk(h, m, s):
            base = datetime.datetime.fromtimestamp(pkts[0][0])
            d = base.replace(hour=h, minute=m, second=s, microsecond=0)
            return d.timestamp()
        if '12-36-45' in n:
            window(pkts, mk(12, 37, 9), mk(12, 37, 10), 'sniffer event88 12:37:09-10')
        if '12-37-14' in n:
            window(pkts, mk(12, 37, 30), mk(12, 37, 32), 'sniffer events97/98 12:37:30-32')
