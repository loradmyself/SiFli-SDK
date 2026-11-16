# -*- coding: utf-8 -*-
"""Full AVDTP + L2CAP + media parse of serial-tool pcap (linktype 160).

Tracks dynamic L2CAP channels (PSM from conn req/rsp), then decodes AVDTP
signalling (GET_CAP/SET_CFG/OPEN/START/SUSPEND/CLOSE) on the signalling
channel and RTP media stats (SSRC/seq/ts) on the transport channel.
"""
import struct, os, sys, glob, datetime

LOGDIR = os.path.dirname(os.path.abspath(__file__))

SIG_NAMES = {0x01: 'DISCOVER', 0x02: 'GET_CAP', 0x03: 'SET_CFG', 0x04: 'GET_CFG',
             0x05: 'SET_DELAY', 0x06: 'OPEN', 0x07: 'START', 0x08: 'CLOSE',
             0x09: 'SUSPEND', 0x0A: 'ABORT', 0x0B: 'SEC_CTRL', 0x0C: 'GET_ALL_CAP',
             0x0D: 'DELAYREPORT'}
MSG_NAMES = {0: 'CMD', 1: 'ACCEPT', 2: 'REJECT', 3: 'NOTIMP'}
SBC_SF = [16000, 32000, 44100, 48000]
SBC_CH = ['MONO', 'DUAL', 'STEREO', 'JOINT']


def fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]


def codec_cfg_bitmask(o):
    """A2DP SBC codec info element: fields are BITMASKS."""
    sf = [f for i, f in enumerate(SBC_SF) if o[0] & (0x80 >> i)]
    ch = [c for i, c in enumerate(SBC_CH) if o[0] & (0x08 >> i)]
    bl = [b for i, b in enumerate([4, 8, 12, 16]) if o[1] & (0x80 >> i)]
    sb = [s for i, s in enumerate([4, 8]) if o[1] & (0x08 >> i)]
    al = []
    if o[1] & 0x02:
        al.append('SNR')
    if o[1] & 0x01:
        al.append('LOUD')
    return (f"sf{sf} ch{ch} blk{bl} sb{sb} alloc{al} "
            f"bp{o[2]}..{o[3]}")


def codec_cfg_value(o):
    """SBC frame header style (value fields) — used for SET_CFG single config."""
    sfi, chi = (o[0] >> 4) & 0xF, o[0] & 0xF
    bi = (o[1] >> 4) & 0xF
    sf = SBC_SF[sfi] if sfi < 4 else f'sf{sfi}'
    ch = SBC_CH[chi] if chi < 4 else f'ch{chi}'
    blocks = [4, 8, 12, 16][bi] if bi < 4 else f'b{bi}'
    sb = 4 if ((o[1] >> 2) & 3) == 0 else 8
    alloc = 'LOUD' if (o[1] >> 1) & 1 else 'SNR'
    return f"{sf} {ch} blk{blocks} sb{sb} {alloc} bp{o[2]}..{o[3]}"


def analyze(path):
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    data = open(path, 'rb').read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkts.append((ts_sec + ts_usec / 1e6, data[off:off + incl]))
        off += incl
    print(f"\n===== {name}: {len(pkts)} pkts {fmt(pkts[0][0])} .. {fmt(pkts[-1][0])}")

    handles = {}                     # hdl -> addr
    cid_psm = {}                     # (hdl, cid) -> psm
    cid_role = {}                    # (hdl, cid) -> 'avdtp_sig' | 'avdtp_media' | 'avctp'
    avdtp_sig_seen = {}              # hdl -> count of l2cap channels on PSM 0x19
    media_stats = {}                 # hdl -> dict
    events = []

    def mark_channel(hdl, cid_a, cid_b, psm):
        cid_psm[(hdl, cid_a)] = psm
        cid_psm[(hdl, cid_b)] = psm
        if psm == 0x0019:
            n = avdtp_sig_seen.get(hdl, 0)
            role = 'avdtp_sig' if n == 0 else 'avdtp_media'
            avdtp_sig_seen[hdl] = n + 1
            cid_role[(hdl, cid_a)] = role
            cid_role[(hdl, cid_b)] = role
        elif psm == 0x0017:
            cid_role[(hdl, cid_a)] = 'avctp'
            cid_role[(hdl, cid_b)] = 'avctp'

    for t, pkt in pkts:
        if len(pkt) < 13:
            continue
        direction = 'TX' if pkt[10] == 0x61 else 'RX'
        h4 = pkt[11]
        body = pkt[12:]
        if h4 == 0x04 and direction == 'RX' and len(body) >= 2:
            evt, plen = body[0], body[1]
            p = body[2:]
            if evt == 0x03 and len(p) >= 11:
                hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF
                addr = ':'.join(f'{b:02X}' for b in p[3:9])
                handles[hdl] = addr
                events.append((t, f"CONN COMPLETE 0x{hdl:03X} {addr} status={p[0]}"))
            elif evt == 0x05 and len(p) >= 3:
                hdl = struct.unpack_from('<H', p, 0)[0] & 0x0FFF
                events.append((t, f"DISCONN 0x{hdl:03X} reason={p[2]:02X}"))
            elif evt in (0x2C, 0x3E):
                events.append((t, f"evt 0x{evt:02X} len={plen} {body[2:2+min(plen,12)].hex()}"))
            continue
        if h4 != 0x02 or len(body) < 8:
            continue
        hdl = struct.unpack_from('<H', body, 0)[0] & 0x0FFF
        l2 = body[4:]
        l2len, cid = struct.unpack_from('<HH', l2, 0)
        pay = l2[4:4 + l2len] if l2len <= len(l2) - 4 else l2[4:]
        addr = handles.get(hdl, f'0x{hdl:03X}')

        if cid == 0x0001:
            # L2CAP signalling
            i = 0
            while i + 4 <= len(pay):
                code, ident, ln = pay[i], pay[i + 1], struct.unpack_from('<H', pay, i + 2)[0]
                d = pay[i + 4:i + 4 + ln]
                i += 4 + ln
                if code == 0x02 and len(d) >= 4:                # conn req
                    psm, scid = struct.unpack_from('<HH', d, 0)
                    cid_psm[(hdl, scid)] = psm
                    events.append((t, f"L2CAP CONN REQ 0x{hdl:03X} psm=0x{psm:04X} scid=0x{scid:04X}"))
                elif code == 0x03 and len(d) >= 8:              # conn rsp
                    dcid, scid, res, st = struct.unpack_from('<HHHH', d, 0)
                    if res == 0:
                        psm = cid_psm.get((hdl, scid))
                        if psm is None:
                            psm = cid_psm.get((hdl, dcid))
                        if psm is not None:
                            mark_channel(hdl, dcid, scid, psm)
                    events.append((t, f"L2CAP CONN RSP 0x{hdl:03X} dcid=0x{dcid:04X} scid=0x{scid:04X} res={res}"))
            continue

        role = cid_role.get((hdl, cid))
        if role is None and len(pay) >= 14 and pay[0] == 0x80:
            role = 'avdtp_media'
            cid_role[(hdl, cid)] = role

        if role == 'avdtp_media' and len(pay) >= 13 and pay[0] == 0x80:
            s = media_stats.setdefault(hdl, {
                'n': 0, 'ssrc': set(), 'seq0': None, 'seq_last': None,
                'gaps': 0, 'cfgs': {}, 'bad': 0, 'first': t, 'last': t})
            seq = struct.unpack_from('>H', pay, 2)[0]
            rtp_ts = struct.unpack_from('>I', pay, 4)[0]
            ssrc = struct.unpack_from('>I', pay, 8)[0]
            s['n'] += 1
            s['ssrc'].add(ssrc)
            if s['seq0'] is None:
                s['seq0'] = (seq, rtp_ts)
            elif s['seq_last'] is not None and seq != ((s['seq_last'] + 1) & 0xFFFF):
                s['gaps'] += 1
            s['seq_last'] = seq
            s['last'] = t
            if pay[13] == 0x9C:
                cfg = f"{SBC_SF[(pay[14]>>6)&3]} {SBC_CH[(pay[14]>>2)&3]} " \
                      f"blk{[4,8,12,16][(pay[14]>>4)&3]} sb{4 if (pay[14]&1)==0 else 8} " \
                      f"{'LOUD' if (pay[15]>>1)&1 else 'SNR'} bp{pay[15]}"
                s['cfgs'][cfg] = s['cfgs'].get(cfg, 0) + 1
            else:
                s['bad'] += 1
        elif role == 'avdtp_sig' and len(pay) >= 2:
            label = (pay[0] >> 4) & 0xF
            msg_type = pay[0] & 0x03
            sig_id = pay[1]
            name = SIG_NAMES.get(sig_id, hex(sig_id))
            extra = ''
            if sig_id in (0x03, 0x02, 0x0C, 0x04):
                i = 4 if (sig_id == 0x03 and msg_type == 0) else 2
                caps = []
                while i + 2 <= len(pay):
                    cat, clen = pay[i], pay[i + 1]
                    if cat == 0x07 and clen >= 6 and i + 8 <= len(pay):
                        o = pay[i + 4:i + 8]
                        caps.append(f"codec[{codec_cfg_bitmask(o)}|val {codec_cfg_value(o)}]")
                    elif cat:
                        caps.append(f"cat{cat:02X}({clen})")
                    i += 2 + clen
                extra = ' ' + ' '.join(caps)
            if sig_id == 0x03 and msg_type == 0 and len(pay) >= 4:
                extra = f" acp=0x{pay[2]>>2:02X} int=0x{pay[3]>>2:02X}" + extra
            print(f"  [{fmt(t)}] {direction} 0x{hdl:03X} {addr}: "
                  f"L{label} {name} {MSG_NAMES.get(msg_type, msg_type)}{extra}")
        elif role == 'avctp':
            pass

    print("  -- controller events --")
    for t, txt in events:
        print(f"  [{fmt(t)}] {txt}")
    print("  -- media stats --")
    for h in sorted(media_stats):
        s = media_stats[h]
        print(f"  0x{h:03X} {handles.get(h,'?')}: n={s['n']} ssrc={s['ssrc']} "
              f"seq0={s['seq0']} gaps={s['gaps']} bad_sync={s['bad']}")
        print(f"      cfgs: {s['cfgs']}")
        print(f"      span: {fmt(s['first'])} .. {fmt(s['last'])}")


if __name__ == '__main__':
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(LOGDIR, '*.pcap')))
    for fp in files:
        try:
            analyze(fp)
        except Exception as e:
            import traceback
            print(f"ERR {fp}: {type(e).__name__}: {e}")
            traceback.print_exc()
