#!/usr/bin/env python3
"""Reject string literals the UI fonts can't render (VB.3).

The generated LVGL fonts only carry ASCII plus the LV_SYMBOL_* private-use
range, so a literal "»", "—" or "→" draws as a tofu box. This scans every
C/C++ string literal under src/ (comments are ignored) for raw non-ASCII
characters and for \\x-escaped UTF-8 lead bytes, allowing only the
LV_SYMBOL private-use block (U+F000-U+F8FF, \\xEF\\x80-\\xA3).

It also checks the home tiles: the icons in kItems (src/apps/menu_app.cpp) can
render in font_montserrat_48_subset (see pick_icon_font), so every
LV_SYMBOL_* they use must be in that subset's unicode list - otherwise the
tile draws a tofu box. That check needs the LVGL headers from .pio/libdeps,
and is skipped (not failed) when they are missing.

Usage: tools/check_ui_glyphs.py [paths...]   (default: src)  exit 1 on hits.
"""
import glob
import os
import pathlib
import re
import sys

MENU = "src/apps/menu_app.cpp"
SUBSET = "src/fonts/font_montserrat_48_subset.c"
SYMBOL_DEF = ".pio/libdeps/*/lvgl/src/font/lv_symbol_def.h"
SYMBOL_DEFINE = re.compile(r'#define\s+(LV_SYMBOL_\w+)\s+"[^"]*"\s*/\*\s*(\d+)')

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


def block_after(text, opener, path, what):
    """The text between opener and the first '};' after it, or None."""
    i = text.find(opener)
    if i < 0:
        print(f"check_ui_glyphs: {path}: no '{opener}' found, {what} cannot be checked")
        return None
    rest = text[i + len(opener):]
    j = rest.find("};")
    if j < 0:
        print(f"check_ui_glyphs: {path}: '{opener}' is not closed by '}};', {what} cannot be checked")
        return None
    return rest[:j]


def check_home_tile_symbols():
    for path in (MENU, SUBSET):
        if not os.path.isfile(path):
            print(f"check_ui_glyphs: {path} not found, home tile check cannot run")
            return 1

    body = block_after(pathlib.Path(MENU).read_text(errors="replace"),
                       "kItems[] = {", MENU, "home tile symbols")
    if body is None:
        return 1
    symbols = sorted(set(re.findall(r"LV_SYMBOL_[A-Z0-9_]+", body)))
    if not symbols:
        print(f"check_ui_glyphs: {MENU}: kItems lists no LV_SYMBOL_*, home tile check cannot run")
        return 1

    body = block_after(pathlib.Path(SUBSET).read_text(errors="replace"),
                       "unicode_list_0[] = {", SUBSET, "the subset's unicode list")
    if body is None:
        return 1
    # range_start is 32, so a code point is 32 + its offset in the list.
    covered = {32 + int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", body)}

    headers = sorted(glob.glob(SYMBOL_DEF))
    if not headers:
        print("check_ui_glyphs: no lv_symbol_def.h under .pio/libdeps, "
              "home tile check skipped (run pio pkg install)")
        return 0
    values = {}
    for line in pathlib.Path(headers[0]).read_text(errors="replace").splitlines():
        m = SYMBOL_DEFINE.match(line)
        if m:
            values.setdefault(m.group(1), int(m.group(2)))

    n = 0
    for sym in symbols:
        cp = values.get(sym)
        if cp is None:
            print(f"{MENU}: {sym} is not defined in {headers[0]}")
            n += 1
        elif cp not in covered:
            print(f"{SUBSET}: {sym} (U+{cp:04X}) is used by a home tile but is missing from the subset")
            n += 1
    return n


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
    if not argv or argv == ["src"]:  # default scan; the Makefile passes 'src'
        n += check_home_tile_symbols()
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
