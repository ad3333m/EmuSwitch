// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu's software canvas and FreeType text, split out of menu.cpp so the same
// drawing code also builds on a desktop to render preview screenshots.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace SwitchFrontend::Gfx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The panel.
constexpr int kPanelW = 1280;
constexpr int kPanelH = 720;

constexpr u32 MakeColor(u8 r, u8 g, u8 b, u8 a = 0xFF) {
    return (u32{a} << 24) | (u32{b} << 16) | (u32{g} << 8) | u32{r};
}

class Canvas {
public:
    Canvas() : pixels(static_cast<std::size_t>(kPanelW) * kPanelH) {}

    u32* Data() {
        return pixels.data();
    }

    int Width() const {
        return width;
    }

    int Height() const {
        return height;
    }

    void Resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, 0);
    }

    void Clear(u32 color) {
        std::fill(pixels.begin(), pixels.end(), color);
    }

    void Blend(int x, int y, u32 color, u8 coverage) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
            return;
        }
        const u32 a = ((color >> 24) & 0xFF) * coverage / 255;
        u32& dst = pixels[static_cast<std::size_t>(y) * width + x];
        if (a == 0) {
            return;
        }
        if (a >= 0xFF) {
            dst = (dst & 0xFF000000u) | (color & 0x00FFFFFFu);
            return;
        }
        const u32 inv = 255 - a;
        const u32 sr = color & 0xFF, sg = (color >> 8) & 0xFF, sb = (color >> 16) & 0xFF;
        const u32 dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF;
        const u32 rr = (sr * a + dr * inv) / 255;
        const u32 rg = (sg * a + dg * inv) / 255;
        const u32 rb = (sb * a + db * inv) / 255;
        dst = MakeColor(static_cast<u8>(rr), static_cast<u8>(rg), static_cast<u8>(rb));
    }

    void FillRect(int x, int y, int w, int h, u32 color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(width, x + w), y1 = std::min(height, y + h);
        const u8 alpha = (color >> 24) & 0xFF;
        for (int yy = y0; yy < y1; ++yy) {
            if (alpha >= 0xFF) {
                std::fill_n(pixels.data() + static_cast<std::size_t>(yy) * width + x0, x1 - x0,
                            color);
            } else {
                for (int xx = x0; xx < x1; ++xx) {
                    Blend(xx, yy, color, alpha);
                }
            }
        }
    }

    void FillRoundRect(int x, int y, int w, int h, int r, u32 color) {
        r = std::clamp(r, 0, std::min(w, h) / 2);
        for (int row = 0; row < h; ++row) {
            int cut = 0;
            if (row < r) {
                const int t = r - 1 - row;
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            } else if (row >= h - r) {
                const int t = row - (h - r);
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            }
            FillRect(x + cut, y + row, w - 2 * cut, 1, color);
        }
    }

    void RoundBorder(int x, int y, int w, int h, int r, int thickness, u32 border, u32 inner) {
        FillRoundRect(x, y, w, h, r, border);
        FillRoundRect(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                      std::max(0, r - thickness), inner);
    }

    void BlitIcon(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size) {
        if (icon.empty() || src_size <= 0) {
            return;
        }
        for (int oy = 0; oy < dst_size; ++oy) {
            const float sy = (oy + 0.5f) * src_size / dst_size - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src_size - 1);
            const int y1 = std::min(y0 + 1, src_size - 1);
            const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
            for (int ox = 0; ox < dst_size; ++ox) {
                const float sx = (ox + 0.5f) * src_size / dst_size - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src_size - 1);
                const int x1 = std::min(x0 + 1, src_size - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const u32 c00 = icon[y0 * src_size + x0], c10 = icon[y0 * src_size + x1];
                const u32 c01 = icon[y1 * src_size + x0], c11 = icon[y1 * src_size + x1];
                float ch[3];
                for (int i = 0; i < 3; ++i) {
                    const int s = i * 8;
                    const float top = ((c00 >> s) & 0xFF) * (1 - fx) + ((c10 >> s) & 0xFF) * fx;
                    const float bot = ((c01 >> s) & 0xFF) * (1 - fx) + ((c11 >> s) & 0xFF) * fx;
                    ch[i] = top * (1 - fy) + bot * fy;
                }
                const int px = dx + ox, py = dy + oy;
                if (px >= 0 && py >= 0 && px < width && py < height) {
                    pixels[static_cast<std::size_t>(py) * width + px] = MakeColor(
                        static_cast<u8>(ch[0]), static_cast<u8>(ch[1]), static_cast<u8>(ch[2]));
                }
            }
        }
    }

    // ---- anti-aliased shapes (the skin's rounded cards, rings, gradients and shadows) ----

    // Copies a full-screen image, e.g. the pre-rendered backdrop.
    void CopyFrom(const std::vector<u32>& src) {
        if (src.size() == pixels.size()) {
            std::memcpy(pixels.data(), src.data(), src.size() * sizeof(u32));
        }
    }

    // Coverage (0..1) of pixel (px, py) inside a rounded rect with float edges.
    static float RoundCoverage(float px, float py, float x, float y, float w, float h, float r) {
        const float cx = std::clamp(px, x + r, x + w - r);
        const float cy = std::clamp(py, y + r, y + h - r);
        const float dx = px - cx, dy = py - cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        // Straight edges: distance to the edge; corners: distance to the arc.
        float inside;
        if (d > 0.0f) {
            inside = r - d;
        } else {
            inside = std::min(std::min(px - x, x + w - px), std::min(py - y, y + h - py));
        }
        return std::clamp(inside + 0.5f, 0.0f, 1.0f);
    }

    // Rounded rect with smooth edges; `color_at(t)` gives the colour at vertical position t (0..1).
    template <typename F>
    void FillRoundAAWith(int x, int y, int w, int h, int r, F&& color_at) {
        if (w <= 0 || h <= 0) {
            return;
        }
        r = std::clamp(r, 0, std::min(w, h) / 2);
        const int y0 = std::max(0, y), y1 = std::min(height, y + h);
        const int x0 = std::max(0, x), x1 = std::min(width, x + w);
        for (int yy = y0; yy < y1; ++yy) {
            const u32 color = color_at(h > 1 ? float(yy - y) / float(h - 1) : 0.0f);
            const u8 alpha = (color >> 24) & 0xFF;
            const bool corner_row = yy < y + r || yy >= y + h - r;
            for (int xx = x0; xx < x1; ++xx) {
                u8 cov = 255;
                if (corner_row && (xx < x + r || xx >= x + w - r)) {
                    cov = static_cast<u8>(255.0f * RoundCoverage(xx + 0.5f, yy + 0.5f, float(x), float(y),
                                                                 float(w), float(h), float(r)));
                }
                if (cov == 255 && alpha == 255) {
                    pixels[static_cast<std::size_t>(yy) * width + xx] = color;
                } else if (cov) {
                    Blend(xx, yy, color, cov);
                }
            }
        }
    }

    void FillRoundAA(int x, int y, int w, int h, int r, u32 color) {
        FillRoundAAWith(x, y, w, h, r, [color](float) { return color; });
    }

    void FillRoundGradient(int x, int y, int w, int h, int r, u32 top, u32 bottom) {
        FillRoundAAWith(x, y, w, h, r, [top, bottom](float t) { return Mix(top, bottom, t); });
    }

    // A smooth outline `t` pixels thick; `color_at(fx)` gives the colour at horizontal
    // position fx (0..1), which is how the focus ring gets its gradient.
    template <typename F>
    void RingRoundAAWith(int x, int y, int w, int h, int r, float t, F&& color_at) {
        const int y0 = std::max(0, y), y1 = std::min(height, y + h);
        const int x0 = std::max(0, x), x1 = std::min(width, x + w);
        const float ir = std::max(0.0f, r - t);
        for (int yy = y0; yy < y1; ++yy) {
            const bool edge_row = yy < y + r + 1 || yy >= y + h - r - 1 || yy < y + t + 1 || yy >= y + h - t - 1;
            for (int xx = x0; xx < x1; ++xx) {
                if (!edge_row && xx >= x + t + 1 && xx < x + w - t - 1) {
                    xx = x + w - static_cast<int>(t) - 2;
                    continue;
                }
                const float px = xx + 0.5f, py = yy + 0.5f;
                const float outer = RoundCoverage(px, py, float(x), float(y), float(w), float(h), float(r));
                const float inner = RoundCoverage(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ir);
                const float cov = std::clamp(outer - inner, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    Blend(xx, yy, color_at(w > 1 ? float(xx - x) / float(w - 1) : 0.0f),
                          static_cast<u8>(cov * 255.0f));
                }
            }
        }
    }

    void RingRoundAA(int x, int y, int w, int h, int r, float t, u32 color) {
        RingRoundAAWith(x, y, w, h, r, t, [color](float) { return color; });
    }

    // Soft shadow under a card: stacked rounded rects fading outwards.
    void SoftShadow(int x, int y, int w, int h, int r, int spread, int drop, u8 strength) {
        for (int i = spread; i >= 1; i -= 2) {
            const u8 a = static_cast<u8>(strength * (spread - i + 1) / (spread + 1) / 4);
            FillRoundAA(x - i, y - i + drop, w + 2 * i, h + 2 * i, r + i, (u32{a} << 24));
        }
    }

    // Draws an RGBA icon scaled (bilinear) into a rounded square with smooth corners.
    void BlitIconRounded(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size, int r) {
        if (icon.empty() || src_size <= 0) {
            return;
        }
        for (int oy = 0; oy < dst_size; ++oy) {
            const float sy = (oy + 0.5f) * src_size / dst_size - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src_size - 1);
            const int y1 = std::min(y0 + 1, src_size - 1);
            const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
            for (int ox = 0; ox < dst_size; ++ox) {
                const float sx = (ox + 0.5f) * src_size / dst_size - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src_size - 1);
                const int x1 = std::min(x0 + 1, src_size - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const u32 c00 = icon[y0 * src_size + x0], c10 = icon[y0 * src_size + x1];
                const u32 c01 = icon[y1 * src_size + x0], c11 = icon[y1 * src_size + x1];
                float ch[3];
                for (int i = 0; i < 3; ++i) {
                    const int sh = i * 8;
                    const float top = ((c00 >> sh) & 0xFF) * (1 - fx) + ((c10 >> sh) & 0xFF) * fx;
                    const float bot = ((c01 >> sh) & 0xFF) * (1 - fx) + ((c11 >> sh) & 0xFF) * fx;
                    ch[i] = top * (1 - fy) + bot * fy;
                }
                const u32 color = MakeColor(static_cast<u8>(ch[0]), static_cast<u8>(ch[1]), static_cast<u8>(ch[2]));
                const float cov = RoundCoverage(ox + 0.5f, oy + 0.5f, 0, 0, float(dst_size), float(dst_size), float(r));
                if (cov > 0.0f) {
                    Blend(dx + ox, dy + oy, color, static_cast<u8>(cov * 255.0f));
                }
            }
        }
    }

    static u32 Mix(u32 a, u32 b, float t) {
        auto ch = [&](int s) {
            const float va = float((a >> s) & 0xFF), vb = float((b >> s) & 0xFF);
            return u32(va + (vb - va) * t + 0.5f) << s;
        };
        return ch(0) | ch(8) | ch(16) | ch(24);
    }

private:
    std::vector<u32> pixels;
    int width = kPanelW;
    int height = kPanelH;
};

class Font {
public:
    // `primary` is a TTF on the romfs (or disk, for previews); the console's shared fonts
    // follow as fallbacks for anything it lacks.
    bool Init(const char* primary = nullptr) {
        if (initialised) {
            return valid;
        }
        initialised = true;
        if (FT_Init_FreeType(&library) != 0) {
            return false;
        }
        if (primary) {
            AddFileFace(primary);
        }
#ifdef __SWITCH__
        if (R_FAILED(plInitialize(PlServiceType_User))) {
            valid = !faces.empty();
            return valid;
        }
        pl_open = true;
        AddSharedFace(PlSharedFontType_Standard);
        AddSharedFace(PlSharedFontType_ChineseSimplified);
        AddSharedFace(PlSharedFontType_ExtChineseSimplified);
        AddSharedFace(PlSharedFontType_KO);
#endif
        valid = !faces.empty();
        return valid;
    }

    void Shutdown() {
        if (!initialised) {
            return;
        }
        cache.clear();
        for (FT_Face face : faces) {
            FT_Done_Face(face);
        }
        faces.clear();
        if (library) {
            FT_Done_FreeType(library);
            library = nullptr;
        }
#ifdef __SWITCH__
        if (pl_open) {
            plExit();
            pl_open = false;
        }
#endif
        initialised = false;
        valid = false;
    }

    int Draw(Canvas& canvas, int x, int baseline, std::string_view text, int size, u32 color) {
        int pen = x;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            if (!g) {
                continue;
            }
            const int gx = pen + g->left;
            const int gy = baseline - g->top;
            for (int row = 0; row < g->h; ++row) {
                for (int col = 0; col < g->w; ++col) {
                    canvas.Blend(gx + col, gy + row, color, g->coverage[row * g->w + col]);
                }
            }
            pen += g->advance;
        }
        return pen - x;
    }

    int Measure(std::string_view text, int size) {
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            if (const Glyph* g = GetGlyph(cp, size)) {
                w += g->advance;
            }
        }
        return w;
    }

    std::string Truncate(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::string out;
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t start = i;
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            const int adv = g ? g->advance : 0;
            if (w + adv + ell > maxw) {
                break;
            }
            out.append(text.substr(start, i - start));
            w += adv;
        }
        out.append("…");
        return out;
    }

    std::string TruncateFront(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::size_t i = 0;
        while (i < text.size()) {
            DecodeUtf8(text, i);
            const std::string_view tail = text.substr(i);
            if (ell + Measure(tail, size) <= maxw) {
                return "…" + std::string{tail};
            }
        }
        return "…";
    }

private:
    struct Glyph {
        int w{}, h{}, left{}, top{}, advance{};
        std::vector<u8> coverage;
    };

    void AddFileFace(const char* path) {
        FT_Face face{};
        if (FT_New_Face(library, path, 0, &face) == 0) {
            faces.push_back(face);
        }
    }

#ifdef __SWITCH__
    void AddSharedFace(PlSharedFontType type) {
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, type))) {
            return;
        }
        FT_Face face{};
        if (FT_New_Memory_Face(library, static_cast<const FT_Byte*>(data.address),
                               static_cast<FT_Long>(data.size), 0, &face) == 0) {
            faces.push_back(face);
        }
    }
#endif

    const Glyph* GetGlyph(u32 cp, int size) {
        const u64 key = (static_cast<u64>(size) << 32) | cp;
        if (auto it = cache.find(key); it != cache.end()) {
            return &it->second;
        }
        FT_Face face = faces.empty() ? nullptr : faces.front();
        for (FT_Face candidate : faces) {
            if (FT_Get_Char_Index(candidate, cp) != 0) {
                face = candidate;
                break;
            }
        }
        if (!face) {
            return nullptr;
        }
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        // NO_AUTOHINT keeps rendering on the font's native TrueType hinter.
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_AUTOHINT) != 0) {
            return nullptr;
        }
        const FT_GlyphSlot slot = face->glyph;
        Glyph g;
        g.w = static_cast<int>(slot->bitmap.width);
        g.h = static_cast<int>(slot->bitmap.rows);
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.advance = static_cast<int>(slot->advance.x >> 6);
        g.coverage.resize(static_cast<std::size_t>(g.w) * g.h);
        for (int row = 0; row < g.h; ++row) {
            std::memcpy(g.coverage.data() + row * g.w,
                        slot->bitmap.buffer + row * slot->bitmap.pitch, g.w);
        }
        return &cache.emplace(key, std::move(g)).first->second;
    }

    static u32 DecodeUtf8(std::string_view s, std::size_t& i) {
        const u8 c = static_cast<u8>(s[i++]);
        if (c < 0x80) {
            return c;
        }
        int extra = 0;
        u32 cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return '?';
        }
        for (int k = 0; k < extra && i < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<u8>(s[i++]) & 0x3F);
        }
        return cp;
    }

    bool initialised{};
    bool valid{};
    bool pl_open{};
    FT_Library library{};
    std::vector<FT_Face> faces;
    std::unordered_map<u64, Glyph> cache;
};


inline int CenterBaseline(int y, int h, int size) {
    return y + (h + static_cast<int>(size * 0.7f)) / 2;
}

} // namespace SwitchFrontend::Gfx
