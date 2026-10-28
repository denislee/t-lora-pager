#!/usr/bin/env python3
"""Reference implementation of PagerLink v1 framing (protocol/PROTOCOL.md §2-§4)
and generator for the golden vectors in protocol/vectors/.

This is the oracle: the firmware (src/hal/link/link_frame.cpp) and the Swift
codec (ios/PagerKit) are tested against what it writes, never against each
other. Deterministic — re-running it must produce byte-identical files. It also
writes notes_crypto.json (OpenSSL `enc` format the firmware's
src/hal/notes_crypto.cpp uses) and git_blob.json (git blob SHA-1).

    python3 protocol/tools/mkvectors.py            # rewrite vectors
    python3 protocol/tools/mkvectors.py --check    # exit 1 if they are stale
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
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


# --- AES-256 (FIPS-197), only for the notes_crypto oracle ---


def gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


def xtime(a):
    return gmul(a, 2)


def _build_sbox():
    """GF(2^8) multiplicative inverse, then the FIPS-197 affine transform."""
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if gmul(a, b) == 1:
                inv[a] = b
                break
    out = []
    for a in range(256):
        b = inv[a]
        s = b
        for _ in range(4):
            b = ((b << 1) | (b >> 7)) & 0xFF
            s ^= b
        out.append(s ^ 0x63)
    return out


SBOX = _build_sbox()
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i
RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def aes256_expand_key(key):
    if len(key) != 32:
        raise ValueError("AES-256 key must be 32 bytes")
    words = [list(key[4 * i:4 * i + 4]) for i in range(8)]
    for i in range(8, 60):
        temp = list(words[i - 1])
        if i % 8 == 0:
            temp = temp[1:] + temp[:1]
            temp = [SBOX[x] for x in temp]
            temp[0] ^= RCON[i // 8 - 1]
        elif i % 8 == 4:
            temp = [SBOX[x] for x in temp]
        words.append([words[i - 8][j] ^ temp[j] for j in range(4)])
    return [bytes(b for w in words[4 * r:4 * r + 4] for b in w) for r in range(15)]


def _add_round_key(state, rk):
    return [state[i] ^ rk[i] for i in range(16)]


def _sub_bytes(state, box):
    return [box[x] for x in state]


def _shift_rows(state, inverse=False):
    out = [0] * 16
    for r in range(4):
        for c in range(4):
            src = (c - r) % 4 if inverse else (c + r) % 4
            out[r + 4 * c] = state[r + 4 * src]
    return out


def _mix_columns(state, inverse=False):
    m = [14, 11, 13, 9] if inverse else [2, 3, 1, 1]
    out = []
    for c in range(4):
        a = state[4 * c:4 * c + 4]
        for row in range(4):
            s = 0
            for k in range(4):
                s ^= gmul(a[k], m[(k - row) % 4])
            out.append(s)
    return out


def aes_encrypt_block(rk, block16):
    if len(block16) != 16:
        raise ValueError("block must be 16 bytes")
    state = _add_round_key(list(block16), rk[0])
    for rnd in range(1, 14):
        state = _add_round_key(_mix_columns(_shift_rows(_sub_bytes(state, SBOX))), rk[rnd])
    state = _add_round_key(_shift_rows(_sub_bytes(state, SBOX)), rk[14])
    return bytes(state)


def aes_decrypt_block(rk, block16):
    if len(block16) != 16:
        raise ValueError("block must be 16 bytes")
    state = _add_round_key(list(block16), rk[14])
    for rnd in range(13, 0, -1):
        state = _shift_rows(_sub_bytes(state, INV_SBOX), True)
        state = _add_round_key(state, rk[rnd])
        state = _mix_columns(state, True)
    state = _shift_rows(_sub_bytes(state, INV_SBOX), True)
    return bytes(_add_round_key(state, rk[0]))


def pkcs7_pad(data):
    n = 16 - (len(data) % 16)
    return data + bytes([n]) * n


def pkcs7_unpad(data):
    if not data:
        return None
    n = data[-1]
    if n == 0 or n > 16 or n > len(data):
        return None
    if data[-n:] != bytes([n]) * n:
        return None
    return data[:-n]


def aes256_cbc_encrypt(key, iv, plaintext):
    rk = aes256_expand_key(key)
    buf = pkcs7_pad(plaintext)
    out = bytearray()
    prev = iv
    for off in range(0, len(buf), 16):
        blk = bytes(a ^ b for a, b in zip(buf[off:off + 16], prev))
        prev = aes_encrypt_block(rk, blk)
        out += prev
    return bytes(out)


def aes256_cbc_decrypt_raw(key, iv, ciphertext):
    rk = aes256_expand_key(key)
    out = bytearray()
    prev = iv
    for off in range(0, len(ciphertext), 16):
        blk = ciphertext[off:off + 16]
        out += bytes(a ^ b for a, b in zip(aes_decrypt_block(rk, blk), prev))
        prev = blk
    return bytes(out)


# FIPS-197 Appendix C.3 known answer.
_FIPS_KEY = bytes(range(32))
_FIPS_IN = bytes.fromhex("00112233445566778899aabbccddeeff")
_FIPS_OUT = bytes.fromhex("8ea2b7ca516745bfeafc49904b496089")
_FIPS_RK = aes256_expand_key(_FIPS_KEY)
assert len(_FIPS_RK) == 15 and all(len(k) == 16 for k in _FIPS_RK)
assert aes_encrypt_block(_FIPS_RK, _FIPS_IN) == _FIPS_OUT
assert aes_decrypt_block(_FIPS_RK, _FIPS_OUT) == _FIPS_IN


NOTES_ITER = 10000
NOTES_MAGIC = b"Salted__"


def notes_derive(passphrase, salt):
    dk = hashlib.pbkdf2_hmac("sha256", passphrase.encode("utf-8"), salt, NOTES_ITER, 48)
    return dk[:32], dk[32:48]


def notes_encrypt(passphrase, salt, plaintext):
    if len(salt) != 8:
        raise ValueError("salt must be 8 bytes")
    key, iv = notes_derive(passphrase, salt)
    return NOTES_MAGIC + salt + aes256_cbc_encrypt(key, iv, plaintext)


def notes_decrypt_detail(passphrase, data):
    """Returns (plaintext, None) or (None, reason)."""
    if data is None or len(data) < 16:
        return None, "short"
    if not data.startswith(NOTES_MAGIC):
        return None, "header"
    salt, body = data[8:16], data[16:]
    if not body:
        return None, "short"
    if len(body) % 16:
        return None, "length"
    key, iv = notes_derive(passphrase, salt)
    plain = pkcs7_unpad(aes256_cbc_decrypt_raw(key, iv, body))
    if plain is None:
        return None, "padding"
    return plain, None


def notes_decrypt(passphrase, data):
    return notes_decrypt_detail(passphrase, data)[0]


def notes_crypto_vectors():
    cases = [
        ("empty", "hunter2", "0102030405060708", b""),
        ("short", "hunter2", "a1a2a3a4a5a6a7a8", b"hi\n"),
        ("exact_block", "correct horse battery staple", "0011223344556677",
         b"0123456789abcdef"),
        ("multi_block", "correct horse battery staple", "deadbeefcafef00d",
         b"Shopping list\n- milk\n- eggs\n- coffee beans\n- batteries (AA)\n\n"
         b"Call the plumber on Tuesday.\n"),
        ("utf8_non_ascii", "p\u00e4ssw\u00f6rd \u2713", "f0e1d2c3b4a59687",
         "\u041f\u0440\u0438\u0432\u0435\u0442, caf\u00e9 \u2014 \u65e5\u672c\u8a9e \u2713\nsecond line\n".encode("utf-8")),
    ]
    valid = []
    for name, passphrase, salt_hex, plaintext in cases:
        salt = bytes.fromhex(salt_hex)
        key, iv = notes_derive(passphrase, salt)
        ct = notes_encrypt(passphrase, salt, plaintext)
        assert notes_decrypt(passphrase, ct) == plaintext, name
        valid.append({"name": name, "passphrase": passphrase, "salt_hex": salt_hex,
                      "key_hex": key.hex(), "iv_hex": iv.hex(),
                      "plaintext_hex": plaintext.hex(), "ciphertext_hex": ct.hex()})

    multi = next(v for v in valid if v["name"] == "multi_block")
    multi_ct = bytes.fromhex(multi["ciphertext_hex"])
    multi_pt = bytes.fromhex(multi["plaintext_hex"])
    wrong_passphrase, wrong = [], None
    for cand in ["hunter3", "hunter4", "hunter5", "hunter6", "hunter7"]:
        if notes_decrypt(cand, multi_ct) is None:
            wrong = cand
            break
    assert wrong is not None, "no wrong passphrase produced invalid padding"
    wrong_passphrase.append({"name": "multi_block_wrong", "passphrase": wrong,
                             "ciphertext_hex": multi_ct.hex()})

    malformed = []
    for name, data, reason in [
        ("empty", b"", "short"),
        ("short_header", b"Salted__1234", "short"),
        ("missing_magic", b"NotSalted1234567" + bytes(16), "header"),
        ("header_only", b"Salted__" + bytes.fromhex("0102030405060708"), "short"),
        ("not_block_multiple", b"Salted__" + bytes.fromhex("0102030405060708") + bytes(15), "length"),
    ]:
        _, got = notes_decrypt_detail("hunter2", data)
        assert notes_decrypt("hunter2", data) is None, name
        assert got == reason, (name, got, reason)
        malformed.append({"name": name, "bytes_hex": data.hex(), "reason": reason})

    return {"iterations": NOTES_ITER, "valid": valid,
            "wrong_passphrase": wrong_passphrase, "malformed": malformed}


# --- git blob hashing ---


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def git_blob_vectors():
    multi = next(v for v in notes_crypto_vectors()["valid"] if v["name"] == "multi_block")
    blobs = []
    for name, data in [
        ("empty", b""),
        ("hello", b"hello\n"),
        ("utf8", "caf\u00e9 \u2713\n".encode("utf-8")),
        ("salted", bytes.fromhex(multi["ciphertext_hex"])),
        ("pattern_5000", pattern(5000, 3)),
    ]:
        blobs.append({"name": name, "bytes_hex": data.hex(), "sha1": git_blob_sha1(data)})
    assert blobs[0]["sha1"] == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"
    assert blobs[1]["sha1"] == "ce013625030ba8dba906f756967f9e9ca394464a"
    return {"blobs": blobs}


# --- settings.* (PROTOCOL.md §6.4) -------------------------------------------
# A small fixed catalog and the exact compact JSON the pager's registry
# (src/hal/settings_registry.cpp) answers for it. test_settings_registry builds
# a registry from "catalog" and checks every case byte for byte; the Go and
# Swift clients decode the same results.

def _compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True)


SETTINGS_REV0 = 100

SETTINGS_GROUPS = [
    {"id": "display", "title": "Display & Backlight"},
    {"id": "storage", "title": "Storage"},
    {"id": "nsync", "title": "Notes Sync"},
    {"id": "wifi", "title": "Wi-Fi Networks"},
]

# Schema items in registry encoding order (settings_registry.cpp encode_item).
SETTINGS_ITEMS = [
    {"id": "display.brightness", "group": "display", "title": "Screen brightness",
     "type": "int", "min": 0, "max": 16, "step": 1, "apply": "live"},
    {"id": "display.timeout_s", "group": "display", "title": "Screen timeout (s)",
     "type": "int", "min": 0, "max": 180, "step": 1, "apply": "tick", "hint": "0 = never"},
    {"id": "display.sleep_mode", "group": "display", "title": "Sleep mode", "type": "enum",
     "enum": [{"v": "light", "label": "Light"}, {"v": "deep", "label": "Deep"}],
     "apply": "sleep"},
    {"id": "storage.msc_target", "group": "storage", "title": "USB drive shows", "type": "enum",
     "enum": [{"v": "sd", "label": "SD card"}, {"v": "int", "label": "Internal flash"}],
     "apply": "reboot"},
    {"id": "nsync.token", "group": "nsync", "title": "GitHub token (PAT)", "type": "secret",
     "max_len": 255, "apply": "live", "secret": True},
    {"id": "wifi.saved", "group": "wifi", "title": "Saved networks", "type": "list",
     "apply": "live", "ro": True},
]

SETTINGS_ACTIONS = [
    {"id": "wifi.scan", "group": "wifi", "title": "Scan for networks"},
]

# Values before any case runs (a secret's initial value is whether it is set).
SETTINGS_INITIAL = {
    "display.brightness": 8,
    "display.timeout_s": 60,
    "display.sleep_mode": "light",
    "storage.msc_target": "sd",
    "nsync.token": False,
    "wifi.saved": ["home"],
}


def settings_rev_schema():
    h = 2166136261
    def mix(b):
        nonlocal h
        for c in b:
            h ^= c
            h = (h * 16777619) & 0xFFFFFFFF
    for g in SETTINGS_GROUPS:
        mix((g["id"] + "\x1f" + g["title"] + "\x1e").encode())
    for it in SETTINGS_ITEMS:
        mix(_compact(it).encode())
    for a in SETTINGS_ACTIONS:
        mix(_compact(a).encode())
    return "%08x" % h


def settings_vectors():
    rs = settings_rev_schema()
    def wire_value(id_, v):
        return {"set": v} if id_ == "nsync.token" else v

    cases = []
    def ok(name, method, params, result):
        cases.append({"name": name, "method": method, "params": params, "result": result,
                      "result_json": _compact(result)})
    def err(name, method, params, code, msg):
        cases.append({"name": name, "method": method, "params": params,
                      "error": {"code": code, "msg": msg}})

    # Each case runs on a fresh registry built from the catalog + initial values.
    ok("schema_first_page", "settings.schema", {},
       {"rev_schema": rs, "groups": SETTINGS_GROUPS, "actions": SETTINGS_ACTIONS,
        "items": SETTINGS_ITEMS})
    ok("schema_cursor", "settings.schema", {"cursor": "4"},
       {"rev_schema": rs, "items": SETTINGS_ITEMS[4:]})
    err("schema_bad_cursor", "settings.schema", {"cursor": "x"}, "invalid_argument", "bad cursor")
    ok("get_all", "settings.get", {},
       {"rev": SETTINGS_REV0, "rev_schema": rs,
        "values": {k: wire_value(k, v) for k, v in SETTINGS_INITIAL.items()}})
    ok("get_with_secret", "settings.get", {"ids": ["display.brightness", "nsync.token"]},
       {"rev": SETTINGS_REV0, "rev_schema": rs,
        "values": {"display.brightness": 8, "nsync.token": {"set": False}}})
    err("get_unknown", "settings.get", {"ids": ["nope.x"]}, "invalid_argument",
        "nope.x: unknown setting")
    ok("set_ok", "settings.set", {"values": {"display.brightness": 5}, "if_rev": SETTINGS_REV0},
       {"rev": SETTINGS_REV0 + 1, "applied": ["display.brightness"], "reboot": False})
    ok("set_unchanged", "settings.set", {"values": {"display.brightness": 8}},
       {"rev": SETTINGS_REV0, "applied": [], "reboot": False})
    ok("set_two", "settings.set",
       {"values": {"display.brightness": 4, "display.sleep_mode": "deep"}},
       {"rev": SETTINGS_REV0 + 1, "applied": ["display.brightness", "display.sleep_mode"],
        "reboot": False})
    ok("set_reboot", "settings.set", {"values": {"storage.msc_target": "int"}},
       {"rev": SETTINGS_REV0 + 1, "applied": ["storage.msc_target"], "reboot": True})
    ok("set_secret", "settings.set", {"values": {"nsync.token": "ghp_example"}},
       {"rev": SETTINGS_REV0 + 1, "applied": ["nsync.token"], "reboot": False})
    err("set_one_bad", "settings.set",
        {"values": {"display.brightness": 5, "display.timeout_s": 999}},
        "invalid_argument", "display.timeout_s: out of range")
    err("set_stale_rev", "settings.set",
        {"values": {"display.brightness": 5}, "if_rev": SETTINGS_REV0 - 1},
        "invalid_argument", "changed")
    err("set_bad_enum", "settings.set", {"values": {"display.sleep_mode": "nap"}},
        "invalid_argument", "display.sleep_mode: not one of the choices")
    err("set_read_only", "settings.set", {"values": {"wifi.saved": []}},
        "invalid_argument", "wifi.saved: read-only")
    err("set_wrong_type", "settings.set", {"values": {"display.brightness": "5"}},
        "invalid_argument", "display.brightness: expected integer")
    err("set_empty", "settings.set", {"values": {}}, "invalid_argument", "values is empty")

    events = [
        {"name": "changed", "e": "evt.settings.changed",
         "p": {"rev": SETTINGS_REV0 + 1, "ids": ["display.brightness"]}},
        {"name": "job_scan", "e": "evt.settings.job",
         "p": {"job": "j1", "done": True, "ok": True, "msg": "ok",
               "result": [{"ssid": "home", "rssi": -50, "secure": True}]}},
        {"name": "job_failed", "e": "evt.settings.job",
         "p": {"job": "j2", "done": True, "ok": False, "msg": "NTP timed out"}},
    ]
    for e in events:
        e["p_json"] = _compact(e["p"])

    return {
        "rev0": SETTINGS_REV0,
        "catalog": {"groups": SETTINGS_GROUPS, "items": SETTINGS_ITEMS,
                    "actions": SETTINGS_ACTIONS, "initial": SETTINGS_INITIAL},
        "rev_schema": rs,
        "cases": cases,
        "events": events,
        "action_job": {"params": {"id": "wifi.scan"}, "result": {"job": "j1"}},
    }


def _openssl_decrypt(openssl, iterations, passphrase, data):
    return subprocess.run(
        [openssl, "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", str(iterations),
         "-md", "sha256", "-pass", "env:MKV_PASS"],
        input=data, env=dict(os.environ, MKV_PASS=passphrase),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def cross_check(rendered):
    notes = rendered["notes_crypto.json"]
    blobs = rendered["git_blob.json"]["blobs"]
    multi_pt = next(bytes.fromhex(v["plaintext_hex"]) for v in notes["valid"]
                    if v["name"] == "multi_block")
    ok = True

    openssl = shutil.which("openssl")
    if openssl:
        for v in notes["valid"]:
            want = bytes.fromhex(v["plaintext_hex"])
            proc = _openssl_decrypt(openssl, notes["iterations"], v["passphrase"],
                                    bytes.fromhex(v["ciphertext_hex"]))
            if proc.returncode != 0 or proc.stdout != want:
                print("cross_check: openssl mismatch for notes_crypto/%s (rc=%d, got=%s, want=%s)"
                      % (v["name"], proc.returncode, proc.stdout.hex(), want.hex()),
                      file=sys.stderr)
                ok = False
        for v in notes["wrong_passphrase"]:
            proc = _openssl_decrypt(openssl, notes["iterations"], v["passphrase"],
                                    bytes.fromhex(v["ciphertext_hex"]))
            if proc.returncode == 0 and proc.stdout == multi_pt:
                print("cross_check: openssl accepted wrong passphrase for notes_crypto/%s"
                      % v["name"], file=sys.stderr)
                ok = False

    git = shutil.which("git")
    if git:
        for b in blobs:
            proc = subprocess.run([git, "hash-object", "--stdin"],
                                  input=bytes.fromhex(b["bytes_hex"]),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            got = proc.stdout.decode("ascii", "replace").strip()
            if proc.returncode != 0 or got != b["sha1"]:
                print("cross_check: git mismatch for git_blob/%s (rc=%d, got=%r, want=%s)"
                      % (b["name"], proc.returncode, got, b["sha1"]), file=sys.stderr)
                ok = False

    return ok


def render():
    return {
        "message.json": message_vectors(),
        "segment.json": segment_vectors(),
        "reassemble.json": reassemble_vectors(),
        "bulk.json": bulk_vectors(),
        "notes_crypto.json": notes_crypto_vectors(),
        "git_blob.json": git_blob_vectors(),
        "settings.json": settings_vectors(),
    }


def main():
    vdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vectors")
    os.makedirs(vdir, exist_ok=True)
    stale = []
    objs = render()
    for fname, obj in objs.items():
        text = json.dumps(obj, indent=1, sort_keys=True) + "\n"
        path = os.path.join(vdir, fname)
        cur = open(path).read() if os.path.exists(path) else None
        if cur != text:
            stale.append(fname)
            if "--check" not in sys.argv:
                with open(path, "w") as f:
                    f.write(text)
    if not cross_check(objs):
        return 1
    if "--check" in sys.argv and stale:
        print("stale vectors: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
