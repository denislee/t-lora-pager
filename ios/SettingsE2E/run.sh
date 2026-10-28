#!/usr/bin/env bash
# Settings end to end (IOS_SETTINGS_PLAN.md W12), run by `make -C ios settings-e2e`.
#
# Starts the pager emulator on a fresh state root, sets every writable setting
# over PagerLink and reads it back (phase `write`), kills the emulator with
# SIGKILL — no clean exit, so only eagerly persisted values survive — starts
# it again on the same root and reads every value back (phase `verify`).
#
# Needs the emulator built (`pio run -e emulator_lora_pager`) and port 7600
# free. Linux runs the Swift side in Docker (swift:6.0-jammy) and the emulator
# under xvfb-run when there is no display; macOS runs both natively.
# Exit status = number of failed phases.
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
EMU="$REPO/.pio/build/emulator_lora_pager/program"
STATE=".build/settings-e2e-expected.json"   # relative to ios/SettingsE2E
ROOT=$(mktemp -d /tmp/settings-e2e-XXXXXX)

[ -x "$EMU" ] || { echo "settings-e2e: build the emulator first (pio run -e emulator_lora_pager)"; exit 1; }

port_open() { (exec 3<>/dev/tcp/127.0.0.1/7600) 2>/dev/null; }

if port_open; then
    echo "settings-e2e: something already listens on 7600 (stop it: pkill -x program)"
    exit 1
fi

start_emu() {
    local wrap=()
    if [ "$(uname)" != Darwin ] && [ -z "${DISPLAY:-}" ]; then wrap=(xvfb-run -a); fi
    (cd "$REPO" && PAGERLINK_FS_ROOT="$ROOT" "${wrap[@]}" "$EMU" >>"$ROOT/emulator.log" 2>&1 &)
    for _ in $(seq 1 80); do port_open && return 0; sleep 0.25; done
    echo "settings-e2e: the emulator did not open 7600 (see $ROOT/emulator.log)"
    return 1
}

stop_emu() {
    pkill "$1" -x program
    for _ in $(seq 1 40); do port_open || return 0; sleep 0.25; done
}

phase() {
    if [ "$(uname)" = Darwin ]; then
        (cd "$REPO/ios/SettingsE2E" && swift run SettingsE2E "$1" "$STATE")
    else
        docker run --rm --network host --user "$(id -u):$(id -g)" -e HOME=/tmp \
            -v "$REPO:/repo" -w /repo/ios/SettingsE2E swift:6.0-jammy \
            swift run SettingsE2E "$1" "$STATE"
    fi
}

fails=0
start_emu || exit 1
phase write || fails=$((fails + 1))
stop_emu -KILL
start_emu || { echo "settings-e2e: restart failed"; exit 1; }
phase verify || fails=$((fails + 1))
stop_emu -TERM

if [ "$fails" -eq 0 ]; then
    echo "settings-e2e: all phases passed"
    rm -rf "$ROOT"
else
    echo "settings-e2e: $fails phase(s) failed; emulator state kept in $ROOT"
fi
exit "$fails"
