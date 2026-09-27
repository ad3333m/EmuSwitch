"""EmuSwitch's homebrew-menu icon (256x256 JPEG): a soft pastel gradient with a dark
controller whose four face buttons are coloured.

    python tools/make_emuswitch_icon.py [out.jpg]
"""

import math
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 4                 # supersampling
N = 256 * S


def mesh_background():
    # Four soft colour fields blended by distance, like a mesh gradient.
    corners = [((0.0, 0.0), (199, 184, 255)), ((1.0, 0.0), (158, 231, 224)),
               ((0.0, 1.0), (255, 214, 165)), ((1.0, 1.0), (184, 242, 201))]
    small = 64
    im = Image.new("RGB", (small, small))
    px = im.load()
    for y in range(small):
        for x in range(small):
            u, v = x / (small - 1), y / (small - 1)
            wsum, acc = 0.0, [0.0, 0.0, 0.0]
            for (cx, cy), col in corners:
                d = math.hypot(u - cx, v - cy)
                w = 1.0 / (d * d + 0.08)
                wsum += w
                for i in range(3):
                    acc[i] += col[i] * w
            px[x, y] = tuple(int(a / wsum) for a in acc)
    return im.resize((N, N), Image.BICUBIC).filter(ImageFilter.GaussianBlur(N * 0.02))


def P(x, y):
    return (x * S, y * S)


def capsule(d, a, b, r):
    """Filled capsule from point a to b with radius r (grid units)."""
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy) or 1.0
    nx, ny = -dy / L * r, dx / L * r
    d.polygon([P(ax + nx, ay + ny), P(bx + nx, by + ny), P(bx - nx, by - ny), P(ax - nx, ay - ny)], fill=255)
    d.ellipse([P(ax - r, ay - r), P(ax + r, ay + r)], fill=255)
    d.ellipse([P(bx - r, by - r), P(bx + r, by + r)], fill=255)


def controller_mask():
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    # Body: a wide capsule; grips: angled capsules; then blur + threshold melts
    # the joins into smooth fillets.
    capsule(d, (74, 124), (182, 124), 36)
    capsule(d, (78, 132), (62, 184), 26)
    capsule(d, (178, 132), (194, 184), 26)
    m = m.filter(ImageFilter.GaussianBlur(S * 7)).point(lambda v: 255 if v > 118 else 0)
    return m.filter(ImageFilter.GaussianBlur(S * 0.7))


def main(out):
    bg = mesh_background().convert("RGBA")
    body = controller_mask()

    # Shadow under the controller.
    shadow = body.filter(ImageFilter.GaussianBlur(N * 0.03)).point(lambda v: v * 0.35)
    shade = Image.new("RGBA", (N, N), (40, 30, 70, 255))
    shade.putalpha(shadow)
    bg.alpha_composite(shade, (0, int(N * 0.02)))

    # Dark body with a gentle top-lit gradient.
    grad = Image.new("RGBA", (N, N))
    gd = ImageDraw.Draw(grad)
    for y in range(N):
        t = y / N
        c = (int(46 - 18 * t), int(46 - 18 * t), int(58 - 20 * t), 255)
        gd.line([(0, y), (N, y)], fill=c)
    grad.putalpha(body)
    bg.alpha_composite(grad)

    d = ImageDraw.Draw(bg)
    # D-pad, knocked out in the background's light tone.
    light = (236, 236, 244, 255)
    d.rounded_rectangle([P(66, 117), P(100, 131)], radius=3 * S, fill=light)
    d.rounded_rectangle([P(76, 107), P(90, 141)], radius=3 * S, fill=light)
    # Four face buttons in a diamond.
    buttons = [((182, 106), (59, 130, 246)), ((199, 123), (239, 68, 68)),
               ((182, 140), (34, 197, 94)), ((165, 123), (245, 158, 11))]
    for (x, y), col in buttons:
        r = 8.5
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=col + (255,))
        d.ellipse([P(x - r * 0.45, y - r * 0.7), P(x + r * 0.1, y - r * 0.2)], fill=(255, 255, 255, 90))
    # Select / start.
    d.rounded_rectangle([P(113, 120), P(125, 127)], radius=3 * S, fill=(120, 120, 140, 255))
    d.rounded_rectangle([P(131, 120), P(143, 127)], radius=3 * S, fill=(120, 120, 140, 255))

    icon = bg.convert("RGB").resize((256, 256), Image.LANCZOS)
    icon.save(out, quality=95)
    print("wrote", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "icon.jpg")
