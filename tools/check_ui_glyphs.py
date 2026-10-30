#!/usr/bin/env python3
"""Reject string literals the UI fonts can't render (VB.3).

The generated LVGL fonts only carry ASCII plus the LV_SYMBOL_* private-use
range, so a literal "»", "—" or "→" draws as a tofu box. This scans every
C/C++ string literal under src/ (comments are ignored) for raw non-ASCII
characters and for \\x-escaped UTF-8 lead bytes, allowing only the
LV_SYMBOL private-use block (U+F000-U+F8FF, \\xEF\\x80-\\xA3).

Usage: tools/check_ui_glyphs.py [paths...]   (default: src)  exit 1 on hits.
"""
import pathlib
import re
import sys

SKIP_DIRS = ("fonts", "images")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
ESC = re.compile(r'\\x([0-9a-fA-F]{2})\\x([0-9a-fA-F]{2})')


def bad_in(lit):
    hits = sorted({"U+%04X" % ord(c) for c in lit if ord(c) > 127 and not 0xF000 <= ord(c) <= 0xF8FF})
    for a, b in ESC.findall(lit):
        a, b = int(a, 16), int(b, 16)
        if 0xC2 <= a <= 0xF4 and 0x80 <= b <= 0xBF and not (a == 0xEF and b <= 0xA3):
            hits.append("\\x%02X\\x%02X" % (a, b))
    return hits


def files(paths):
    for p in map(pathlib.Path, paths):
        if p.is_file():
            yield p
            continue
        for f in sorted(p.rglob("*")):
            if f.suffix in (".c", ".cpp", ".h", ".ino") and not set(f.parts) & set(SKIP_DIRS):
                if f.parts[:2] == ("src", "src"):  # generated image/font arrays
                    continue
                yield f


def main(argv):
    n = 0
    for f in files(argv or ["src"]):
        text = f.read_text(errors="replace")
        for m in TOKEN.finditer(text):
            tok = m.group(0)
            if tok[0] not in "\"'":
                continue
            hits = bad_in(tok)
            if hits:
                line = text.count("\n", 0, m.start()) + 1
                print(f"{f}:{line}: {' '.join(hits)} {tok[:70]}")
                n += 1
    if n:
        print(f"{n} literal(s) use glyphs the UI fonts lack; use ASCII or LV_SYMBOL_*.")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
