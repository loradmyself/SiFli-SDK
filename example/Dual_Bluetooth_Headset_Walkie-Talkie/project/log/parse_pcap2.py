# -*- coding: utf-8 -*-
"""Deeper pcap analysis: time ranges, RTP stats, hunt sniffer's corrupt seq/ts/ssrc."""
import struct, os, glob, datetime

LOGDIR = os.path.dirname(os.path.abspath(__file__))


def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic = struct.unpack_from('<I', data, 0)[0]
    assert magic == 0xA1B2C3D4, hex(magic)
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkt = data[off:off + incl]
        off += incl
        pkts.append((ts_sec, ts_usec, pkt))
    return pkts


def find_rtp(pkt):
    """Return list of (pay_off, seq, ts, ssrc, fcnt, sync, acl_len) for RTP-like payloads on CID 0x0042."""
    out = []
    for cid_off in range(4, min(len(pkt) - 20, 100)):
        if pkt[cid_off] == 0x42 and pkt[cid_off + 1] == 0x00:
            l2len = struct.unpack_from('<H', pkt, cid_off - 2)[0]
            pay = cid_off + 2
            if l2len < 100 or pay + 13 >= len(pkt):
                continue
            if pkt[pay] != 0x80:
                continue
            seq = struct.unpack_from('>H', pkt, pay + 2)[0]
            ts = struct.unpack_from('>I', pkt, pay + 4)[0]
            ssrc = struct.unpack_from('>I', pkt, pay + 8)[0]
            fcnt = pkt[pay + 12]
            sync = pkt[pay + 13]
            out.append((pay, seq, ts, ssrc, fcnt, sync, l2len, cid_off - 2))
    return out


def main():
    for path in sorted(glob.glob(os.path.join(LOGDIR, '*.pcap'))):
        pkts = parse_pcap(path)
        if not pkts:
            continue
        t0 = pkts[0][0] + pkts[0][1] / 1e6
        t1 = pkts[-1][0] + pkts[-1][1] / 1e6
        name = os.path.basename(path)
        print(f"\n== {name}: {len(pkts)} pkts "
              f"{datetime.datetime.fromtimestamp(t0).strftime('%H:%M:%S.%f')[:-3]} .. "
              f"{datetime.datetime.fromtimestamp(t1).strftime('%H:%M:%S.%f')[:-3]}")
        n_media = 0
        anomalies = []
        seqs = {}
        for i, (s, u, pkt) in enumerate(pkts):
            for (pay, seq, ts, ssrc, fcnt, sync, l2len, l2off) in find_rtp(pkt):
                n_media += 1
                ts_s = s + u / 1e6
                tags = []
                if sync != 0x9C:
                    tags.append(f"sync=0x{sync:02X}")
                if fcnt > 15:
                    tags.append(f"fcnt={fcnt}")
                if ssrc not in (0x01000000, 0):
                    tags.append(f"ssrc=0x{ssrc:08X}")
                if l2len != 643:
                    tags.append(f"l2len={l2len}")
                # RTP version check
                if pkt[pay] >> 6 != 2:
                    tags.append(f"rtpv={pkt[pay]>>6}")
                if tags:
                    anomalies.append((ts_s, i, seq, ts, ssrc, fcnt, sync, l2len, tags, pkt[pay:pay + 16].hex(' ')))
                # track seq continuity per ssrc
                key = ssrc
                seqs.setdefault(key, []).append((seq, ts_s, i))
        print(f"  media={n_media} anomalies={len(anomalies)}")
        for a in anomalies[:12]:
            ts_s, i, seq, ts, ssrc, fcnt, sync, l2len, tags, hdr = a
            print(f"   @{datetime.datetime.fromtimestamp(ts_s).strftime('%H:%M:%S.%f')[:-3]} "
                  f"pkt#{i} seq={seq} rtp_ts={ts} fcnt={fcnt} l2len={l2len} "
                  f"tags={tags} rtp16={hdr}")
        # seq gaps
        for key, lst in seqs.items():
            lst.sort(key=lambda x: x[1])
            gaps = []
            for j in range(1, len(lst)):
                d = (lst[j][0] - lst[j - 1][0]) & 0xFFFF
                if d != 1 and d != 0:
                    gaps.append((lst[j][1], lst[j - 1][0], lst[j][0], lst[j][2]))
            if gaps:
                print(f"   ssrc=0x{key:08X}: {len(gaps)} seq gaps of {len(lst)} pkts")
                for g in gaps[:8]:
                    ts_s, pseq, nseq, idx = g
                    print(f"      gap {pseq}->{nseq} @{datetime.datetime.fromtimestamp(ts_s).strftime('%H:%M:%S.%f')[:-3]} pkt#{idx}")


if __name__ == '__main__':
    main()
