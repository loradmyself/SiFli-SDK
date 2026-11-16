# -*- coding: utf-8 -*-
"""Dump full corrupt-frame packets from .cfa; check all 10 SBC frame sync positions (frame len 63)."""
import struct, os

LOGDIR = os.path.dirname(os.path.abspath(__file__))
CFA = os.path.join(LOGDIR, '电流和卡顿.cfa')

with open(CFA, 'rb') as f:
    cfa = f.read()

FRAME_LEN = 63  # SBC frame length for 16blk/8sb/joint/bitpool25 @44.1k


def dump_packet(off_rtp, label):
    """off_rtp points at RTP 0x80. L2CAP payload starts here. Total L2CAP len 643."""
    l2pay = cfa[off_rtp:off_rtp + 643]
    seq = struct.unpack_from('>H', l2pay, 2)[0]
    ts = struct.unpack_from('>I', l2pay, 4)[0]
    ssrc = struct.unpack_from('>I', l2pay, 8)[0]
    fcnt = l2pay[12]
    print(f"\n### {label}: seq={seq} ts={ts} ssrc=0x{ssrc:08X} fcnt={fcnt} len={len(l2pay)}")
    # check each SBC frame sync
    for k in range(fcnt):
        pos = 13 + k * FRAME_LEN
        if pos >= len(l2pay):
            print(f"  frame {k}: offset {pos} BEYOND packet")
            break
        sync = l2pay[pos]
        hdr = l2pay[pos:pos + 4].hex(' ')
        ok = "OK" if sync == 0x9C else "**BAD**"
        print(f"  frame {k}: off={pos} sync=0x{sync:02X} hdr={hdr} {ok}")
    return l2pay


# known corrupt frames found earlier
hits = {
    'frame20979 h80 seq4681 ts5992960': 0x653B98 - 10,   # ctx started at '02 00 00 02 80 20...' find rtp
    'frame20979 h81 seq94 ts5992960': 0x654013 - 10,
    'frame15089 h80 seq2896 ts3708160': 0x4842CC - 10,
    'frame15089 h81 seq288 ts3708160': 0x484565 - 10,
}

# locate RTP offsets precisely
for label, approx in hits.items():
    # search around approx for 0x80 0x60
    window = cfa[max(0, approx - 32): approx + 32]
    rtp_rel = window.find(b'\x80\x60')
    rtp_off = max(0, approx - 32) + rtp_rel
    dump_packet(rtp_off, label)

# Also search .cfa for ALL media packets and verify every sync; report any packet
# where ANY internal frame sync != 0x9C
print("\n\n===== full-payload sync audit of all media packets in .cfa =====")
i = 0
n_pkt = 0
bad_pkts = []
while True:
    j = cfa.find(b'\x80\x60', i)
    if j < 0 or j + 643 > len(cfa):
        break
    ts = struct.unpack_from('>I', cfa, j + 4)[0]
    ssrc = struct.unpack_from('>I', cfa, j + 8)[0]
    fcnt = cfa[j + 12]
    if (ts % 1280) == 0 and ssrc in (0, 0x01000000) and 1 <= fcnt <= 15:
        n_pkt += 1
        l2pay = cfa[j:j + 643]
        bad = []
        for k in range(fcnt):
            pos = 13 + k * FRAME_LEN
            if pos < len(l2pay) and l2pay[pos] != 0x9C:
                bad.append((k, pos, l2pay[pos]))
        if bad:
            seq = struct.unpack_from('>H', cfa, j + 2)[0]
            bad_pkts.append((j, seq, ts, ssrc, fcnt, bad))
    i = j + 2
print(f"audited {n_pkt} packets, {len(bad_pkts)} with bad internal sync")
for (j, seq, ts, ssrc, fcnt, bad) in bad_pkts[:15]:
    print(f"  off=0x{j:X} seq={seq} ts={ts} ssrc=0x{ssrc:08X} fcnt={fcnt} bad_frames={bad}")
