# -*- coding: utf-8 -*-
"""Parse serial-tool pcap (linktype 160) -> find AVDTP media packets with bad SBC sync."""
import struct, sys, os, glob

LOGDIR = os.path.dirname(os.path.abspath(__file__))


def parse_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, vmaj, vminor, tz, sig, snaplen, linktype = struct.unpack_from('<IHHIIII', data, 0)
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkt = data[off:off + incl]
        off += incl
        pkts.append((ts_sec, ts_usec, pkt))
    return linktype, snaplen, pkts


def find_media(pkt):
    """Locate AVDTP media payload: RTP(12) + frame_cnt(1) + SBC frames.
    Heuristic: search for CID 0x0042 (AVDTP media channel seen in captures)."""
    results = []
    for cid_off in range(0, min(len(pkt) - 20, 80)):
        if pkt[cid_off] == 0x42 and pkt[cid_off + 1] == 0x00:
            # L2CAP: len(2) CID(2) then payload
            l2len = struct.unpack_from('<H', pkt, cid_off - 2)[0] if cid_off >= 2 else 0
            pay_off = cid_off + 2
            if l2len > 10 and pay_off + 14 < len(pkt):
                results.append((cid_off - 2, l2len, pay_off))
    return results


def analyze(path):
    linktype, snaplen, pkts = parse_pcap(path)
    print(f"\n===== {os.path.basename(path)}: linktype={linktype} packets={len(pkts)}")
    n_media = 0
    n_bad = 0
    for i, (ts_sec, ts_usec, pkt) in enumerate(pkts):
        for (l2_off, l2len, pay_off) in find_media(pkt):
            if pay_off + 13 >= len(pkt):
                continue
            rtp0 = pkt[pay_off]
            if rtp0 != 0x80:
                continue
            seq = struct.unpack_from('>H', pkt, pay_off + 2)[0]
            ts = struct.unpack_from('>I', pkt, pay_off + 4)[0]
            ssrc = struct.unpack_from('>I', pkt, pay_off + 8)[0]
            fcnt = pkt[pay_off + 12]
            sync = pkt[pay_off + 13]
            n_media += 1
            if sync != 0x9C:
                n_bad += 1
                total = len(pkt)
                print(f"  BAD pkt#{i} ts={ts_sec}.{ts_usec:06d} len={total} l2len={l2len} "
                      f"seq={seq} rtp_ts={ts} ssrc={ssrc} fcnt={fcnt} sync=0x{sync:02X}")
                # dump first 64 bytes of SBC area
                sbc = pkt[pay_off + 12:pay_off + 12 + 80]
                print(f"      hdr+payload[0:80]: {sbc.hex(' ')}")
    print(f"  media pkts={n_media} bad_sync={n_bad}")


if __name__ == '__main__':
    files = sorted(glob.glob(os.path.join(LOGDIR, '*.pcap')))
    for fp in files:
        try:
            analyze(fp)
        except Exception as e:
            print(f"ERR {fp}: {e}")
