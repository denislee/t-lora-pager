#!/usr/bin/env python3
"""Recover a T-LoRa-Pager whose screen shows noise after a flash, then show its boot log.

After `pio run -t upload` the esptool RTS hard reset can leave the ESP32-S3 in
ROM download mode. The firmware never runs, nothing sets up the display, and the
panel shows random memory (looks like analog TV noise). This script:

  1. finds the board on USB (VID 0x303a) and reports its mode:
       0x1001  ROM loader (download mode)    0x82d4  firmware (TinyUSB CDC)
  2. in ROM-loader mode, boots the firmware with an RTC watchdog reset (an RTS
     reset would just land in download mode again) and waits for the CDC port
  3. reads the serial log with DTR on and RTS off. TinyUSB only sends while DTR
     is set, and opening the port with default pyserial lines can drop the chip
     back into download mode.

Requires: pip install pyserial esptool
Usage:    tools/pager_rescue.py [--port PORT] [--seconds N] [--no-reset]
Exit:     0 log captured, 1 no board found / port never came back, 2 missing deps.

Note: the CDC port only exists once the firmware is running. A panic that
happens before USB comes up prints on UART0, not here: if the board keeps
falling back to 0x1001 or the port never returns, that's the sign.
"""
import argparse
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing: pip install pyserial esptool")

ESPRESSIF_VID = 0x303A
PID_ROM = 0x1001
PID_APP = 0x82D4

# RTC_CNTL watchdog: unlock, set stage-0 timeout, enable with "reset system" action.
RTC_WDT_WRITES = [
    ("0x600080B0", "0x50D83AA1"),  # RTC_CNTL_WDTWPROTECT_REG <- write key
    ("0x6000809C", "2000"),        # RTC_CNTL_WDTCONFIG1_REG  <- stage-0 timeout
    ("0x60008098", "0xD0000102"),  # RTC_CNTL_WDTCONFIG0_REG  <- enable, system reset
]


def find_board(port=None):
    """Return (device, pid) for the first Espressif port (or the named one), else (None, None)."""
    for p in list_ports.comports():
        if port and p.device != port:
            continue
        if p.vid == ESPRESSIF_VID or port:
            return p.device, p.pid
    return None, None


def wait_for(pid, port, timeout):
    end = time.time() + timeout
    while time.time() < end:
        dev, cur = find_board(port)
        if dev and cur == pid:
            return dev
        time.sleep(0.25)
    return None


def esptool(port, *args):
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "--before", "no_reset", "--after", "no_reset", *args]
    return subprocess.run(cmd, capture_output=True, text=True)


def rtc_wdt_reset(port):
    if subprocess.run([sys.executable, "-m", "esptool", "version"],
                      capture_output=True).returncode != 0:
        print("esptool missing: pip install esptool")
        sys.exit(2)
    for i, (addr, val) in enumerate(RTC_WDT_WRITES):
        r = esptool(port, "write_mem", addr, val)
        last = i == len(RTC_WDT_WRITES) - 1
        # The last write fires the reset, so esptool may lose the port mid-reply.
        if r.returncode != 0 and not last:
            print(r.stdout + r.stderr)
            sys.exit(f"write_mem {addr} failed")


def read_log(port, seconds):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.rts, s.dtr = False, True  # set before open(): RTS low, DTR high
    s.open()
    print(f"--- reading {port} for {seconds}s (Ctrl-C to stop) ---", flush=True)
    end = time.time() + seconds
    try:
        while time.time() < end:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode(errors="replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    except serial.SerialException as e:
        print(f"\n--- port dropped: {e} (reboot or crash?) ---")
    finally:
        s.close()
    print("\n--- end of log ---")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="serial port (default: first VID 0x303a device)")
    ap.add_argument("--seconds", type=int, default=30, help="how long to read the log")
    ap.add_argument("--no-reset", action="store_true",
                    help="only report the mode, never reset from the ROM loader")
    a = ap.parse_args()

    dev, pid = find_board(a.port)
    if not dev:
        print("no Espressif USB device found: check the cable, or power-cycle the pager")
        return 1

    if pid == PID_ROM:
        print(f"{dev}: 303a:1001 ROM loader. The firmware is not running (download mode).")
        if a.no_reset:
            return 0
        print("booting the firmware with an RTC watchdog reset ...")
        rtc_wdt_reset(dev)
        dev = wait_for(PID_APP, None, 15)
        if not dev:
            _, cur = find_board()
            if cur == PID_ROM:
                print("came back as the ROM loader again: the firmware is probably crashing or "
                      "the app partition is bad. Re-flash, then check UART0 for a panic.")
            else:
                print("firmware USB port did not show up within 15s: it may crash before USB init.")
            return 1
        print(f"{dev}: firmware is up (303a:82d4).")
    elif pid == PID_APP:
        print(f"{dev}: 303a:82d4 firmware running. If the screen is noise, the app is up but "
              "not drawing; the log below should say why.")
    else:
        print(f"{dev}: unknown PID 0x{pid or 0:04x}, reading it anyway")

    read_log(dev, a.seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
