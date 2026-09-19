#!/usr/bin/env python3
"""Regenerate the shared LVGL fonts (lib/ws_fonts/ws_font_*.c).

The fonts are generated assets: they are committed so the project builds without
network access, and this script exists to make regeneration reproducible and
reviewable.

Prerequisites (one-off, outside the repositories):
  1. Node.js 18+ (lv_font_conv is a Node tool; it is NOT part of any build):
         npx --yes lv_font_conv --version
  2. The Montserrat source font (SIL Open Font License 1.1), downloaded to a tool
     cache outside the repo, e.g.:
         curl -L -o .tools/fonts/Montserrat-Variable.ttf \
           "https://raw.githubusercontent.com/google/fonts/main/ofl/montserrat/Montserrat%5Bwght%5D.ttf"
     SHA-256 of the file used for the committed fonts:
         0f7b311b2f3279e4eef9b2f968bcdbab6e28f4daeb1f049f4f278a902bcd82f7

Usage:
    python lib/ws_fonts/generate_fonts.py [--ttf PATH] [--out DIR]

The generated files are post-processed: lv_font_conv guards each file with its own
`#if WS_FONT_xx`, this script replaces that with the shared `WS_LVGL_FONTS` guard
so the firmware (no LVGL) compiles an empty translation unit, and moves the
`#include "lvgl.h"` inside the guard.
"""
import argparse
import pathlib
import re
import subprocess
import sys

SIZES = (14, 20, 28)
RANGES = ("0x20-0x7F", "0xB0", "0x401", "0x410-0x44F", "0x451", "0x2013-0x2014",
          "0x2018-0x201D", "0x2026", "0x00AB", "0x00BB")

PROLOGUE = """/* ============================================================================
 * ws_font_{size}.c — GENERATED FILE, do not edit by hand.
 *
 * Montserrat {size} px, 4 bpp, ASCII + Cyrillic (А-я, Ё/ё) + typographic marks.
 * Regenerate with:  python lib/ws_fonts/generate_fonts.py
 * Source typeface:  Montserrat (SIL OFL 1.1) — see lib/ws_fonts/README.md
 *
 * The guard keeps this file empty in builds without LVGL (the current ESP32
 * environment), so PlatformIO can compile lib/ unconditionally.
 * ==========================================================================*/
#include "ws_fonts.h"

#if defined(WS_LVGL_FONTS)

#include "lvgl.h"
"""

EPILOGUE = "#endif /* WS_LVGL_FONTS */\n"


def patch(text: str, size: int) -> str:
    """Replaces lv_font_conv's own guard block with the shared WS_LVGL_FONTS one."""
    # Drop the auto-generated comment header, the include block and the per-file
    # guard, then re-attach our prologue.
    marker = re.search(r"^#if WS_FONT_%d\s*$" % size, text, re.MULTILINE)
    if marker is None:
        raise SystemExit(f"cannot find the '#if WS_FONT_{size}' guard in the generated file")
    body = text[marker.end():]
    body = body.rstrip()
    # Remove lv_font_conv's trailing '#endif /* #if WS_FONT_xx */'.
    body = re.sub(r"#endif\s*/\*\s*#if WS_FONT_%d\s*\*/\s*$" % size, "", body).rstrip()
    return PROLOGUE.format(size=size) + body + "\n" + EPILOGUE


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ttf", default=r"C:\Users\Victor\Desktop\workspace\family-project"
                                         r"\agents_test\ds-harness\.tools\fonts\Montserrat-Variable.ttf")
    parser.add_argument("--out", default=str(pathlib.Path(__file__).resolve().parent))
    args = parser.parse_args()

    ttf = pathlib.Path(args.ttf)
    if not ttf.exists():
        print(f"source font not found: {ttf}", file=sys.stderr)
        print("see the docstring for the download command", file=sys.stderr)
        return 2

    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    # lv_font_conv derives BOTH the guard macro and the C symbol name from the
    # output file name, so it must be generated as ws_font_<size>.c — a scratch
    # subdirectory avoids clobbering the committed file before it is patched.
    scratch = out_dir / "_generated"
    scratch.mkdir(exist_ok=True)

    for size in SIZES:
        generated = scratch / f"ws_font_{size}.c"
        command = ["npx", "--yes", "lv_font_conv", "--font", str(ttf)]
        for r in RANGES:
            command += ["--range", r]
        command += ["--bpp", "4", "--size", str(size), "--format", "lvgl", "--no-compress",
                    "--lv-include", "lvgl.h", "-o", str(generated)]
        print(" ".join(command))
        subprocess.run(command, check=True, shell=(sys.platform == "win32"))
        target = out_dir / f"ws_font_{size}.c"
        target.write_text(patch(generated.read_text(encoding="utf-8"), size), encoding="utf-8")
        print(f"  -> {target.name} ({target.stat().st_size} bytes)")

    for leftover in scratch.glob("*"):
        leftover.unlink()
    scratch.rmdir()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
