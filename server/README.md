# LilyHub

Local-first HTTP server for LilyGo pager / watch apps. Sits on a Linux box on
the same LAN as the device (e.g. a Raspberry Pi or any always-on ARM box).
On-device apps point at it first; on any failure they fall back to the public
internet so the device keeps working when the hub is offline.

Apps served: **Weather** (proxies and caches `ip-api.com` and `open-meteo.com`),
**Notes** (GitHub sync proxy + on-hub note store), **Chat** (Groq LLM + Whisper
STT), and a Telegram Bot API proxy. Adding more apps is a matter of dropping a
new package next to `internal/weather/` and wiring its `Register(mux)` in
`cmd/lilyhub/main.go`.

## Layout

```
server/
├── cmd/lilyhub/        entry point — wires the cache, mux and apps
├── internal/cache/     in-memory TTL cache shared across apps
├── internal/httpx/     proxy, retry/backoff, JSON decode/write helpers + request logger
├── internal/weather/   /api/weather/* endpoints (first app)
├── internal/chat/      /api/chat — Groq chat completion + Whisper STT, per-device history
├── internal/notessync/ /api/notes/sync (GitHub Git Data API push) + /api/notes/upload (on-hub store)
├── internal/telegram/  /api/telegram/proxy — SSRF-guarded relay to api.telegram.org
├── deploy/             systemd unit
└── Makefile            build + cross-compile targets
```

## Endpoints

| Method | Path                              | Cache TTL | Upstream |
|-------:|-----------------------------------|-----------|----------|
| GET    | `/healthz`                        | —         | (local)  |
| GET    | `/api/weather/geo/ip`             | 30 min    | `http://ip-api.com/json/?fields=...` |
| GET    | `/api/weather/geo/search?name=…&count=…` | 24 h | `https://geocoding-api.open-meteo.com/v1/search` |
| GET    | `/api/weather/forecast?latitude=…&longitude=…&…` | 10 min | `https://api.open-meteo.com/v1/forecast` |
| POST   | `/api/notes/sync`                 | —         | `https://api.github.com` |
| POST   | `/api/chat`                       | —         | `https://api.groq.com/openai/v1` |
| POST   | `/api/notes/upload`               | —         | local disk (`$LILYHUB_NOTES_DIR`) |
| POST   | `/api/telegram/proxy`             | —         | `https://api.telegram.org` |

Routes are registered with Go 1.22 method patterns, so a wrong method gets 405
from the mux.

`POST /api/chat` accepts `{device_id, text?, audio_b64?, reset?}` and returns
`{transcript?, reply}`. When `audio_b64` is set the hub runs Whisper STT
first; the transcript becomes the user turn. Conversation history is kept
in-process keyed by `device_id` (last 20 messages, idle sessions reaped at
1 h). Set `reset:true` to clear it. Requires `GROQ_API_KEY` in the
environment — without it the endpoint returns 503. Free key at
[console.groq.com](https://console.groq.com); set it via `/etc/lilyhub/env`
(see "Configure secrets" below).

`/api/notes/sync` accepts `{repo, branch?, token, files:[{name, content_b64}]}`
and returns `{uploaded, already, errors?}`. Additive only: names already in
`notes/` on the branch are skipped, nothing is overwritten or deleted, so it is
safe to retry. New files land as one commit (parallel blob creates, then tree,
commit, ref update). The token is a GitHub PAT with `contents:write`; it is
never logged or stored.

`/api/notes/upload` accepts `{name, content_b64}` and stores the decoded bytes
at `$LILYHUB_NOTES_DIR/<name>` (atomic temp-file + rename). Replies `200 {stored, bytes, path}`;
the device only checks the status, the body is for curl debugging.

`/api/telegram/proxy` accepts `{url, method (GET|POST), token, body?}`; `url`
must be `https://api.telegram.org/...`; Telegram's status and JSON body are
relayed verbatim.

JSON replies the device reads carry `Content-Length` and `Connection: close`
(the ESP32 HTTPClient can hang on chunked responses).

## Environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `GROQ_API_KEY` | (unset) | enables `/api/chat`; without it the endpoint returns 503 |
| `STT_LANG` | `pt` | ISO-639-1 language hint sent to Whisper |
| `STT_LANG_AUTO` | (unset) | set to any value (with `STT_LANG` empty) to let Whisper auto-detect |
| `STT_PROMPT` | a short pt-BR vocabulary hint | Whisper biasing prompt; set to empty string to send none |
| `LILYHUB_NOTES_DIR` | `$HOME/.lilyhub/notes` (falls back to `$TMPDIR/lilyhub-notes`) | where `/api/notes/upload` stores files |
| `LILYHUB_ADDR` | `:8080` | listen address (the `-addr` flag overrides it) |

## Configure secrets

The unit reads `/etc/lilyhub/env` (optional — the service still starts
without it, only `/api/chat` is gated on `GROQ_API_KEY`). The Makefile
manages this file for you:

```sh
# Bundled with install:
GROQ_API_KEY=gsk_xxx make install

# After install, or to rotate later:
GROQ_API_KEY=gsk_new make set-groq-key
```

Either form writes `/etc/lilyhub/env` as `0600 root:root` and restarts
`lilyhub.service` if it's running. Pass the key via the environment
(as above) instead of `make ... GROQ_API_KEY=…` to keep it out of `ps`
and shell history.

The file is read by systemd as PID 1 *before* it drops privileges to the
`lilyhub` user, so root-only is correct — the daemon never reads it
directly. To revoke: `sudo rm /etc/lilyhub/env && sudo systemctl restart
lilyhub` — the chat endpoint then cleanly degrades to 503 and the device
shows a clear error.

`make install`, `make upgrade`, and `make uninstall` never delete this
file unless you explicitly `rm -rf /etc/lilyhub`.

The `geo/ip` and `forecast` responses are passed through unchanged, so the device
parsers stay unchanged; `geo/search` is the exception — it is trimmed to the
fields the device reads (`name`, `latitude`, `longitude`, `country`, `admin1`;
see `trimGeoSearch`). The forecast endpoint forwards the full query string, so
adding/removing `hourly=`/`daily=` fields on the device needs no server change.

`X-Cache: HIT|MISS` is set on responses for debugging.

## Build

```sh
make build           # native
make linux-arm64     # Raspberry Pi 3/4/5, BPi, Pine64, etc.
make linux-armv7     # 32-bit ARM (Raspberry Pi 2, older boards)
make linux-amd64     # x86_64 Linux
```

The binary is statically linked (`CGO_ENABLED=0`), so it has no glibc/musl
dependency on the target host.

## Install as a systemd service

If you're building on the same Linux box you'll run on, the Makefile does the
whole setup. From the `server/` directory:

```sh
make install        # build + create user + install binary + enable + start
make status         # show systemd status
make logs           # tail logs (Ctrl-C to stop)
make upgrade        # rebuild + replace binary + restart (after `git pull`)
make uninstall      # stop, disable, remove binary + unit
make restart        # restart the service
make stop / start   # one-off control
```

`make install` is idempotent — safe to re-run. Defaults can be overridden:

```sh
make install PREFIX=/usr/local/lilyhub USER=hub GROUP=hub
```

For cross-host install (build elsewhere, deploy to ARM box):

```sh
make linux-arm64
scp bin/lilyhub-linux-arm64 deploy/lilyhub.service user@host:/tmp/
ssh user@host
# on the target:
sudo useradd -r -s /usr/sbin/nologin lilyhub
sudo install -d -o lilyhub -g lilyhub /opt/lilyhub
sudo install -m 0755 /tmp/lilyhub-linux-arm64 /opt/lilyhub/lilyhub
sudo install -m 0644 /tmp/lilyhub.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now lilyhub
```

Verify:

```sh
curl -s http://<host>:8080/healthz
curl -s 'http://<host>:8080/api/weather/forecast?latitude=-23.55&longitude=-46.63&hourly=temperature_2m&forecast_days=1&timezone=auto'
```

## Configure the device

Open **Settings → Local Hub** on the device, toggle "Use local hub" on, and
set the URL (e.g. `http://192.168.1.10:8080`, no trailing slash). The setting
is shared across every feature that consults the hub (weather, notes sync,
…). On any hub failure (timeout, 5xx, DNS, connection refused) the device
transparently falls back to the public internet — or, for notes sync, to the
direct GitHub path.

## Adding a new app

1. Create `internal/<app>/<app>.go` with a `Handler` and a `Register(mux *http.ServeMux)` method.
2. In `cmd/lilyhub/main.go`, instantiate it after `weather.New(c)` and call `Register(mux)`.
3. Reuse `httpx.Proxy` for cached pass-throughs; talk to the cache directly for anything bespoke.
