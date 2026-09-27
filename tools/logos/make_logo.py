#!/usr/bin/env python3
# SPDX-FileCopyrightText: EmuSwitch
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Turns a console logo (PNG/WebP/JPEG, transparent or on white) into the version the Home
# screen shows: cropped to the logo, on a transparent background, with black and grey
# lettering made light so it reads on the dark menu. Coloured parts keep their colour.
#   python3 make_logo.py <input> src/citra_switch/assets/logos/<console id>.png
# Console ids: 3ds ds gba gb nes snes n64 ps1 ps2 psp wiiu

import sys

from PIL import Image


def main():
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size
    # A logo on white: make the white see-through (colour to alpha against white).
    opaque = sum(1 for y in range(0, h, 4) for x in range(0, w, 4) if px[x, y][3] > 250)
    samples = ((w + 3) // 4) * ((h + 3) // 4)
    if opaque > samples * 0.9:
        for y in range(h):
            for x in range(w):
                r, g, b, _ = px[x, y]
                a = max(255 - r, 255 - g, 255 - b)
                if a == 0:
                    px[x, y] = (0, 0, 0, 0)
                    continue
                k = 255.0 / a
                px[x, y] = tuple(int(round(255 - (255 - c) * k)) for c in (r, g, b)) + (a,)
    # Crop to the logo with a small margin.
    box = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
    if box:
        m = 4
        im = im.crop((max(0, box[0] - m), max(0, box[1] - m), min(w, box[2] + m), min(h, box[3] + m)))
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            lum = (r * 299 + g * 587 + b * 114) // 1000
            if max(r, g, b) - min(r, g, b) < 60 and lum < 180:
                # Black and grey lettering turns light; lighter greys stay as they are.
                v = int(round(255 - lum * 0.32))
                px[x, y] = (v, v, v, a)
    im.save(dst, optimize=True)
    print(f"{dst}: {im.width}x{im.height}")


if __name__ == "__main__":
    main()
