#!/usr/bin/env python3
"""Print what is inside a .huifont, so a baked one can be checked against it.

    python3 tools/dump-font.py <file.huifont> [--glyphs N]

The format is in the kit's src/gfx/font_format.hpp: a 40-byte header, a glyph
table sorted by codepoint, a kerning table, then a single-channel SDF atlas.

Copyright (C) 2026 the psp5 authors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import pathlib
import struct

HEADER = "<IIHHfffffII"
HEADER_SIZE = struct.calcsize(HEADER)
GLYPH = "<IHHHHfff"
GLYPH_SIZE = struct.calcsize(GLYPH)
KERN = "<IIf"
KERN_SIZE = struct.calcsize(KERN)

MAGIC = 0x46505A50  # "PZPF"


def read(path):
    data = path.read_bytes()
    (magic, version, atlas_w, atlas_h, pixel_size, sdf_range, ascent, descent,
     line_gap, glyph_count, kern_count) = struct.unpack_from(HEADER, data, 0)
    if magic != MAGIC:
        raise SystemExit("%s: not a huifont (magic %08x)" % (path, magic))

    at = HEADER_SIZE
    glyphs = []
    for _ in range(glyph_count):
        glyphs.append(struct.unpack_from(GLYPH, data, at))
        at += GLYPH_SIZE
    kerns = []
    for _ in range(kern_count):
        kerns.append(struct.unpack_from(KERN, data, at))
        at += KERN_SIZE

    return {
        "version": version,
        "atlas": (atlas_w, atlas_h),
        "pixel_size": pixel_size,
        "sdf_range": sdf_range,
        "ascent": ascent,
        "descent": descent,
        "line_gap": line_gap,
        "glyphs": glyphs,
        "kerns": kerns,
        "atlas_offset": at,
        "size": len(data),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=pathlib.Path)
    parser.add_argument("--glyphs", type=int, default=12)
    args = parser.parse_args()

    font = read(args.file)
    print("version      %d" % font["version"])
    print("atlas        %dx%d" % font["atlas"])
    print("pixel_size   %g" % font["pixel_size"])
    print("sdf_range    %g" % font["sdf_range"])
    print("ascent       %g" % font["ascent"])
    print("descent      %g" % font["descent"])
    print("line_gap     %g" % font["line_gap"])
    print("glyphs       %d" % len(font["glyphs"]))
    print("kerns        %d" % len(font["kerns"]))
    expect = font["atlas_offset"] + font["atlas"][0] * font["atlas"][1]
    print("size         %d (expected %d)%s"
          % (font["size"], expect, "" if font["size"] == expect else "  MISMATCH"))

    codes = [g[0] for g in font["glyphs"]]
    print("range        U+%04X..U+%04X" % (min(codes), max(codes)))
    print()
    print("  code  char   x    y    w    h    off_x    off_y  advance")
    for g in font["glyphs"][:args.glyphs]:
        code, x, y, w, h, ox, oy, adv = g
        char = chr(code) if 0x20 < code < 0x7F else " "
        print("  %04X   %s   %4d %4d %4d %4d  %7.2f %7.2f  %7.2f"
              % (code, char, x, y, w, h, ox, oy, adv))


if __name__ == "__main__":
    main()
