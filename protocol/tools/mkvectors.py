#!/usr/bin/env python3
"""Reference implementation of PagerLink v1 framing (protocol/PROTOCOL.md §2-§4)
and generator for the golden vectors in protocol/vectors/.

This is the oracle: the firmware (src/hal/link/link_frame.cpp) and the Swift
codec (ios/PagerKit) are tested against what it writes, never against each
other. Deterministic — re-running it must produce byte-identical files.

    python3 protocol/tools/mkvectors.py            # rewrite vectors
    python3 protocol/tools/mkvectors.py --check    # exit 1 if they are stale
"""
import json
import os
import struct
import sys
import zlib

VER = 1
HEADER = 6
MAX_PAYLOAD = 4096
MAX_MESSAGE = HEADER + MAX_PAYLOAD
START, END = 0x80, 0x40
TYPES = {"req": 0, "res": 1, "err": 2, "evt": 3, "data": 4, "ack": 5}


def encode_message(mtype, mid, payload):
    return struct.pack("<BBHH", VER, mtype, mid, len(payload)) + payload


def decode_message(b):
    if len(b) < HEADER:
        return None, "short"
    ver, mtype, mid, ln = struct.unpack_from("<BBHH", b)
    if ver != VER:
        return None, "version"
    if mtype > 5:
        return None, "type"
    if ln > MAX_PAYLOAD:
        return None, "oversize"
    if HEADER + ln != len(b):
        return None, "length"
    return (mtype, mid, b[HEADER:]), None


def segment(msg, packet_size, seq):
    """Returns (packets, next_seq)."""
    room = packet_size - 1
    out = []
    for off in range(0, len(msg), room):
        flags = seq & 0x3F
        if off == 0:
            flags |= START
        if off + room >= len(msg):
            flags |= END
        out.append(bytes([flags]) + msg[off:off + room])
        seq = (seq + 1) & 0x3F
    return out, seq


class Reassembler:
    def __init__(self):
        self.expected = 0
        self.partial = None
        self.discarding = False  # after seq_gap/oversize: drop non-START silently
        self.events = []

    def reset(self, reason):
        self.events.append({"kind": "reset", "reason": reason})

    def feed(self, pkt):
        if len(pkt) < 2:
            self.reset("empty")
            return
        flags, data = pkt[0], pkt[1:]
        seq = flags & 0x3F
        if seq != self.expected:
            self.reset("seq_gap")
            self.partial = None
            self.discarding = True
        self.expected = (seq + 1) & 0x3F
        if flags & START:
            self.discarding = False
            if self.partial is not None:
                self.reset("unexpected_start")
            self.partial = bytearray()
        elif self.partial is None:
            if not self.discarding:
                self.reset("unexpected_continue")
            return
        if len(self.partial) + len(data) > MAX_MESSAGE:
            self.reset("oversize")
            self.partial = None
            self.discarding = True
            return
        self.partial += data
        if flags & END:
            self.events.append({"kind": "message", "hex": bytes(self.partial).hex()})
            self.partial = None


def pattern(n, salt=0):
    return bytes((i * 7 + salt) & 0xFF for i in range(n))


def json_msg(mtype, mid, obj):
    return encode_message(mtype, mid, json.dumps(obj, separators=(",", ":")).encode())


def message_vectors():
    valid = []
    for name, mtype, mid, payload in [
        ("req_ping", TYPES["req"], 2, b'{"m":"sys.ping","p":{"t":42}}'),
        ("res_ping", TYPES["res"], 2, b'{"t":42}'),
        ("err_unsupported", TYPES["err"], 65534, b'{"code":"unsupported","msg":"no such method"}'),
        ("evt_battery", TYPES["evt"], 0, b'{"e":"evt.battery","p":{"pct":80,"charging":true}}'),
        ("req_empty_payload", TYPES["req"], 1, b""),
        ("data_max", TYPES["data"], 0x1234, struct.pack("<I", 0) + pattern(MAX_PAYLOAD - 4)),
        ("ack", TYPES["ack"], 7, struct.pack("<IH", 4096, 16)),
        ("id_max", TYPES["res"], 0xFFFF, b"{}"),
    ]:
        valid.append({"name": name, "type": mtype, "id": mid, "payload_hex": payload.hex(),
                      "bytes_hex": encode_message(mtype, mid, payload).hex()})
    good = encode_message(0, 2, b'{"m":"x"}')
    malformed = [
        ("empty", b""),
        ("short", good[:5]),
        ("header_only_len_mismatch", good[:6]),
        ("bad_version", bytes([2]) + good[1:]),
        ("bad_type", good[:1] + bytes([6]) + good[2:]),
        ("payload_too_long", struct.pack("<BBHH", 1, 4, 1, MAX_PAYLOAD + 1) + pattern(MAX_PAYLOAD + 1)),
        ("trailing_bytes", good + b"x"),
        ("truncated_payload", good[:-1]),
    ]
    out = []
    for name, b in malformed:
        _, reason = decode_message(b)
        assert reason, name
        out.append({"name": name, "bytes_hex": b.hex(), "reason": reason})
    return {"valid": valid, "malformed": out}


def segment_vectors():
    cases = []
    msgs = [
        ("tiny", json_msg(0, 2, {"m": "sys.ping", "p": {"t": 1}})),
        ("exact_one_packet_19", encode_message(1, 4, pattern(13))),   # 19 bytes = 1 packet at P=20
        ("one_over_19", encode_message(1, 4, pattern(14))),           # 20 bytes = 2 packets at P=20
        ("medium", encode_message(1, 6, pattern(500, 3))),
        ("max", encode_message(4, 9, pattern(MAX_PAYLOAD, 5))),
    ]
    for psize in (20, 182, 244):
        for mname, msg in msgs:
            for start in (0, 62):
                pkts, nxt = segment(msg, psize, start)
                cases.append({"name": f"{mname}_p{psize}_s{start}", "packet_size": psize,
                              "start_seq": start, "message_hex": msg.hex(),
                              "packets_hex": [p.hex() for p in pkts], "next_seq": nxt})
    return cases


def reassemble_vectors():
    m1 = json_msg(0, 2, {"m": "sys.ping", "p": {"t": 7}})
    m2 = encode_message(1, 2, pattern(300, 1))
    m3 = encode_message(3, 0, b'{"e":"evt.battery","p":{"pct":5,"charging":false}}')
    big = encode_message(4, 3, pattern(MAX_PAYLOAD, 9))
    P = 20

    def seg(msg, seq, size=P):
        return segment(msg, size, seq)[0]

    cases = {}
    cases["single"] = seg(m3, 0, 244)
    cases["multi_in_order"] = seg(m2, 0)
    a, s = segment(m1, P, 0)
    b, _ = segment(m2, P, s)
    cases["back_to_back"] = a + b
    p = seg(m2, 0)
    cases["gap_mid_message_then_continue"] = p[:3] + p[4:]  # rest of m2 dropped silently
    q = seg(m3, 9)
    cases["gap_then_start"] = p[:3] + q
    s1 = seg(m2, 0)
    s2 = seg(m1, 3)
    cases["start_while_open"] = s1[:3] + s2
    cases["continue_without_start"] = [bytes([0x00]) + b"abc"] + seg(m1, 1)
    cases["empty_packet"] = [b"", bytes([0x00])] + seg(m1, 0)
    # Advance to seq 60 with filler messages, then a message that crosses 63->0.
    filler, seq = [], 0
    while seq != 60:
        pk, seq = segment(m1, 244, seq)
        filler += pk
    cases["wrap_63_to_0"] = filler + seg(m2, 60)
    # Oversize: a 4103-byte reassembly. Only the last packet pushes it over.
    over = big + b"\x00"
    cases["oversize"] = seg(over, 0, 244)
    cases["max_message_ok"] = seg(big, 0, 244)

    out = []
    for name, pkts in cases.items():
        r = Reassembler()
        for pk in pkts:
            r.feed(pk)
        out.append({"name": name, "packets_hex": [pk.hex() for pk in pkts], "events": r.events})
    return out


def bulk_vectors():
    data = [{"offset": off, "bytes_hex": pattern(n, 2).hex(),
             "payload_hex": (struct.pack("<I", off) + pattern(n, 2)).hex()}
            for off, n in [(0, 0), (0, 10), (4092, 100), (0xFFFFFFFF, 3), (12345, MAX_PAYLOAD - 4)]]
    ack = [{"next": nxt, "credits": c, "payload_hex": struct.pack("<IH", nxt, c).hex()}
           for nxt, c in [(0, 16), (4096, 16), (0xFFFFFFFF, 0), (1, 65535)]]
    bad_data = [{"name": "short", "payload_hex": "010203"}]
    bad_ack = [{"name": "short", "payload_hex": "0000000010"},
               {"name": "long", "payload_hex": "00000000100000"}]
    crc32 = [{"name": name, "input_hex": buf.hex(), "crc32": zlib.crc32(buf) & 0xFFFFFFFF}
             for name, buf in [("empty", b""), ("check", b"123456789"), ("one_byte", b"a"),
                               ("pattern_4092", pattern(4092, 2)),
                               ("pattern_10000", pattern(10000, 5))]]
    return {"data": data, "ack": ack, "bad_data": bad_data, "bad_ack": bad_ack,
            "crc32": crc32}


def render():
    return {
        "message.json": message_vectors(),
        "segment.json": segment_vectors(),
        "reassemble.json": reassemble_vectors(),
        "bulk.json": bulk_vectors(),
    }


def main():
    vdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vectors")
    os.makedirs(vdir, exist_ok=True)
    stale = []
    for fname, obj in render().items():
        text = json.dumps(obj, indent=1, sort_keys=True) + "\n"
        path = os.path.join(vdir, fname)
        cur = open(path).read() if os.path.exists(path) else None
        if cur != text:
            stale.append(fname)
            if "--check" not in sys.argv:
                with open(path, "w") as f:
                    f.write(text)
    if "--check" in sys.argv and stale:
        print("stale vectors: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
