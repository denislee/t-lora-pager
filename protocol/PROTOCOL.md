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

Link-only codes: `bad_request` (REQ payload is not a JSON object with a
string `m`), `busy` (the handler can't accept work now; retry later),
`timeout` and `disconnected` (local only, never sent on the wire:
the caller's request expired, or the link dropped while it was
outstanding; `HalError::LinkOffline` maps to `disconnected`).

Receivers must treat an unknown `code` as `internal`.

## 4. Bulk transfer

Started by an RPC (`fs.read` / `fs.write.begin`) that returns
`{"xfer":<id>, …}`. Transfer ids are allocated by the side that will
**receive** the DATA with the same parity rule as request ids, and are
independent of request ids.

A pager request whose answer can exceed a RES payload (§3: 4096 B) may carry
a receiver-allocated id: the pager allocates an odd `xfer` before the call
and puts it in the request params (`tg.chats`, `tg.messages`). The phone
either answers inline, or answers `{"xfer","size","crc32"}` with that same
id and streams the body as DATA, which may overtake the RES on the wire (the
pager buffers it). The pager checks size and CRC once `size` bytes are in,
and drops the receiver when the call ends.

- The receiver grants credits with ACK. The initial grant is implicit:
  **16** DATA messages (not packets: a DATA message carries up to 4092 B).
  The sender keeps at most `credits` DATA messages unacknowledged. A receiver
  may grant fewer (the pager sizes its grant to its BLE receive queue) and
  should then ACK often enough that the window never runs dry.
- `ACK.next_expected_offset` acknowledges every byte below it.
  `ACK.credits` replaces (not adds to) the sender's window, counting from
  that offset.
- DATA offsets must be contiguous. A DATA message with an unexpected offset
  is dropped. The receiver re-ACKs its current `next_expected_offset` once
  per gap: it does not re-ACK again until in-order DATA has arrived. Once
  every byte is in, it re-ACKs the final offset for any further DATA, which
  covers a lost final ACK.
- An ACK that makes no progress while DATA is unacknowledged rewinds the
  sender to that offset (go-back-N), at most once per acknowledged offset.
  An ACK above the current send point (it overtook a rewind) moves the send
  point forward.
- Retransmit timeout: a sender that has seen no ACK for **2 s** while bytes
  are unacknowledged rewinds to the acknowledged offset and resends. This
  recovers a lost last DATA, a lost re-ACK or a lost final ACK. Timer
  granularity is up to the implementation (the pager checks once per second).
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
| `sys.info` | P→D | → `{"batt_pct","charging","uptime_s","wifi":{"on","connected","ssid","rssi"},"storage":{"int":{"t","u"},"sd":{"t","u","mounted"}}}` (sizes in bytes) |
| `sys.ping` | both | `{"t":<n>}` → `{"t":<n>}` (echo) |
| `sys.time.set` | P→D | `{"epoch":<unix s>,"tz":"<IANA>"}` → `{}` |
| `evt.battery` | D→P | `{"pct","charging"}` on change ≥ 5 % or charge-state flip |
| `chat.audio.begin` | D→P | `{"size","crc32","fmt":"wav"}` → `{"xfer"}`. Announces a voice memo (16 kHz/16-bit mono WAV, at most 15 s); the phone receives, so it allocates `xfer` (§4), then the pager streams the bytes as DATA. A CRC mismatch surfaces on the following `chat.ask` as `invalid_argument` |
| `chat.ask` | D→P | `{"text"?,"audio_xfer"?,"reset"?}` → `{"transcript"?,"reply"}`. `audio_xfer` names a finished `chat.audio.begin` upload; `transcript` is set for voice. The phone keeps per-pager history (`reset` clears it). Timeout 60 s |
| `notes.sync` | D→P | `{"targets":["github"\|"phone"],"files":[{"name","size","mtime","crc32","vol"}]}` → `{"job"}`. The pager announces its notes; `name` is the leaf under `/notes` on `vol` (`"int"` or `"sd"`, default `"int"`), `size`/`mtime`/`crc32` describe the raw on-disk bytes (an encrypted note's `Salted__` ciphertext). The phone answers at once, pulls the files it needs with `fs.read` (§6.1) and checks each against its `crc32`; `"github"` pushes them with the phone's repo/branch/token, `"phone"` stores them in the app's folder. The REQ cap (4096 B, §3) applies: the pager splits a long manifest into batches of at most 3500 B of params and runs them as sequential jobs |
| `evt.notes.sync` | P→D | `{"job","phase","uploaded","already","errors":[<name>…],"done"}`. Progress for `job`, counts cumulative within the job; the last one has `done:true`. The pager fails the sync after 120 s without one |
| `tg.chats` | D→P | `{"bridge","token","limit","xfer"?}` → `{"body":[{"id","title","unread"}…]}` or, above 4096 B, `{"xfer","size","crc32"}` + DATA (§4). The phone calls `GET <bridge>/v1/chats?limit=` with `Authorization: Bearer <token>` and trims each chat to the listed fields. `bridge`/`token` come with every call (D-H7); the phone uses the token for that one request and never logs or stores it. Timeout 15 s |
| `tg.messages` | D→P | `{"bridge","token","chat_id","limit","xfer"?}` → `{"body":[{"id","text","from","out","media"?:{"type","w","h"}}…]}` newest first, or `{"xfer","size","crc32"}` + DATA. `GET /v1/chats/{chat_id}/messages?limit=`. `chat_id` and `limit` must be JSON integers (else `invalid_argument`): the phone builds the path itself and never fetches one the pager supplies. Timeout 15 s |
| `tg.read` | D→P | `{"bridge","token","chat_id","up_to"}` → `{}`. `POST /v1/chats/{chat_id}/read {"up_to"}` |
| `tg.send` | D→P | `{"bridge","token","chat_id","text"}` → `{}`. `POST /v1/chats/{chat_id}/messages {"text"}` |

`bridge`/`token` are optional (both or neither). When the phone holds its own
Telegram login (the companion app's TDLib session) it answers every tg.* from
that, ignoring any bridge, with the same result shapes; message `id`s are
server message ids (TDLib id `>> 20`). With no login and no bridge sent the
phone answers `unsupported` "log in on phone", shown as `phone: <msg>`. The
pager sends tg.* to the phone whenever the link is up and goes direct to the
bridge only without one.

tg.* errors: a bridge 401/403 → `unauthorized`; any other non-2xx →
`http_error` whose `msg` is the decimal HTTP status (e.g. `"502"`), which the
pager shows as `HTTP <status>`, the same text as its direct Wi-Fi path. When
the bridge gives no HTTP reply at all, `http_error`'s `msg` is a short,
token-free reason instead (`"bridge unreachable"`, `"bridge timeout"`,
`"bridge needs https"`, `"bridge TLS error"`, `"network error"`), shown as
`phone: <msg>`.

## 6.1 Methods — Phase 2 (file service, `caps` `"fs"`)

`vol` is `"int"` (internal flash) or `"sd"` (SD card; `invalid_argument`
"sd not mounted" while it is out). Every `path` is normalized against the
volume root: an optional leading `/`, repeated `/` and `.` collapse, and
`..`, a backslash or a control byte is rejected with `invalid_argument`
(`sandbox_path`, `src/hal/notes_path.h`). Bytes are the on-disk bytes: an
encrypted note is served as its `Salted__` ciphertext and flagged
`protected:true`. `crc32` is CRC-32/ISO-HDLC over the whole file.

| Method / event | Dir | Params → result |
|---|---|---|
| `fs.list` | P→D | `{"vol","path","cursor"?,"limit"?≤64}` → `{"entries":[{"name","dir","size","mtime"}],"cursor"?}`. Folders first, then files, each by name. `cursor` is present while entries remain; pass it back for the next page. `mtime` may be 0 in a listing |
| `fs.stat` | P→D | `{"vol","path"}` → `{"dir","size","mtime","protected"}` |
| `fs.read` | P→D | `{"vol","path","xfer","offset"?}` → `{"xfer","size","crc32","protected"}`, then DATA from `offset` (default 0) under §4 flow control. The phone receives, so it allocates `xfer` (even, not in use; else `invalid_argument`) |
| `fs.write.begin` | P→D | `{"vol","path","size","crc32","overwrite"?}` → `{"xfer","offset":0}`; the pager allocates `xfer` (odd). The phone then sends DATA. Bytes land in `<path>.part`. An existing file without `overwrite` → `invalid_argument` "exists"; `resume:true` → `unsupported` in v1 |
| `fs.write.commit` | P→D | `{"xfer"}` → `{}`. Checks size and `crc32`, then renames `.part` onto `path`. Mismatch or missing bytes → `invalid_argument` and the `.part` is deleted. Timeout 60 s |
| `fs.write.abort` | P→D | `{"xfer"}` → `{}`; deletes the `.part` |
| `fs.delete` | P→D | `{"vol","path"}` → `{}`. Directories are removed with their contents; the root never |
| `fs.mkdir` | P→D | `{"vol","path"}` → `{}`. One level; the parent must exist |
| `fs.rename` | P→D | `{"vol","from","to"}` → `{}`. `to` must not exist, and a directory can't move into itself |
| `evt.fs.changed` | D→P | `{}` after any local write (editor save, eviction, an fs.* change). Drop listing caches |

At most two transfers run at once (`busy` beyond that). A transfer with no
DATA/ACK for 30 s is dropped, and a link drop ends every transfer: an
upload's `.part` is deleted, so an interrupted upload leaves nothing behind.

## 6.2 Emulator test hook (not part of the protocol)

Emulator builds answer `test.callback` (P→D) so `pagerlink smoke` can drive
pager-initiated requests: `{"method","params"?,"timeout_ms"?}` makes the
pager call `method` on the phone, and `{"sink","bytes"}` makes it call `sink`
with `{"size","crc32"}` and stream `bytes` of a fixed pattern to the `xfer`
the phone returns. `{"chat":"text"|"voice"}` runs a chat turn, and
`{"notes":"github"|"phone"}` seeds `/notes` on both volumes and runs that
notes sync, and
`{"tg":"chats"|"msgs"|"send"|"notify","bridge","token","chat_id"?,"text"?,"reset"?,"link_down"?}`
runs one step of the Telegram worker code (list, open chat + mark-read, send,
one unread-notifier tick). The outcome arrives as `evt.test.result`. `test.weather.cache` `{"key"}` (P→D) answers synchronously with the forecast the pager has cached under `key`, re-encoded as a ForecastPayload (§6.3), or ERR `not_found` (test-only code) when there is none. `test.weather.fetch` `{"key"}` runs the Weather app's phone step (`weather.get` → key check → cache save) and reports `{"step":"weather","ok","msg","city","hourly_count","daily_count"}` as `evt.test.result`.
Hardware builds never register it (`src/hal/link/handlers_test.cpp`): it
would let a phone make the pager call arbitrary methods.

## 6.3 Methods — weather (`caps` `"weather"`)

The phone holds the location/weather sources; the pager caches the result per
cache key (`"auto"` for IP geolocation, otherwise the user-chosen city name)
and renders it. Both directions carry the same JSON object, the
**ForecastPayload**:

```json
{"key":"Sao Paulo","ts":1760000000,"hourly":[{"hr":0,"code":0,"temp":18,"pop":0}],"daily":[{"date":"2026-10-11","code":2,"hi":28,"lo":17,"pop":0}],"city":"Sao Paulo","lat":-23.55,"lon":-46.63}
```

| Field | Rule |
|---|---|
| `key` | required string, 1..64 bytes; the cache key the forecast belongs to |
| `ts` | required integer, 0..4294967295; unix seconds of the forecast (0 = unknown clock) |
| `hourly` | required array, at most 8 entries (3-hourly slots of today); each `{hr, code, temp, pop}`: integers `hr` 0..23, `code` 0..255 (WMO weather code), `temp` -128..127 °C, `pop` 0..100 % |
| `daily` | required array, at most 7 entries; each `{date, code, hi, lo, pop}`: `date` exactly `"YYYY-MM-DD"`, the rest integers — `code` 0..255, `hi`/`lo` -128..127, `pop` 0..100 |
| `city`, `lat`, `lon` | optional; `city` a string (stored ASCII-folded for the pager font), `lat` a number in [-90, 90], `lon` a number in [-180, 180]. The location is stored only when all three are present; a partial set is ignored, not an error |

Integers must be JSON integers (a string or a fraction is rejected). A payload that breaks any rule is refused whole with `invalid_argument` and a `msg` naming the field (e.g. `hourly[0].temp out of range`); the pager never clamps or stores part of it.

| Method / event | Dir | Params → result |
|---|---|---|
| `weather.push` | P→D | ForecastPayload → `{}`. The pager stores the forecast (and the location, when complete) under `key`; an open Weather app showing that key re-renders. Invalid → `invalid_argument` |
| `weather.get` | D→P | `{"key"}` → ForecastPayload. The pager asks for the forecast of its current key when it has no fresh cache and no Wi-Fi (or the direct fetch failed). A reply whose `key` differs from the requested one is discarded. Timeout 15 s |

The rest of the catalog (notify.*, …) is specified in
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
- `bulk.json` — DATA / ACK payload encode/decode, and CRC-32 check values (`crc32`).
