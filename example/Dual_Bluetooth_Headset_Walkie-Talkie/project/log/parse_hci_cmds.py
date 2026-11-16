# -*- coding: utf-8 -*-
"""HCI command opcode / event histogram from serial-tool pcap (linktype 160).

Focus: link-policy & QoS & flush-timeout commands, Mode Change (0x14),
Sniff Subrating (0x2E), QoS Setup Complete (0x0C), Num Completed (0x0B).
"""
import struct, os, sys, glob, datetime, collections

LOGDIR = os.path.dirname(os.path.abspath(__file__))

CMD_NAMES = {
    0x0405: 'DISCONNECT', 0x0406: 'AUTH_REQ', 0x0408: 'SET_CONN_ENCRYPT',
    0x0409: 'CHANGE_CONN_LK', 0x040B: 'REMOTE_NAME_REQ',
    0x040D: 'READ_REMOTE_FEAT', 0x0419: 'READ_REMOTE_EXT_FEAT',
    0x041A: 'REMOTE_NAME_CANCEL', 0x041C: 'READ_CLOCK_OFFSET',
    0x0801: 'HOLD_MODE', 0x0803: 'SNIFF_MODE', 0x0804: 'EXIT_SNIFF_MODE',
    0x0805: 'PARK_STATE', 0x0806: 'EXIT_PARK_STATE',
    0x0807: 'QOS_SETUP', 0x0809: 'QOS_SETUP?', 0x080D: 'LINK_POLICY',
    0x080F: 'WRITE_DEF_LINK_POLICY', 0x0C01: 'SET_EV_MASK',
    0x0C03: 'RESET', 0x0C05: 'SET_EV_FILTER', 0x0C08: 'FLUSH',
    0x0C13: 'WRITE_LOCAL_NAME?', 0x0C14: 'READ_CONN_ACCEPT_TO',
    0x0C15: 'WRITE_CONN_ACCEPT_TO', 0x0C16: 'READ_PAGE_TO',
    0x0C17: 'WRITE_PAGE_TO', 0x0C1A: 'WRITE_SCAN_EN',
    0x0C20: 'WRITE_AUTH_EN', 0x0C23: 'READ_CLASS_OF_DEV',
    0x0C24: 'WRITE_CLASS_OF_DEV', 0x0C25: 'READ_VOICE_SET',
    0x0C26: 'WRITE_VOICE_SET', 0x0C28: 'WRITE_AUTO_FLUSH_TO',
    0x0C29: 'WRITE_NUM_BCAST_RETRANS', 0x0C2A: 'HOLD_MODE_CYCLE',
    0x0C33: 'WRITE_LINK_SUPER_TO', 0x0C39: 'WRITE_IAC_LAP',
    0x0C45: 'WRITE_INQUIRY_MODE', 0x0C52: 'WRITE_EIR',
    0x0C56: 'WRITE_INQ_SCAN_TYPE', 0x0C5A: 'WRITE_PAGE_SCAN_TYPE',
    0x0C5B: 'WRITE_AFH_ASSESS', 0x0C61: 'WRITE_EXT_INQ_RESP',
    0x0C67: 'WRITE_SSP_MODE', 0x0C69: 'READ_INQ_RSP_TX_POW',
    0x0C71: 'WRITE_LE_HOST_SUP', 0x0C7C: 'WRITE_SECURE_CONNS',
    0x0C7D: 'READ_AUTH_PAY_TO', 0x0C7E: 'WRITE_AUTH_PAY_TO',
    0x0C80: 'SET_EVT_MASK2', 0x0C81: 'READ_PAGE_SCAN_ACT',
    0x0C83: 'WRITE_PAGE_SCAN_ACT', 0x0C84: 'READ_INQ_SCAN_ACT',
    0x0C86: 'WRITE_INQ_SCAN_ACT', 0x0C90: 'SET_ECOSYSTEM_BASE',
    0x0C92: 'READ_PAGE_SCAN_REP', 0x0C93: 'WRITE_PAGE_SCAN_REP',
    0x0C94: 'WRITE_PAGE_SCAN_REP2?',
    0x2001: 'LE_SET_EV_MASK', 0x2002: 'LE_READ_BUF_SIZE',
    0x2005: 'LE_SET_RAND_ADDR', 0x2006: 'LE_SET_ADV_PARAM',
    0x2008: 'LE_SET_ADV_DATA', 0x2009: 'LE_SET_SCAN_RSP',
    0x200A: 'LE_SET_ADV_EN', 0x200B: 'LE_SET_SCAN_PARAM',
    0x200C: 'LE_SET_SCAN_EN', 0x200D: 'LE_CREATE_CONN',
    0x200E: 'LE_CREATE_CONN_CANCEL', 0x2011: 'LE_CONN_UPDATE',
    0x2013: 'LE_READ_CH_MAP', 0x2016: 'LE_ENC_CHANGE?',
    0x2017: 'LE_LTK_REQ_REPLY', 0x201A: 'LE_READ_STATES',
    0x201D: 'LE_START_ENC', 0x2022: 'LE_SET_PHY?',
    0x2043: 'LE_EXT_CREATE_CONN',
}

EVT_NAMES = {
    0x03: 'CONN_COMPLETE', 0x04: 'CONN_REQ', 0x05: 'DISCONN_COMPLETE',
    0x06: 'AUTH_COMPLETE', 0x08: 'ENCRYPT_CHANGE', 0x0B: 'NUM_COMPLETED',
    0x0C: 'MODE_CHANGE', 0x0E: 'CMD_COMPLETE', 0x0F: 'CMD_STATUS',
    0x13: 'NUM_COMPLETED', 0x14: 'MODE_CHANGE', 0x1B: 'MAX_SLOTS_CHANGE',
    0x1C: 'PIN_CODE_REQ', 0x1D: 'LINK_KEY_REQ', 0x1E: 'LINK_KEY_NOTIFY',
    0x20: 'DATA_BUF_OVERFLOW', 0x23: 'PAGE_SCAN_REP_MODE',
    0x2C: 'SYNC_CONN_COMPLETE', 0x2D: 'EXT_INQ_RESULT',
    0x30: 'ENC_KEY_REFRESH', 0x31: 'IO_CAP_REQ', 0x32: 'IO_CAP_RSP',
    0x33: 'USER_CONFIRM_REQ', 0x34: 'USER_PASSKEY_REQ',
    0x36: 'SIMPLE_PAIR_COMPLETE', 0x38: 'LINK_SUP_TO',
    0x3E: 'LE_META', 0x38: 'LINK_SUPER_TIMEOUT',
    0x57: 'AUTH_PAY_TO_EXPIRE', 0x58: 'SAM_STATUS_CHANGE',
}


def fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]


def analyze(path, mode='hist'):
    name = os.path.basename(path).encode('ascii', 'replace').decode()
    data = open(path, 'rb').read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkts.append((ts_sec + ts_usec / 1e6, data[off:off + incl]))
        off += incl
    if not pkts:
        return
    print(f"\n===== {name}: {len(pkts)} pkts {fmt(pkts[0][0])} .. {fmt(pkts[-1][0])}")

    cmd_hist = collections.Counter()
    evt_hist = collections.Counter()
    interesting = []

    for t, pkt in pkts:
        if len(pkt) < 13:
            continue
        direction = 'TX' if pkt[10] == 0x61 else 'RX'
        h4 = pkt[11]
        body = pkt[12:]
        if h4 == 0x01 and direction == 'TX' and len(body) >= 3:
            op = struct.unpack_from('<H', body, 0)[0]
            plen = body[2]
            cmd_hist[op] += 1
            # record link-policy / QoS / flush / sniff family in full
            if (op & 0x1FFF) in (0x0803, 0x0804, 0x0805, 0x0806, 0x0807, 0x0808,
                                 0x0809, 0x080D, 0x080F, 0x0C08, 0x0C28, 0x0C33):
                interesting.append((t, direction, f"CMD {CMD_NAMES.get(op, hex(op))} "
                                    f"op=0x{op:04X} plen={plen} {body[3:3 + min(plen, 24)].hex()}"))
        elif h4 == 0x04 and direction == 'RX' and len(body) >= 2:
            evt, plen = body[0], body[1]
            evt_hist[evt] += 1
            p = body[2:]
            if evt == 0x0C and len(p) >= 5:                # MODE_CHANGE
                st = p[0]
                hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF
                mode = p[3]
                interval = struct.unpack_from('<H', p, 4)[0] if len(p) >= 6 else -1
                mn = {0: 'ACTIVE', 1: 'HOLD', 2: 'SNIFF', 3: 'PARK'}.get(mode, mode)
                interesting.append((t, direction, f"MODE_CHANGE 0x{hdl:03X} -> {mn} "
                                    f"interval={interval} status={st}"))
            elif evt == 0x0B and mode != 'hist':            # NUM_COMPLETED verbose
                n = p[0] if p else 0
                i = 1
                pairs = []
                for _ in range(n):
                    if i + 4 <= len(p):
                        hdl = struct.unpack_from('<H', p, i)[0] & 0x0FFF
                        cnt = struct.unpack_from('<H', p, i + 2)[0]
                        pairs.append(f"0x{hdl:03X}:{cnt}")
                        i += 4
                interesting.append((t, direction, f"NUM_COMPL {pairs}"))
            elif evt == 0x0C:                                # already handled
                pass
            elif evt in (0x0F, 0x0E):
                # status of link-policy family
                if evt == 0x0F and len(p) >= 4:
                    st_op = struct.unpack_from('<H', p, 1)[0]
                    if st_op in (0x0803, 0x0804, 0x0805, 0x0806, 0x0807, 0x0809,
                                 0x080D, 0x0C28):
                        interesting.append((t, direction,
                                            f"CMD_STATUS {CMD_NAMES.get(st_op, hex(st_op))} "
                                            f"status={p[0]}"))
            elif evt == 0x1B and len(p) >= 4:                # MAX_SLOTS_CHANGE
                hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF
                interesting.append((t, direction, f"MAX_SLOTS_CHANGE 0x{hdl:03X} max={p[3]}"))
            elif evt == 0x38:
                hdl = struct.unpack_from('<H', p, 1)[0] & 0x0FFF if len(p) >= 3 else -1
                interesting.append((t, direction, f"LINK_SUP_TIMEOUT 0x{hdl:03X}"))

    print("  -- command opcodes --")
    for op, n in cmd_hist.most_common():
        print(f"    0x{op:04X} {CMD_NAMES.get(op, '?'):24s} n={n}")
    print("  -- event codes --")
    for ev, n in evt_hist.most_common():
        print(f"    0x{ev:02X} {EVT_NAMES.get(ev, '?'):24s} n={n}")
    print("  -- interesting (policy/qos/flush/mode) --")
    for t, d, txt in interesting:
        print(f"  [{fmt(t)}] {d} {txt}")


if __name__ == '__main__':
    args = sys.argv[1:]
    mode = 'hist'
    if args and args[0] == '-v':
        mode = 'verbose'
        args = args[1:]
    files = args or sorted(glob.glob(os.path.join(LOGDIR, '*.pcap')))
    for fp in files:
        try:
            analyze(fp, mode)
        except Exception as e:
            import traceback
            print(f"ERR {fp}: {type(e).__name__}: {e}")
            traceback.print_exc()
