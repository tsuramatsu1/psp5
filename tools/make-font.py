#!/usr/bin/env python3
"""Bake a TTF into the UI kit's .huifont format.

    python3 tools/make-font.py <font.ttf> <out.huifont> [--size 56] [--range 8]
                               [--scripts latin,cyrillic,greek] [--like ref.huifont]

The kit ships baked fonts and the header of src/gfx/font_format.hpp names a
`tools/font-baker` that is not in the repository, so this is psp5's. The format
is small and fully documented there:

    Header                                    40 bytes
    Glyph[glyph_count]                        sorted by codepoint, 24 bytes each
    Kern[kern_count]                          sorted by (first, second)
    uint8 atlas[atlas_width * atlas_height]   single-channel SDF, row-major

The SDF stores 128 on the glyph's edge and falls by 128 / sdf_range per pixel
outward, which is what the kit's shader expects.

Why this exists: the shipped atlases hold 113 glyphs, essentially ASCII, so a
game called "God of War®" drew a question mark, and so did every language name
that is not written in plain English.

Copyright (C) 2026 the psp5 authors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import pathlib
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HEADER = "<IIHHfffffII"
GLYPH = "<IHHHHfff"
KERN = "<IIf"
MAGIC = 0x46505A50  # "PZPF"
VERSION = 1

ATLAS_WIDTH = 1024


# ---- which characters to bake -------------------------------------------

def codepoints(scripts):
    """The set to bake, as a sorted list.

    Latin-1 and Latin Extended-A between them cover every western and central
    European language - French, German, Spanish, Portuguese, Polish, Czech,
    Turkish - and the accents and trademark signs that turn up in game titles.
    """
    want = set(range(0x20, 0x7F))  # ASCII, always

    # The shapes the kit draws as text rather than geometry - arrows, bullets,
    # the tick. Every one of these is in the fonts it ships, so dropping any of
    # them would take arrows out of the interface.
    want |= {0x00B0, 0x00D7, 0x2022, 0x2713,
             0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CF,
             0x2190, 0x2191, 0x2192, 0x2193}

    # What turns up in a game's own title, and in prose psp5 writes.
    want |= {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x00B7,
             0x2122, 0x00AE, 0x00A9}

    if "latin" in scripts:
        want |= set(range(0x00A0, 0x0180))  # Latin-1 Supplement + Extended-A
        want |= set(range(0x0180, 0x0200))  # Extended-B, for the odd name
    if "cyrillic" in scripts:
        want |= set(range(0x0400, 0x0460))
    if "greek" in scripts:
        want |= set(range(0x0370, 0x0400))
    return sorted(want)


def interface_codepoints(source, langs, own):
    """Every character psp5's interface can end up drawing.

    This used to read the keys out of the C++ and bake only those translations.
    It was wrong three times over, and each time the same way: a string psp5
    draws but does not name. "Recent" came from a category the key scanner did
    not know (MainMenu); "Time Played" is formatted inside PPSSPP's own
    Config.cpp, so psp5 never writes the key at all; and "Favorites" moved into
    psp5's own translation files, which the scanner never looked at. Each one
    reached a console as "??" with nothing in the build to catch it.

    So it no longer tries to be clever. Every value in every section of every
    language file is baked, plus psp5's own. That is 2634 codepoints against
    the 2285 the curated list would give - fifteen per cent more glyphs to stop
    a whole class of bug, which is a trade worth making. `source` is kept for
    the caller's signature and no longer read.
    """
    want = set()
    for path in sorted(langs.glob("*.ini")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line or line.startswith("[") or line.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep:
                continue
            want |= {ord(c) for c in value.strip()}

    # psp5's own words, where PPSSPP has none (assets/lang/psp5/<id>.ini).
    if own is not None and own.is_dir():
        for path in sorted(own.glob("*.ini")):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                key, sep, value = line.partition("=")
                if not sep:
                    continue
                want |= {ord(c) for c in value.strip()}
    return want


# ---- the signed distance field -------------------------------------------

def distance_transform(mask):
    """Euclidean distance to the nearest True, for every cell. Felzenszwalb.

    numpy is here and scipy is not, so this is the two-pass parabola method
    written out: one pass down the columns, one along the rows, each O(n).
    """
    big = 1e20
    height, width = mask.shape
    out = np.where(mask, 0.0, big)

    def pass1d(values):
        n = values.shape[0]
        k = 0
        v = np.zeros(n, dtype=np.int64)
        z = np.empty(n + 1)
        z[0] = -big
        z[1] = big
        for q in range(1, n):
            s = ((values[q] + q * q) - (values[v[k]] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k])
            while s <= z[k]:
                k -= 1
                s = ((values[q] + q * q) - (values[v[k]] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k])
            k += 1
            v[k] = q
            z[k] = s
            z[k + 1] = big
        k = 0
        result = np.empty(n)
        for q in range(n):
            while z[k + 1] < q:
                k += 1
            d = q - v[k]
            result[q] = d * d + values[v[k]]
        return result

    for x in range(width):
        out[:, x] = pass1d(out[:, x])
    for y in range(height):
        out[y, :] = pass1d(out[y, :])
    return np.sqrt(out)


def signed_field(ink, sdf_range):
    """The kit's encoding: 128 on the edge, falling by 128/range outward."""
    inside = ink > 127
    outside = distance_transform(inside)
    insidedist = distance_transform(~inside)
    # Positive inside, negative outside, in pixels.
    signed = np.where(inside, insidedist - 0.5, -(outside - 0.5))
    value = 128.0 + signed * (128.0 / sdf_range)
    return np.clip(value, 0.0, 255.0).astype(np.uint8)


# ---- kerning ------------------------------------------------------------

def kerning(ttf, size, wanted):
    """Pairs from the font's GPOS, in pixels, for the glyphs being baked.

    The kit's own Inter carries 263 of these and text set without them is
    visibly looser around AV, To, Yo and the like. fontTools reads GPOS; the
    pairs are flattened here into the (first, second, amount) triples the
    format wants, keyed by codepoint rather than glyph name.
    """
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return []

    font = TTFont(str(ttf), lazy=True)
    if "GPOS" not in font:
        return []
    units = font["head"].unitsPerEm
    scale = float(size) / float(units)

    # glyph name -> codepoint, for the glyphs we are baking.
    names = {}
    for code, name in font.getBestCmap().items():
        if code in wanted:
            names.setdefault(name, code)

    pairs = {}

    def classes_of(definition):
        out = {}
        if definition is None:
            return out
        for name, index in definition.classDefs.items():
            out.setdefault(index, []).append(name)
        return out

    for lookup in font["GPOS"].table.LookupList.Lookup:
        if lookup.LookupType != 2:  # pair adjustment
            continue
        for sub in lookup.SubTable:
            fmt = getattr(sub, "Format", None)
            if fmt == 1:
                coverage = sub.Coverage.glyphs
                for first, record in zip(coverage, sub.PairSet):
                    for entry in record.PairValueRecord:
                        amount = getattr(entry.Value1, "XAdvance", 0) or 0
                        if amount:
                            pairs[(first, entry.SecondGlyph)] = amount
            elif fmt == 2:
                first_classes = classes_of(sub.ClassDef1)
                second_classes = classes_of(sub.ClassDef2)
                covered = set(sub.Coverage.glyphs)
                for i, row in enumerate(sub.Class1Record):
                    for j, cell in enumerate(row.Class2Record):
                        amount = getattr(cell.Value1, "XAdvance", 0) or 0
                        if not amount:
                            continue
                        for a in first_classes.get(i, []):
                            if a not in covered:
                                continue
                            for b in second_classes.get(j, []):
                                pairs[(a, b)] = amount

    # Montserrat's classes expand to some forty thousand pairs, nearly all of
    # them a fraction of a pixel at this size. A quarter of a pixel is the
    # smallest adjustment worth half a megabyte of table.
    out = []
    for (a, b), amount in pairs.items():
        if a not in names or b not in names:
            continue
        pixels = amount * scale
        if abs(pixels) < 0.25:
            continue
        out.append((names[a], names[b], pixels))
    out.sort(key=lambda k: (k[0], k[1]))
    return out


# ---- the font's own metrics, which PIL rounds ---------------------------

def hhea_metrics(path, size):
    """ascent, descent and line gap in pixels, read from the TTF itself.

    PIL rounds these to whole pixels; the shipped fonts carry fractions, so
    they are taken from the head and hhea tables directly rather than through
    PIL. A sfnt file is a table directory and these two tables are fixed-width.
    """
    data = path.read_bytes()
    count = struct.unpack_from(">H", data, 4)[0]
    tables = {}
    for i in range(count):
        at = 12 + i * 16
        tag = data[at:at + 4].decode("latin-1")
        offset, length = struct.unpack_from(">II", data, at + 8)
        tables[tag] = (offset, length)
    if "head" not in tables or "hhea" not in tables:
        return None
    units = struct.unpack_from(">H", data, tables["head"][0] + 18)[0]
    hhea = tables["hhea"][0]
    ascent, descent, line_gap = struct.unpack_from(">hhh", data, hhea + 4)
    scale = float(size) / float(units)
    return ascent * scale, descent * scale, line_gap * scale


# ---- baking ---------------------------------------------------------------

def coverage(path):
    """The codepoints a font file actually has, via its cmap."""
    try:
        from fontTools.ttLib import TTFont
        return set(TTFont(str(path), fontNumber=0, lazy=True).getBestCmap().keys())
    except Exception:
        return None


def bake(ttf, size, sdf_range, wanted, fallbacks=()):
    font = ImageFont.truetype(str(ttf), size)
    pad = int(sdf_range)

    # Faces to try, in order, for anything the first one does not carry. The
    # kit's fonts have no CJK, Thai or Hebrew at all, so without this a
    # Japanese label is a row of blanks however many codepoints are asked for.
    chain = [(font, coverage(ttf))]
    for path in fallbacks:
        try:
            chain.append((ImageFont.truetype(str(path), size), coverage(path)))
        except Exception as exc:
            print("  (fallback %s unusable: %s)" % (pathlib.Path(path).name, exc))

    glyphs = []
    borrowed = 0
    for code in wanted:
        char = chr(code)
        font = None
        for candidate, covered in chain:
            if covered is None or code in covered:
                font = candidate
                break
        if font is None:
            continue
        if font is not chain[0][0]:
            borrowed += 1
        try:
            if font.getmask(char, mode="L").size == (0, 0) and code != 0x20:
                continue
        except Exception:
            continue
        advance = font.getlength(char)

        # Render into a box big enough for the glyph wherever it sits.
        box = int(size * 3)
        image = Image.new("L", (box, box), 0)
        draw = ImageDraw.Draw(image)
        origin = (box // 4, box // 4)
        draw.text(origin, char, font=font, fill=255)
        ink = np.array(image)
        rows = np.any(ink > 0, axis=1)
        cols = np.any(ink > 0, axis=0)
        if not rows.any() or not cols.any():
            # A space, or anything else with no ink: advance only.
            glyphs.append({"code": code, "w": 0, "h": 0, "ox": 0.0, "oy": 0.0,
                           "advance": advance, "sdf": None})
            continue

        top, bottom = np.where(rows)[0][[0, -1]]
        left, right = np.where(cols)[0][[0, -1]]
        cut = ink[top:bottom + 1, left:right + 1]
        padded = np.zeros((cut.shape[0] + pad * 2, cut.shape[1] + pad * 2), dtype=np.uint8)
        padded[pad:pad + cut.shape[0], pad:pad + cut.shape[1]] = cut
        sdf = signed_field(padded, sdf_range)

        # PIL draws with the text origin at the top of the ascent box, so the
        # baseline sits `ascent` below it. The kit wants the quad's top-left
        # relative to the pen on the baseline.
        ascent_px = font.getmetrics()[0]
        baseline_y = origin[1] + ascent_px
        glyphs.append({
            "code": code,
            "w": padded.shape[1],
            "h": padded.shape[0],
            "ox": float(left - origin[0] - pad),
            "oy": float(top - baseline_y - pad),
            "advance": advance,
            "sdf": sdf,
        })
    return glyphs, borrowed


def pack(glyphs, width):
    """Shelf packing, tallest first. Good enough and entirely predictable."""
    order = sorted([g for g in glyphs if g["sdf"] is not None],
                   key=lambda g: -g["h"])
    x = y = shelf = 0
    for g in order:
        if x + g["w"] > width:
            x = 0
            y += shelf
            shelf = 0
        g["x"], g["y"] = x, y
        x += g["w"]
        shelf = max(shelf, g["h"])
    height = y + shelf
    for g in glyphs:
        if g["sdf"] is None:
            g["x"] = g["y"] = 0
    return height


def write(out, glyphs, atlas, size, sdf_range, metrics, kerns):
    ascent, descent, line_gap = metrics
    ordered = sorted(glyphs, key=lambda g: g["code"])
    header = struct.pack(HEADER, MAGIC, VERSION, atlas.shape[1], atlas.shape[0],
                         float(size), float(sdf_range), ascent, descent, line_gap,
                         len(ordered), len(kerns))
    body = bytearray(header)
    for g in ordered:
        body += struct.pack(GLYPH, g["code"], g["x"], g["y"], g["w"], g["h"],
                            g["ox"], g["oy"], g["advance"])
    for first, second, amount in kerns:
        body += struct.pack(KERN, first, second, amount)
    body += atlas.tobytes()
    out.write_bytes(bytes(body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ttf", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--size", type=float, default=56.0)
    parser.add_argument("--range", dest="sdf_range", type=float, default=8.0)
    parser.add_argument("--scripts", default="latin,cyrillic,greek")
    parser.add_argument("--fallback", default="",
                        help="comma-separated faces to borrow missing glyphs from")
    parser.add_argument("--interface", nargs=2, metavar=("SOURCE", "LANGS"),
                        help="also bake every character psp5's interface can draw: "
                             "every value in PPSSPP's language files under LANGS, "
                             "and psp5's own under LANGS/psp5. SOURCE is kept for "
                             "the old call and is no longer read")
    parser.add_argument("--own", type=pathlib.Path, default=None,
                        help="psp5's own translation files (assets/lang/psp5), whose "
                             "values are baked too")
    args = parser.parse_args()

    scripts = {s.strip() for s in args.scripts.split(",") if s.strip()}
    wanted = set(codepoints(scripts))
    if args.interface:
        langs = pathlib.Path(args.interface[1])
        own = args.own if args.own is not None else langs / "psp5"
        wanted |= interface_codepoints(pathlib.Path(args.interface[0]), langs, own)
    wanted = sorted(wanted)
    fallbacks = [f.strip() for f in args.fallback.split(",") if f.strip()]

    metrics = hhea_metrics(args.ttf, args.size)
    if metrics is None:
        ascent, descent = ImageFont.truetype(str(args.ttf), int(args.size)).getmetrics()
        metrics = (float(ascent), float(-descent), 0.0)

    glyphs, borrowed = bake(args.ttf, args.size, args.sdf_range, wanted, fallbacks)
    height = pack(glyphs, ATLAS_WIDTH)
    atlas = np.zeros((height, ATLAS_WIDTH), dtype=np.uint8)
    for g in glyphs:
        if g["sdf"] is None:
            continue
        atlas[g["y"]:g["y"] + g["h"], g["x"]:g["x"] + g["w"]] = g["sdf"]

    kerns = kerning(args.ttf, args.size, set(wanted))
    write(args.out, glyphs, atlas, args.size, args.sdf_range, metrics, kerns)
    print("%s: %d glyphs (%d borrowed), %d kerns, atlas %dx%d, %d bytes"
          % (args.out.name, len(glyphs), borrowed, len(kerns), ATLAS_WIDTH, height,
             args.out.stat().st_size))


if __name__ == "__main__":
    sys.exit(main())
