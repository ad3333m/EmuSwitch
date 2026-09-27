#!/usr/bin/env python3
# SPDX-FileCopyrightText: Azahar Emulator Project
# Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bakes an anti-aliased glyph atlas for the on-screen overlay from a TrueType font, plus the
# shapes its rounded panels are drawn with, and emits a generated overlay_font.{h,cpp}.
#
# This requires Pillow. Regenerate with:
#   python3 bake_overlay_font.py
# from this directory. The generated files are checked in so the build needs no font tooling.

import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# Inter, the menu's own typeface, so the in-game overlay matches it.
FONT_PATH = os.path.join(HERE, "Inter-SemiBold.ttf")
OUT_DIR = os.path.normpath(os.path.join(HERE, ".."))
FONT_NAME = "Inter-SemiBold.ttf (SIL OFL 1.1)"

BAKE_PX = 48       # Nominal em size to rasterise glyphs at.
ATLAS_W = 512      # Fixed atlas width.
PAD = 2            # Transparent padding around each glyph.
FIRST = 0x20
LAST = 0x7E        # inclusive
WHITE = 4          # Side of the reserved solid white block.
SHAPE_R = 62       # Radius of the baked shapes, in texels.
SHAPE_M = 3        # Margin around each shape, so filtering at its edges stays inside it.
RING_T = 3.0       # Thickness of the ring shape, in texels.


def shape_value(kind, d):
    """Coverage at distance `d` (texels) from a shape's centre."""
    if kind == "corner":  # a solid disc, antialiased at its edge
        return min(max(SHAPE_R - d + 0.5, 0.0), 1.0)
    if kind == "ring":  # a thin circle at the disc's edge
        return min(max(min(SHAPE_R - d + 0.5, d - (SHAPE_R - RING_T) + 0.5), 0.0), 1.0)
    # "shadow": full at the centre, easing out to nothing at the radius
    t = min(d / SHAPE_R, 1.0)
    return (1.0 - t) ** 1.6 * (1.0 - t * t) ** 0.5


def bake_shape(kind):
    """A quarter of the shape, its centre at texel (SHAPE_M, SHAPE_M) of the block."""
    size = SHAPE_R + 2 * SHAPE_M + 1
    img = Image.new("L", (size, size), 0)
    px = img.load()
    for y in range(size):
        for x in range(size):
            # Texels on the centre's side of the margin repeat the centre, so filtering there
            # never pulls in anything else.
            fx = max(x + 0.5 - SHAPE_M, 0.0)
            fy = max(y + 0.5 - SHAPE_M, 0.0)
            # 4x4 supersampling for a clean edge.
            acc = 0.0
            for sy in range(4):
                for sx in range(4):
                    ox = fx + (sx - 1.5) / 4.0
                    oy = fy + (sy - 1.5) / 4.0
                    acc += shape_value(kind, math.hypot(max(ox, 0.0), max(oy, 0.0)))
            px[x, y] = int(round(255 * acc / 16.0))
    return img

SPDX = (
    "// SPDX-FileCopyrightText: Azahar Emulator Project\n"
    "// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)\n"
    "// SPDX-License-Identifier: GPL-2.0-or-later\n"
)


def main():
    font = ImageFont.truetype(FONT_PATH, BAKE_PX)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    # Rasterise each glyph's ink into its own bitmap and record placement.
    glyphs = []  # (code, xadvance, xoff, yoff, w, h, image_or_None)
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        advance = font.getlength(ch)
        bbox = font.getbbox(ch)  # ink box with baseline at y = ascent & pen at x = 0
        if bbox is None:
            bbox = (0, 0, 0, 0)
        left, top, right, bottom = bbox
        w, h = right - left, bottom - top
        if w <= 0 or h <= 0:
            glyphs.append((code, advance, 0.0, 0.0, 0, 0, None))
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-left, -top), ch, fill=255, font=font)
        glyphs.append((code, advance, float(left), float(top), w, h, img))

    # Row-pack the glyphs and append a white block.
    placements = {}  # code -> (x, y)
    x, y, row_h = PAD, PAD, 0
    for code, _, _, _, w, h, img in glyphs:
        if img is None:
            continue
        if x + w + PAD > ATLAS_W:
            x, y, row_h = PAD, y + row_h + PAD, 0
        placements[code] = (x, y)
        x += w + PAD
        row_h = max(row_h, h)
    if x + WHITE + PAD > ATLAS_W:
        x, y, row_h = PAD, y + row_h + PAD, 0
    white_xy = (x, y)
    row_h = max(row_h, WHITE)

    # The shapes get a row of their own.
    shapes = {kind: bake_shape(kind) for kind in ("corner", "shadow", "ring")}
    y, x = y + row_h + PAD, PAD
    shape_xy = {}
    for kind, img in shapes.items():
        shape_xy[kind] = (x, y)
        x += img.width + PAD
    atlas_h = y + max(img.height for img in shapes.values()) + PAD

    atlas = Image.new("L", (ATLAS_W, atlas_h), 0)
    for code, _, _, _, w, h, img in glyphs:
        if img is not None:
            atlas.paste(img, placements[code])
    atlas.paste(255, (white_xy[0], white_xy[1], white_xy[0] + WHITE, white_xy[1] + WHITE))
    for kind, img in shapes.items():
        atlas.paste(img, shape_xy[kind])
    # Each shape by the atlas coordinates of its centre.
    shape_uv = {kind: ((sx + SHAPE_M) / ATLAS_W, (sy + SHAPE_M) / atlas_h)
                for kind, (sx, sy) in shape_xy.items()}

    pixels = list(atlas.tobytes())
    white_u = (white_xy[0] + WHITE * 0.5) / ATLAS_W
    white_v = (white_xy[1] + WHITE * 0.5) / atlas_h

    write_header(ascent, descent, line_height, atlas_h, white_u, white_v, shape_uv)
    write_source(glyphs, placements, atlas_h, pixels)
    print(f"Baked {len(glyphs)} glyphs into {ATLAS_W}x{atlas_h} atlas "
          f"({len(pixels)} bytes) from {FONT_NAME}.")


def write_header(ascent, descent, line_height, atlas_h, white_u, white_v, shape_uv):
    count = LAST - FIRST + 1
    with open(os.path.join(OUT_DIR, "overlay_font.h"), "w") as f:
        f.write(SPDX)
        f.write(f"""
// Generated by font/bake_overlay_font.py from {FONT_NAME}. Do not edit by hand.
//
// See the above file for more information.

#pragma once

namespace Vulkan::OverlayFont {{

struct Glyph {{
    float u0, v0, u1, v1;      // Atlas texture coordinates.
    float xoff, yoff;          // Bake-pixel offset from the pen to the quad.
    float w, h;                // Quad size in bake pixels.
    float xadvance;            // Horizontal pen advance in bake pixels.
}};

constexpr int kFirstChar = {FIRST};
constexpr int kLastChar = {LAST};
constexpr int kGlyphCount = {count};

constexpr int kAtlasWidth = {ATLAS_W};
constexpr int kAtlasHeight = {atlas_h};

constexpr float kBakePixelHeight = {BAKE_PX:.1f};
constexpr float kAscent = {ascent:.1f};
constexpr float kDescent = {descent:.1f};
constexpr float kLineHeight = {line_height:.1f};

constexpr float kWhiteU = {white_u:.6f}f;
constexpr float kWhiteV = {white_v:.6f}f;

// Shapes for rounded panels, each a quarter with its centre at (U, V) and a radius of
// kShapeRadiusU / kShapeRadiusV in atlas coordinates: a solid disc for corners, a soft falloff
// for shadows and a thin ring (kRingThickness of the radius) for outlines.
constexpr float kShapeRadiusU = {SHAPE_R / ATLAS_W:.6f}f;
constexpr float kShapeRadiusV = {SHAPE_R / atlas_h:.6f}f;
constexpr float kRingThickness = {RING_T / SHAPE_R:.6f}f;
constexpr float kCornerU = {shape_uv["corner"][0]:.6f}f;
constexpr float kCornerV = {shape_uv["corner"][1]:.6f}f;
constexpr float kShadowU = {shape_uv["shadow"][0]:.6f}f;
constexpr float kShadowV = {shape_uv["shadow"][1]:.6f}f;
constexpr float kRingU = {shape_uv["ring"][0]:.6f}f;
constexpr float kRingV = {shape_uv["ring"][1]:.6f}f;

extern const Glyph kGlyphs[kGlyphCount];
extern const unsigned char kAtlas[kAtlasWidth * kAtlasHeight];

// Returns the glyph for a character folding anything outside the baked range to space.
inline const Glyph& GlyphFor(char c) {{
    const int code = static_cast<unsigned char>(c);
    const int index = (code < kFirstChar || code > kLastChar) ? 0 : code - kFirstChar;
    return kGlyphs[index];
}}

}} // namespace Vulkan::OverlayFont
""")


def write_source(glyphs, placements, atlas_h, pixels):
    with open(os.path.join(OUT_DIR, "overlay_font.cpp"), "w") as f:
        f.write(SPDX)
        f.write(f"""
// Generated by font/bake_overlay_font.py from {FONT_NAME}. Do not edit by hand.
// See the above file for more information.

#include "video_core/renderer_vulkan/overlay_font.h"

namespace Vulkan::OverlayFont {{

const Glyph kGlyphs[kGlyphCount] = {{
""")
        for code, advance, xoff, yoff, w, h, img in glyphs:
            if img is None:
                u0 = v0 = u1 = v1 = 0.0
            else:
                px, py = placements[code]
                u0, v0 = px / ATLAS_W, py / atlas_h
                u1, v1 = (px + w) / ATLAS_W, (py + h) / atlas_h
            ch = chr(code)
            note = "space" if ch == " " else ch
            f.write(
                f"    {{{u0:.6f}f, {v0:.6f}f, {u1:.6f}f, {v1:.6f}f, "
                f"{xoff:.1f}f, {yoff:.1f}f, {float(w):.1f}f, {float(h):.1f}f, "
                f"{advance:.3f}f}}, // '{note}'\n"
            )
        f.write("};\n\n")
        f.write("const unsigned char kAtlas[kAtlasWidth * kAtlasHeight] = {\n")
        line = "   "
        for value in pixels:
            token = f" {value},"
            if len(line) + len(token) > 100:
                f.write(line + "\n")
                line = "   "
            line += token
        if line.strip():
            f.write(line + "\n")
        f.write("};\n\n} // namespace Vulkan::OverlayFont\n")


if __name__ == "__main__":
    main()
