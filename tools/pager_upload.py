#!/usr/bin/env python3
"""Flash a built env to the attached LilyGo ESP32-S3 over its built-in USB-JTAG.

`make upload` runs this. It works with or without the cdc_acm serial driver,
because `pio run -t upload` can't: it needs /dev/ttyACM*, fails mid-reset when the
port re-enumerates, and its RTS hard reset leaves the chip in download mode.

  1. A running board is sent to the ROM loader with the 1200-baud request: through
     /dev/ttyACM* when it exists, else as a raw CDC SET_LINE_CODING through usbfs.
     JTAG is only reachable from the ROM loader (the app owns USB in OTG mode).
  2. OpenOCD writes and verifies the four images. NVS is not touched.
  3. An RTC watchdog reset boots the firmware (a plain reset stays in download mode).

Usage: tools/pager_upload.py --env ENV
Exit:  0 flashed and running, 1 device or flash failure, 2 bad env or missing files,
       3 no permission to reach the running board.
"""
import argparse
import ctypes
import fcntl
import glob
import os
import struct
import subprocess
import sys
import termios
import time

VID = "303a"
PID_ROM = 0x1001
APP_PIDS = {"tlora_pager": 0x82D4, "twatchs3": 0x821B, "twatch_ultra": 0x8227}

USBDEVFS_CONTROL = 0xC0185500
CDC_CLASS = "02"
SET_LINE_CODING_TYPE = 0x21  # class request, host to device, interface recipient
SET_LINE_CODING = 0x20

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGES = os.path.expanduser("~/.platformio/packages")

NO_ACCESS_HELP = """\
no /dev/ttyACM* (is cdc_acm loaded? `uname -r` must have modules in /lib/modules) and no write access to the USB node.
Either reboot into a kernel with modules, or run once:
  sudo cp tools/99-lilygo-upload.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
then replug the device. Or hold BOOT while pressing RST and run make upload again."""


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def scan():
    """Every Espressif device on USB, from sysfs (no driver or permission needed)."""
    found = []
    for sysdir in glob.glob("/sys/bus/usb/devices/*"):
        if read(os.path.join(sysdir, "idVendor")) != VID:
            continue
        found.append({
            "sysdir": sysdir,
            "pid": int(read(os.path.join(sysdir, "idProduct"), "0"), 16),
            "busnum": int(read(os.path.join(sysdir, "busnum"), "0")),
            "devnum": int(read(os.path.join(sysdir, "devnum"), "0")),
            "product": read(os.path.join(sysdir, "product")),
        })
    return found


def app_pid_for(env):
    matches = [name for name in APP_PIDS if env.startswith(name)]
    return APP_PIDS[max(matches, key=len)] if matches else None


def touch_tty(path):
    """Opening at 1200 baud is what the firmware treats as 'reboot to bootloader'."""
    fd = None
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B1200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except PermissionError:
        print(f"no permission on {path}: add yourself to the uucp (Arch) or dialout group")
        return 3
    except OSError:
        pass  # the device may reset mid-call
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
    return 0


def touch_usbfs(dev):
    """Same request as touch_tty, sent directly because no serial driver is bound."""
    node = f"/dev/bus/usb/{dev['busnum']:03d}/{dev['devnum']:03d}"
    try:
        fd = os.open(node, os.O_RDWR)
    except PermissionError:
        print(NO_ACCESS_HELP)
        return 3
    try:
        interface = None
        for path in glob.glob(os.path.join(dev["sysdir"], "*", "bInterfaceClass")):
            if read(path) == CDC_CLASS:
                number = os.path.join(os.path.dirname(path), "bInterfaceNumber")
                interface = int(read(number, "0"), 16)
                break
        if interface is None:
            print("device has no CDC control interface")
            return 1
        data = ctypes.create_string_buffer(struct.pack("<IBBB", 1200, 0, 0, 8), 7)
        transfer = bytearray(struct.pack(
            "=BBHHHI4xQ", SET_LINE_CODING_TYPE, SET_LINE_CODING, 0, interface, 7, 1000,
            ctypes.addressof(data)))
        try:
            fcntl.ioctl(fd, USBDEVFS_CONTROL, transfer)
        except OSError:
            pass  # the device resets before completing the transfer
    finally:
        os.close(fd)
    return 0


def enter_rom(dev):
    ttys = glob.glob(os.path.join(dev["sysdir"], "*", "tty", "ttyACM*"))
    if ttys:
        path = "/dev/" + os.path.basename(ttys[0])
        print(f"sending {dev['product']} to the ROM loader via {path}")
        rc = touch_tty(path)
    else:
        print(f"sending {dev['product']} to the ROM loader via usbfs (no /dev/ttyACM*)")
        rc = touch_usbfs(dev)
    if rc:
        return rc
    end = time.time() + 10
    while time.time() < end:
        devices = scan()
        if len(devices) == 1 and devices[0]["pid"] == PID_ROM:
            return 0
        time.sleep(0.25)
    print("device did not enter the ROM loader within 10s; "
          "hold BOOT while pressing RST and run make upload again")
    return 1


def flash(images, openocd):
    programs = "; ".join(f"program_esp {path} {addr} verify" for path, addr in images)
    cmd = (f"init; reset halt; {programs}; halt; "
           "mww 0x600080B0 0x50D83AA1; mww 0x6000809C 2000; mww 0x60008098 0xD0000102; "
           "resume; shutdown")
    print("flashing over USB-JTAG ...", flush=True)
    try:
        r = subprocess.run(
            [os.path.join(openocd, "bin", "openocd"),
             "-s", os.path.join(openocd, "share", "openocd", "scripts"),
             "-f", "board/esp32s3-builtin.cfg", "-c", cmd],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        print("openocd did not finish within 300s")
        return 1
    if r.returncode != 0 or r.stdout.count("** Verify OK **") != len(images):
        print(r.stdout)
        print(f"flash failed: openocd exit {r.returncode}")
        return 1
    print(f"flashed and verified {len(images)} images")
    return 0


def confirm_boot(app_pid):
    end = time.time() + 20
    while time.time() < end:
        if any(d["pid"] == app_pid for d in scan()):
            time.sleep(5)
            running = [d for d in scan() if d["pid"] == app_pid]
            if running:
                print(f"{running[0]['product']} is running (303a:{app_pid:04x})")
                return 0
            print("firmware came up then dropped off USB: possible crash, check with make rescue")
            return 1
        time.sleep(0.25)
    if any(d["pid"] == PID_ROM for d in scan()):
        print("flashed, but the device is still in the ROM loader: the firmware may be crashing")
    else:
        print("flashed, but the firmware USB device did not appear")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--env", required=True, help="PlatformIO env, already built")
    a = ap.parse_args()

    app_pid = app_pid_for(a.env)
    if app_pid is None:
        print(f"unknown env {a.env}: expected one starting with {', '.join(APP_PIDS)}")
        return 2

    build = os.path.join(REPO, ".pio", "build", a.env)
    boot_app0 = glob.glob(os.path.join(
        PACKAGES, "framework-arduinoespressif32*", "tools", "partitions", "boot_app0.bin"))
    openocd = os.path.join(PACKAGES, "tool-openocd-esp32")
    images = [(os.path.join(build, "bootloader.bin"), "0x0"),
              (os.path.join(build, "partitions.bin"), "0x8000"),
              (boot_app0[0] if boot_app0 else "boot_app0.bin", "0xe000"),
              (os.path.join(build, "firmware.bin"), "0x10000")]
    missing = [path for path, _ in images if not os.path.isfile(path)]
    if not os.path.isdir(openocd):
        missing.append(openocd)
    if missing:
        print("missing: " + ", ".join(missing))
        return 2

    devices = scan()
    if not devices:
        print("no Espressif USB device found: check the cable and that the device is on")
        return 1
    if len(devices) > 1:
        for d in devices:
            print(f"  303a:{d['pid']:04x} {d['product']}")
        print("more than one Espressif device attached: unplug all but one")
        return 1

    dev = devices[0]
    if dev["pid"] == PID_ROM:
        print(f"device is in the ROM loader; can't identify the board, trusting env {a.env}")
    elif dev["pid"] == app_pid:
        rc = enter_rom(dev)
        if rc:
            return rc
    else:
        print(f"device is 303a:{dev['pid']:04x} ({dev['product']}) but env {a.env} expects "
              f"303a:{app_pid:04x}; refusing to flash")
        return 1

    rc = flash(images, openocd)
    return rc if rc else confirm_boot(app_pid)


if __name__ == "__main__":
    sys.exit(main())
