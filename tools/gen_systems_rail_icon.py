"""Appends kSystems (a 32x32 gamepad outline coverage mask) to rail_icons.h, in the
same style as the Lucide masks: a ~2px stroke.

    python tools/gen_systems_rail_icon.py
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 16
N = 32 * S
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(HERE, "src", "citra_switch", "rail_icons.h")


def P(x, y):
    return (x * S, y * S)


def silhouette():
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)

    def capsule(a, b, r):
        (ax, ay), (bx, by) = a, b
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / L * r, dx / L * r
        d.polygon([P(ax + nx, ay + ny), P(bx + nx, by + ny), P(bx - nx, by - ny), P(ax - nx, ay - ny)], fill=255)
        d.ellipse([P(ax - r, ay - r), P(ax + r, ay + r)], fill=255)
        d.ellipse([P(bx - r, by - r), P(bx + r, by + r)], fill=255)

    capsule((10, 14), (22, 14), 7.5)
    capsule((10.5, 16), (8, 23.5), 5)
    capsule((21.5, 16), (24, 23.5), 5)
    return m.filter(ImageFilter.GaussianBlur(S * 1.2)).point(lambda v: 255 if v > 118 else 0)


def main():
    outer = np.array(silhouette(), dtype=np.int32)
    inner = np.array(Image.fromarray(outer.astype(np.uint8)).filter(ImageFilter.MinFilter(int(2 * S) * 2 + 1)), dtype=np.int32)
    ring = np.clip(outer - inner, 0, 255).astype(np.uint8)
    img = Image.fromarray(ring)
    d = ImageDraw.Draw(img)
    # D-pad and two buttons.
    d.rounded_rectangle([P(8.2, 12.9), P(13.8, 15.1)], radius=S, fill=255)
    d.rounded_rectangle([P(9.9, 11.2), P(12.1, 16.8)], radius=S, fill=255)
    d.ellipse([P(18.6, 11.4), P(21.0, 13.8)], fill=255)
    d.ellipse([P(21.2, 14.2), P(23.6, 16.6)], fill=255)
    mask = np.array(img.resize((32, 32), Image.LANCZOS), dtype=np.uint8)

    src = open(TARGET, encoding="utf-8").read()
    if "kSystems" in src:
        start = src.index("// gamepad (EmuSwitch)")
        end = src.index("};", start) + 3
        src = src[:start] + src[end:]
    lines = ["// gamepad (EmuSwitch)", "constexpr std::uint8_t kSystems[kSize * kSize] = {"]
    flat = [str(int(v)) for v in mask.flatten()]
    for i in range(0, len(flat), 31):
        lines.append("    " + ", ".join(flat[i:i + 31]) + ",")
    lines.append("};")
    block = "\n".join(lines) + "\n\n"
    close = src.rindex("} // namespace")
    src = src[:close] + block + src[close:]
    open(TARGET, "w", encoding="utf-8", newline="\n").write(src)
    Image.fromarray(mask).resize((256, 256), Image.NEAREST).save(os.path.join(HERE, "tools", "systems_icon_preview.png"))
    print("wrote kSystems")


if __name__ == "__main__":
    main()
