# PagerLink v1 — protocol specification

Normative spec for the link between the pager firmware (`src/hal/link/`) and
the iOS companion (`ios/PagerKit`). Design rationale lives in
`IOS_COMPANION_PLAN.md` §4; this file is the contract. Both implementations
are tested against the golden vectors in `vectors/*.json` — a change here
lands together with the firmware codec, the Swift codec and the vectors, in
one commit.

All multi-byte integers are **little-endian**.

## 1. GATT surface (BLE transport)

| Item | UUID | Properties |
|---|---|---|
| Service `PagerLink` | `5BFF3035-4B08-4B35-BEEB-6A426D451D47` | primary; UUID in the scan response |
| `rx` phone → pager | `02855CCC-F4A4-4AA7-AFDC-237158247D4D` | Write, Write Without Response; encrypted + authenticated |
| `tx` pager → phone | `A975E68C-192B-4364-BE4E-6BB414A20346` | Notify; encrypted + authenticated |
| `info` | `D7B0CF7E-0B2B-4CAC-BB23-4C0E20464B42` | Read, unencrypted; JSON `{"proto":1,"fw":"<hash>","board":"pager","l2cap_psm":0}` |

These UUIDs are frozen. Constants: `src/hal/link/link_uuids.h`,
`ios/PagerKit/Sources/PagerKit/Link/UUIDs.swift`.

## 2. Link layer — packets

One ATT write / notification (or one TCP record, §5) carries one packet:

```
byte 0    flags: bit7 START, bit6 END, bits5..0 seq
byte 1..  message bytes (a slice of one message)
```

- **Packet size** `P` = negotiated `ATT_MTU − 3` (minimum 20). Each packet
  carries at most `P − 1` message bytes. A packet with zero message bytes is
  invalid.
- **Segmentation:** a message is cut into `ceil(len / (P−1))` packets in
  order. The first has START, the last has END; a one-packet message has
  both.
- **Sequence:** each direction keeps one rolling counter, `seq`, starting at
  `0` when the link (re)connects and incremented by one (mod 64) for every
  packet sent, across message boundaries.
- **Reassembly:** the receiver tracks `expected_seq` (starts at 0) and a
  partial message buffer. For each packet:
  1. Empty packet (0 bytes or only the flags byte) → **reset** `empty`;
     the packet is dropped; `expected_seq` is unchanged.
  2. `seq != expected_seq` → **reset** `seq_gap`; the partial message is
     dropped and the receiver enters *discard* state.
  3. `expected_seq = (seq + 1) mod 64`.
  4. START → leave discard state. If a partial message is open → **reset**
     `unexpected_start` and drop it. Start a new partial with this packet.
  5. No START and no partial open → drop the packet; **reset**
     `unexpected_continue` unless in discard state (one reset per fault,
     not one per remaining packet).
  6. Append the bytes. If the partial would exceed `MAX_MESSAGE` (4102 =
     6 + 4096) → **reset** `oversize`; drop the partial; enter discard
     state.
  7. END → deliver the partial as a complete message.
- A reset is reported to the owner (logged; the RPC layer raises the
  `link.reset` event locally). Outstanding requests are **not** failed by a
  reset — they time out.

## 3. Message layer

```
offset size field
0      u8   ver      = 1
1      u8   type     0 REQ · 1 RES · 2 ERR · 3 EVT · 4 DATA · 5 ACK
2      u16  id       request id (REQ/RES/ERR), transfer id (DATA/ACK), 0 for EVT
4      u16  len      payload length, ≤ 4096
6      ...  payload  exactly `len` bytes
```

A message is **malformed** (dropped, logged, no reply) when: it is shorter
than 6 bytes, `ver != 1`, `type > 5`, `len > 4096`, or `6 + len` differs from
the reassembled length.

### 3.1 Payloads

| type | payload |
|---|---|
| REQ | UTF-8 JSON object `{"m":"<method>","p":<params>}`; `p` is optional (absent ≡ `{}`) |
| RES | UTF-8 JSON value — the result (an object, or `{}`) |
| ERR | UTF-8 JSON object `{"code":"<code>","msg":"<text>"}` |
| EVT | UTF-8 JSON object `{"e":"<event>","p":<params>}` |
| DATA | `u32 offset` + raw bytes (≤ 4092) |
| ACK | `u32 next_expected_offset` + `u16 credits` (exactly 6 bytes) |

### 3.2 Request ids

- The phone allocates **even** ids, the pager **odd** ids. Neither uses 0.
- Each side allocates sequentially and wraps within its parity: phone
  `2, 4, …, 65534, 2, …`; pager `1, 3, …, 65535, 1, …`. An id still
  outstanding is skipped.
- A RES/ERR whose id has no outstanding request (late, duplicate, or never
  sent) is dropped and logged.
- A REQ with the wrong parity for its sender is answered with ERR
  `invalid_argument`.

### 3.3 Timeouts

Default 10 s per request, measured by the caller. Longer for named
operations (`fs.write.commit`, `ota.*`: 60 s). On timeout the caller fails
the request locally with `timeout`; a later RES is dropped as late.

### 3.4 Error codes

`code` is one of the following strings. The first group maps one-to-one onto
`HalError` (`src/hal/result.h`):

| code | HalError |
|---|---|
| `invalid_argument` | `InvalidArgument` |
| `unsupported` | `NotSupported` (also: unknown method) |
| `internal` | `InternalError` |
| `storage_full` | `StorageFull` |
| `wifi_offline` | `WifiOffline` |
| `http_error` | `HttpError` |
| `unauthorized` | `Unauthorized` |
| `hub_disabled` | `HubDisabled` |
| `hub_unreachable` | `HubUnreachable` |

Link-only codes: `bad_request` (REQ payload is not a JSON object with a
string `m`), `busy` (the handler can't accept work now; retry later),
`timeout` and `disconnected` (local only, never sent on the wire:
the caller's request expired, or the link dropped while it was
outstanding).

Receivers must treat an unknown `code` as `internal`.

## 4. Bulk transfer

Started by an RPC (`fs.read` / `fs.write.begin`) that returns
`{"xfer":<id>, …}`. Transfer ids are allocated by the side that will
**receive** the DATA with the same parity rule as request ids, and are
independent of request ids.

- The receiver grants credits with ACK. The initial grant is implicit:
  **16** packets. The sender keeps at most `credits` DATA messages
  unacknowledged.
- `ACK.next_expected_offset` acknowledges every byte below it.
  `ACK.credits` replaces (not adds to) the sender's window, counting from
  that offset.
- DATA offsets must be contiguous. A DATA message with an unexpected offset
  is dropped and the receiver re-ACKs its current `next_expected_offset`.
- Completion and integrity (CRC-32/ISO-HDLC over the whole file) are
  confirmed by the operation's own RPC (`fs.write.commit`) or, for reads,
  by the receiver once `size` bytes are in.

## 5. TCP transport (emulator)

The emulator listens on `127.0.0.1:7600` (override with env
`PAGERLINK_PORT`). One client at a time. Each packet (§2, flags byte
included) is sent as a record:

```
u16 length   number of packet bytes that follow (1..65535)
...          packet
```

The packet size `P` over TCP is fixed at **244** (equivalent to MTU 247).
Both sides reset `seq` to 0 on each new TCP connection.

## 6. Methods — Phase 1

| Method / event | Dir | Params → result |
|---|---|---|
| `sys.hello` | P→D | `{"proto":1,"app":"<ver>","caps":[…]}` → `{"proto":1,"fw":"<hash>","board":"pager","name":"lilygo-pager","caps":[…],"mtu":<P+3>}`. A `proto` major mismatch → ERR `unsupported` |
| `sys.info` | P→D | → `{"batt_pct","charging","uptime_s","wifi":{"on","connected","ssid","rssi"},"hub":{"on","reachable"},"storage":{"int":{"t","u"},"sd":{"t","u","mounted"}}}` (sizes in bytes) |
| `sys.ping` | both | `{"t":<n>}` → `{"t":<n>}` (echo) |
| `sys.time.set` | P→D | `{"epoch":<unix s>,"tz":"<IANA>"}` → `{}` |
| `evt.battery` | D→P | `{"pct","charging"}` on change ≥ 5 % or charge-state flip |

The rest of the catalog (fs.*, weather.*, notify.*, …) is specified in
`IOS_COMPANION_PLAN.md` §4.5 and moves here as each phase lands. A side
that doesn't implement a method answers ERR `unsupported`; `caps` lists the
method families each side implements (`"sys"`, `"fs"`, `"weather"`, …).

## 7. Golden vectors

`vectors/*.json` — consumed by `test/test_link_frame` (via
`support/gen_link_vectors.py`, which turns them into a C header at test-build
time) and by `ios/PagerKit/Tests/PagerKitTests`. Byte strings are lowercase
hex with no separators. Files:

- `message.json` — header encode/decode, valid and malformed.
- `segment.json` — segmentation of one message at packet sizes 20 / 182 / 244.
- `reassemble.json` — packet sequences → delivered messages and resets.
- `bulk.json` — DATA / ACK payload encode/decode.
