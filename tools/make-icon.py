#!/usr/bin/env python3
"""Draw sce_sys/icon0.png, the title's icon on the console's home screen.

    python3 tools/make-icon.py [--out sce_sys/icon0.png]

The console wants 512x512 PNG, 8 bits a channel, RGB with no alpha - the format
PS5_VulkanTemplate's own icon0.png uses, and what this writes.

The drawing is a generic handheld: a wide body, a screen, a d-pad and four face
buttons. It is deliberately not anyone's logo or product shape, and carries no
PlayStation or Sony mark; the only wording is the project's own name, PSP5.

The PSP and PS5 wordmarks are set in Sony's own typefaces, which are not here and
are not psp5's to ship. --psp-font and --five-font take a path each if you have
them; without them both halves are set in the system's bold sans.

Everything is drawn at 4x and downsampled, which is what gives the curves and the
small shapes clean edges - PIL has no antialiased drawing of its own.
"""

import argparse
import pathlib
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("error: this needs Pillow (pip install Pillow)")

SIZE = 512
SCALE = 4
S = SIZE * SCALE

# A cool, dark palette: the icon sits on the console's own dark home screen, so the
# background stays dark and the device carries the contrast.
BACKDROP_TOP = (10, 18, 38)
BACKDROP_BOTTOM = (22, 46, 92)
GLOW = (46, 104, 190)
BODY = (232, 237, 244)
BODY_EDGE = (188, 197, 211)
BEZEL = (28, 34, 48)
SCREEN_TOP = (64, 214, 255)
SCREEN_BOTTOM = (22, 94, 255)
CONTROL = (74, 84, 102)
TEXT = (240, 245, 252)


def vertical_gradient(size, top, bottom):
    """A one-pixel-wide column stretched out: far cheaper than filling per pixel."""
    column = Image.new("RGB", (1, size))
    pixels = column.load()
    for y in range(size):
        t = y / (size - 1)
        pixels[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return column.resize((size, size), Image.BILINEAR)


def radial_glow(size, colour, radius):
    """A soft light behind the device, as a mask blurred by resampling."""
    small = 64
    mask = Image.new("L", (small, small), 0)
    draw = ImageDraw.Draw(mask)
    centre = small / 2
    steps = 24
    for i in range(steps, 0, -1):
        r = radius * (i / steps) * small / size
        value = round(150 * (1 - i / steps) ** 2)
        draw.ellipse(
            (centre - r, centre - r * 0.62, centre + r, centre + r * 0.62), fill=value
        )
    mask = mask.resize((size, size), Image.BICUBIC)
    layer = Image.new("RGB", (size, size), colour)
    return layer, mask


def load_font(points, path=None):
    """The font to set the wordmark in; `path` wins when it is given."""
    if path:
        try:
            return ImageFont.truetype(path, points)
        except OSError:
            print("warning: cannot read %s, falling back" % path, file=sys.stderr)
    """A bold face for the wordmark, from whichever platform is running this."""
    candidates = [
        "C:/Windows/Fonts/segoeuib.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    ]
    for path in candidates:
        if pathlib.Path(path).exists():
            return ImageFont.truetype(path, points)
    # Not fatal: the device is the icon, the word is a label on it.
    print("warning: no bold font found, the wordmark will be small", file=sys.stderr)
    return ImageFont.load_default()


def draw_icon(psp_font_path=None, five_font_path=None):
    image = vertical_gradient(S, BACKDROP_TOP, BACKDROP_BOTTOM)
    glow, mask = radial_glow(S, GLOW, 0.92)
    image = Image.composite(glow, image, mask)

    draw = ImageDraw.Draw(image)
    centre_x = S / 2
    centre_y = S * 0.44

    # The body: wide, with ends rounded far more than the top and bottom, which is
    # what reads as "handheld" rather than "tablet" at icon size.
    body_w, body_h = S * 0.78, S * 0.345
    body = (
        centre_x - body_w / 2,
        centre_y - body_h / 2,
        centre_x + body_w / 2,
        centre_y + body_h / 2,
    )
    draw.rounded_rectangle(body, radius=body_h * 0.46, fill=BODY, outline=BODY_EDGE,
                           width=max(2, int(S * 0.004)))

    # The bezel, then the screen inside it.
    bezel_w, bezel_h = S * 0.40, S * 0.245
    bezel = (
        centre_x - bezel_w / 2,
        centre_y - bezel_h / 2,
        centre_x + bezel_w / 2,
        centre_y + bezel_h / 2,
    )
    draw.rounded_rectangle(bezel, radius=S * 0.022, fill=BEZEL)

    inset = S * 0.012
    screen_box = (bezel[0] + inset, bezel[1] + inset, bezel[2] - inset, bezel[3] - inset)
    screen_w = int(screen_box[2] - screen_box[0])
    screen_h = int(screen_box[3] - screen_box[1])
    screen = vertical_gradient(max(screen_w, screen_h), SCREEN_TOP, SCREEN_BOTTOM)
    screen = screen.resize((screen_w, screen_h), Image.BILINEAR)
    screen_mask = Image.new("L", (screen_w, screen_h), 0)
    ImageDraw.Draw(screen_mask).rounded_rectangle(
        (0, 0, screen_w - 1, screen_h - 1), radius=S * 0.012, fill=255
    )
    image.paste(screen, (int(screen_box[0]), int(screen_box[1])), screen_mask)

    # A diagonal sheen across the screen, so it reads as glass rather than a panel
    # of flat colour.
    sheen = Image.new("L", (screen_w, screen_h), 0)
    ImageDraw.Draw(sheen).polygon(
        [(0, screen_h), (screen_w * 0.46, 0), (screen_w * 0.72, 0), (0, screen_h)],
        fill=38,
    )
    image.paste(Image.new("RGB", (screen_w, screen_h), (255, 255, 255)),
                (int(screen_box[0]), int(screen_box[1])),
                Image.composite(sheen, Image.new("L", (screen_w, screen_h), 0), screen_mask))

    # The d-pad, left of the screen.
    pad_cx = centre_x - S * 0.295
    arm, thick = S * 0.052, S * 0.021
    draw.rounded_rectangle(
        (pad_cx - arm, centre_y - thick, pad_cx + arm, centre_y + thick),
        radius=thick * 0.45, fill=CONTROL)
    draw.rounded_rectangle(
        (pad_cx - thick, centre_y - arm, pad_cx + thick, centre_y + arm),
        radius=thick * 0.45, fill=CONTROL)

    # Four face buttons, right of the screen, in the usual diamond.
    btn_cx = centre_x + S * 0.295
    spread, r = S * 0.047, S * 0.019
    for dx, dy in ((0, -spread), (0, spread), (-spread, 0), (spread, 0)):
        draw.ellipse((btn_cx + dx - r, centre_y + dy - r,
                      btn_cx + dx + r, centre_y + dy + r), fill=CONTROL)

    # The wordmark: PSP5, drawn rather than set.
    #
    # Its letters are single-weight strokes - two bracket shapes around a
    # zigzag - which no ordinary typeface has, so setting them in one looked
    # nothing like it. The 5 is built the same way, out of the same strokes at
    # the same weight, so it belongs to the other three rather than sitting
    # beside them in someone else's face. Drawing them also keeps the icon free
    # of anyone's font file.
    letter_h = S * 0.085
    stroke = max(2.0, letter_h * 0.085)
    letter_w = letter_h * 1.70
    gap = letter_w * 0.26
    top = S * 0.695
    bottom = top + letter_h

    def bar(x0, y0, x1, y1):
        draw.rectangle((x0, y0, x1, y1), fill=TEXT)

    def letter_p(x0):
        """Top bar, a short stem down its right, the bar back, then a long left stem."""
        waist = top + letter_h * 0.45
        bar(x0, top, x0 + letter_w, top + stroke)
        bar(x0 + letter_w - stroke, top, x0 + letter_w, waist)
        bar(x0, waist - stroke, x0 + letter_w, waist)
        bar(x0, waist - stroke, x0 + stroke, bottom)

    def letter_s(x0):
        """A full-height stem down the middle, a bar right at the top and left at
        the foot - the zigzag between the two Ps."""
        cx = x0 + letter_w * 0.5
        bar(cx - stroke * 0.5, top, cx + stroke * 0.5, bottom)
        bar(cx - stroke * 0.5, top, x0 + letter_w, top + stroke)
        bar(x0, bottom - stroke, cx + stroke * 0.5, bottom)

    def digit_five(x0):
        """The same strokes as the letters: top bar, down the left, across, down
        the right, and along the foot."""
        waist = top + letter_h * 0.5
        bar(x0, top, x0 + letter_w, top + stroke)
        bar(x0, top, x0 + stroke, waist)
        bar(x0, waist - stroke, x0 + letter_w, waist)
        bar(x0 + letter_w - stroke, waist - stroke, x0 + letter_w, bottom)
        bar(x0, bottom - stroke, x0 + letter_w, bottom)

    glyphs = (letter_p, letter_s, letter_p, digit_five)
    total = letter_w * len(glyphs) + gap * (len(glyphs) - 1)
    x = centre_x - total / 2
    for glyph in glyphs:
        glyph(x)
        x += letter_w + gap

    # Down to the console's size in one step, which is where the edges get clean.
    return image.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="sce_sys/icon0.png", type=pathlib.Path)
    parser.add_argument("--psp-font", default=None,
                        help="a .ttf for the PSP half of the wordmark")
    parser.add_argument("--five-font", default=None,
                        help="a .ttf for the 5")
    args = parser.parse_args()

    icon = draw_icon(args.psp_font, args.five_font).convert("RGB")  # No alpha: the console wants colour type 2.
    args.out.parent.mkdir(parents=True, exist_ok=True)
    icon.save(args.out, "PNG", optimize=True)
    print(f"wrote {args.out}: {icon.width}x{icon.height}, {args.out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
