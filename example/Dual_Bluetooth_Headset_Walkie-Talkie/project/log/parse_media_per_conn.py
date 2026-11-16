# -*- coding: utf-8 -*-
"""Per-connection media timeline from serial-tool pcap (linktype 160).

Decides the silent-2nd-headset question:
  - does media TX reach the 2nd headset's ACL handle at all?   (H4: wedge)
  - are the frames fed to each handle the FIRST link's SBC cfg? (H2: config)

Frame layout per pcap packet: 10-byte serial prefix + 1 dir tag
(0x61 host->ctrl, 0x62 ctrl->host) + H4 (01 cmd / 02 acl / 04 evt).
"""
import struct, os, glob, datetime, sys

LOGDIR = os.path.dirname(os.path.abspath(__file__))


def fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]


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


SBC_SF = [16000, 32000, 44100, 48000]
SBC_CH = ['MONO', 'DUAL', 'STEREO', 'JOINT']


def sbc_cfg_name(hdr1, hdr2):
    sf = SBC_SF[(hdr1 >> 6) & 3]
    blocks = [4, 8, 12, 16][(hdr1 >> 4) & 3]
    ch = SBC_CH[(hdr1 >> 2) & 3]
    alloc = 'loud' if (hdr1 >> 1) & 1 else 'snr'
    sb = 4 if (hdr1 & 1) == 0 else 8
    return f"{sf}Hz b{blocks} {ch} {alloc} sb{sb} bp{hdr2}"


def sbc_frame_len(hdr1, hdr2):
    # Bit-accurate SBC frame layout: 32-bit header + scale factors (4 b each,
    # one less for joint) + joint flags (subbands b) + blocks*bitpool audio.
    blocks = [4, 8, 12, 16][(hdr1 >> 4) & 3]
    ch_mode = (hdr1 >> 2) & 3
    subbands = 4 if (hdr1 & 1) == 0 else 8
    nch = 1 if ch_mode == 0 else 2
    joint = 1 if ch_mode == 3 else 0
    bits = 32 + 4 * nch * subbands - 4 * joint + joint * subbands + blocks * hdr2
    return (bits + 7) // 8


def codec_cfg(octets):
    sfi = (octets[0] >> 4) & 0xF
    chi = octets[0] & 0xF
    bi = (octets[1] >> 4) & 0xF
    sf = SBC_SF[sfi] if sfi < 4 else f'sf{sfi}'
    ch = SBC_CH[chi] if chi < 4 else f'ch{chi}'
    blocks = [4, 8, 12, 16][bi] if bi < 4 else f'b{bi}'
    sb = 4 if ((octets[1] >> 2) & 0x3) == 0 else 8
    alloc = 'loud' if (octets[1] >> 1) & 1 else 'snr'
    return f"{sf}Hz b{blocks} {ch} {alloc} sb{sb} bp{octets[2]}..{octets[3]}"


def analyze(path):
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    pkts = parse_pcap(path)
    if not pkts:
        return
    print(f"\n===== {name}: {len(pkts)} pkts  {fmt(pkts[0][0])} .. {fmt(pkts[-1][0])}")
    handles = {}          # handle -> {'addr': str, 'first': t, 'last': t}
    media = {}            # handle -> list of (t, dir, seq, cfg, nframes, bad_sync)
    sig = []              # (t, addr_or_handle, text) AVDTP signalling events
    pending_cmd = {}      # opcode -> (t, addr) for commands carrying bdaddr

    for t, pkt in pkts:
        if len(pkt) < 13:
            continue
        tag = pkt[10]
        direction = 'TX' if tag == 0x61 else 'RX' if tag == 0x62 else '?'
        h4 = pkt[11]
        body = pkt[12:]
        if h4 == 0x04 and direction == 'RX':          # HCI event
            evt = body[0]
            plen = body[1]
            p = body[2:]
            if evt == 0x03 and len(p) >= 11:          # connection complete
                status, hdl = p[0], struct.unpack_from('<H', p, 1)[0] & 0x0FFF
                addr = ':'.join(f'{b:02X}' for b in p[3:9])
                link = p[9]
                handles[hdl] = {'addr': addr, 'first': t, 'last': t, 'link': link}
                print(f"  [{fmt(t)}] CONN COMPLETE handle=0x{hdl:03X} {addr} link={link}")
            elif evt == 0x0B and len(p) >= 9:         # num completed pkts (skip detail)
                pass
        elif h4 == 0x02 and len(body) >= 4:           # ACL
            hdl = struct.unpack_from('<H', body, 0)[0] & 0x0FFF
            pb = (struct.unpack_from('<H', body, 0)[0] >> 12) & 0x3
            alen = struct.unpack_from('<H', body, 2)[0]
            l2 = body[4:]
            if len(l2) < 4:
                continue
            l2len, cid = struct.unpack_from('<HH', l2, 0)
            pay = l2[4:]
            if hdl in handles:
                handles[hdl]['last'] = t
            if len(pay) >= 14 and pay[0] == 0x80 and pay[1] == 0x60:
                # RTP media
                seq = struct.unpack_from('>H', pay, 2)[0]
                fcnt = pay[12] & 0x0F
                off = 13
                cfgs = []
                bad = 0
                for _ in range(fcnt):
                    if off + 3 > len(pay):
                        break
                    if pay[off] != 0x9C:
                        bad += 1
                        break
                    cfgs.append(sbc_cfg_name(pay[off + 1], pay[off + 2]))
                    off += sbc_frame_len(pay[off + 1], pay[off + 2])
                media.setdefault(hdl, []).append(
                    (t, direction, seq, cfgs[0] if cfgs else '?', fcnt, bad))
            elif cid != 0x0001:                       # signalling-ish (not fixed)
                # AVDTP signal: byte0 = (label<<4)|(pkt_type<<2)|msg_type, byte1 = sig_id
                if len(pay) >= 12 and (pay[1] in (0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09)):
                    msg_type = pay[0] & 0x03           # 0=CMD 1=ACCEPT 2=REJECT
                    sig_id = pay[1]
                    names = {0x02: 'GET_CAP', 0x03: 'SET_CFG', 0x04: 'GET_CFG',
                             0x06: 'OPEN', 0x07: 'START', 0x08: 'CLOSE',
                             0x09: 'SUSPEND'}
                    mname = {0: 'CMD', 1: 'ACCEPT', 2: 'REJECT', 3: 'NOTIMP'}
                    codecs = []
                    if sig_id in (0x03, 0x02, 0x04):
                        # walk service capability list: SET_CFG starts at byte 4
                        # (acp_seid, int_seid), GET_CAP/GET_CFG rsp at byte 2
                        i = 4 if (sig_id == 0x03 and msg_type == 0) else 2
                        while i + 2 <= len(pay):
                            cat, clen = pay[i], pay[i + 1]
                            if cat == 0x07 and clen >= 6 and i + 8 <= len(pay):
                                codecs.append(codec_cfg(pay[i + 4:i + 8]))
                            i += 2 + clen
                    tag_s = ''
                    if sig_id == 0x03 and msg_type == 0 and len(pay) >= 4:
                        tag_s = f" acp=0x{pay[2] >> 2:02X} int=0x{pay[3] >> 2:02X}"
                    if codecs:
                        sig.append((t, hdl, direction,
                                    f"{names.get(sig_id, hex(sig_id))} {mname[msg_type]}{tag_s} "
                                    f"codec={'; '.join(codecs)}"))
                    elif sig_id in (0x06, 0x07, 0x08, 0x09) or (
                            sig_id == 0x03 and msg_type == 0):
                        sig.append((t, hdl, direction,
                                    f"{names.get(sig_id, hex(sig_id))} {mname[msg_type]}{tag_s}"))

    # ---- report ----
    print("  -- connections --")
    for h in sorted(handles):
        info = handles[h]
        n = len(media.get(h, []))
        print(f"  handle 0x{h:03X} {info['addr']}  media_pkts={n}")

    print("  -- AVDTP signalling --")
    for t, h, d, txt in sig:
        addr = handles.get(h, {}).get('addr', f'0x{h:03X}')
        print(f"  [{fmt(t)}] {d} {addr}: {txt}")

    print("  -- media timeline (per handle, per 5 s bins) --")
    for h in sorted(media):
        lst = media[h]
        addr = handles.get(h, {}).get('addr', '?')
        bins = {}
        cfgs = {}
        bad_total = 0
        for (t, d, seq, cfg, fcnt, bad) in lst:
            b = int(t) // 5 * 5
            bins.setdefault(b, []).append((t, d, seq))
            cfgs[cfg] = cfgs.get(cfg, 0) + 1
            bad_total += bad
        print(f"  handle 0x{h:03X} {addr}: total={len(lst)} bad_sync_frames={bad_total}")
        print(f"      configs: {cfgs}")
        prev = None
        for b in sorted(bins):
            items = bins[b]
            dirs = {}
            for (_, d, _) in items:
                dirs[d] = dirs.get(d, 0) + 1
            gap = '' if prev is None else f" (gap {items[0][0]-prev:.1f}s)"
            print(f"      {fmt(b)}  n={len(items)} {dirs}{gap}")
            prev = items[-1][0]

    # seq continuity per handle in the biggest window
    print("  -- seq gaps --")
    for h in sorted(media):
        lst = media[h]
        gaps = 0
        for i in range(1, len(lst)):
            if lst[i][1] == 'TX' and lst[i - 1][1] == 'TX':
                if lst[i][2] != ((lst[i - 1][2] + 1) & 0xFFFF):
                    gaps += 1
        print(f"  handle 0x{h:03X}: TX seq discontinuities={gaps}")


if __name__ == '__main__':
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(LOGDIR, '*.pcap')))
    for fp in files:
        try:
            analyze(fp)
        except Exception as e:
            print(f"ERR {fp}: {type(e).__name__}: {e}")
