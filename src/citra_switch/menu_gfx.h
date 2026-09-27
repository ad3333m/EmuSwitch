// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu's software canvas and FreeType text, split out of menu.cpp so the same
// drawing code also builds on a desktop to render preview screenshots.
//
// A frame is drawn in horizontal bands on several cores at once: every band gets its own
// Canvas view (same pixels, its own clip), and the whole scene is drawn into each view.
// Everything here is therefore safe to call from several threads as long as the views'
// clips don't overlap; the caches below lock internally.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

constexpr u32 WithAlpha(u32 color, u8 a) {
    return (color & 0x00FFFFFFu) | (u32{a} << 24);
}

// x / 255 for x in [0, 65534].
constexpr u32 Div255(u32 x) {
    return (x + 1 + (x >> 8)) >> 8;
}

// `s` over `d` with weight `a` in [0, 256]; the result is opaque.
inline u32 Lerp256(u32 d, u32 s, u32 a) {
    const u32 rb = (((s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * (256 - a)) >> 8) & 0x00FF00FFu;
    const u32 g = (((s & 0x0000FF00u) * a + (d & 0x0000FF00u) * (256 - a)) >> 8) & 0x0000FF00u;
    return rb | g | 0xFF000000u;
}

// An RGBA picture. `opaque` is true when every pixel's alpha is 255.
struct Image {
    int w = 0;
    int h = 0;
    std::vector<u32> px;
    bool opaque = true;

    bool Empty() const {
        return w <= 0 || h <= 0 || px.empty();
    }
};

// A coverage-only picture: icons and shadows, tinted when drawn.
struct Mask {
    int w = 0;
    int h = 0;
    std::vector<u8> a;
};

class Canvas {
public:
    Canvas() {
        Resize(kPanelW, kPanelH);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) = default;
    Canvas& operator=(Canvas&&) = default;

    u32* Data() {
        return px_;
    }
    const u32* Data() const {
        return px_;
    }
    int Width() const {
        return w_;
    }
    int Height() const {
        return h_;
    }

    // Only on the canvas that owns its pixels.
    void Resize(int w, int h) {
        w_ = w;
        h_ = h;
        storage_.assign(static_cast<std::size_t>(w) * h, 0xFF000000u);
        px_ = storage_.data();
        cx0_ = cy0_ = 0;
        cx1_ = w;
        cy1_ = h;
    }

    // A view of rows [y0, y1) that shares these pixels: one band of a parallel frame.
    Canvas View(int y0, int y1) {
        Canvas v(ViewTag{});
        v.px_ = px_;
        v.w_ = w_;
        v.h_ = h_;
        v.cx0_ = 0;
        v.cx1_ = w_;
        v.cy0_ = std::clamp(y0, 0, h_);
        v.cy1_ = std::clamp(y1, v.cy0_, h_);
        return v;
    }

    int ClipX0() const {
        return cx0_;
    }
    int ClipX1() const {
        return cx1_;
    }
    int ClipY0() const {
        return cy0_;
    }
    int ClipY1() const {
        return cy1_;
    }
    bool ClipEmpty() const {
        return cx0_ >= cx1_ || cy0_ >= cy1_;
    }
    // Whether rows [y, y + h) (in drawing coordinates) touch the clip at all.
    bool RowsVisible(int y, int h) const {
        return y + oy_ < cy1_ && y + oy_ + h > cy0_;
    }

    // Narrows the clip to a rectangle (drawing coordinates) until the scope ends.
    class ClipScope {
    public:
        ClipScope(Canvas& c, int x, int y, int w, int h)
            : c_{c}, x0_{c.cx0_}, y0_{c.cy0_}, x1_{c.cx1_}, y1_{c.cy1_} {
            c.cx0_ = std::max(c.cx0_, x + c.ox_);
            c.cy0_ = std::max(c.cy0_, y + c.oy_);
            c.cx1_ = std::min(c.cx1_, x + c.ox_ + w);
            c.cy1_ = std::min(c.cy1_, y + c.oy_ + h);
        }
        ~ClipScope() {
            c_.cx0_ = x0_;
            c_.cy0_ = y0_;
            c_.cx1_ = x1_;
            c_.cy1_ = y1_;
        }
        ClipScope(const ClipScope&) = delete;
        ClipScope& operator=(const ClipScope&) = delete;

    private:
        Canvas& c_;
        int x0_, y0_, x1_, y1_;
    };

    // Moves everything drawn until the scope ends by (dx, dy).
    class OffsetScope {
    public:
        OffsetScope(Canvas& c, int dx, int dy) : c_{c}, dx_{dx}, dy_{dy} {
            c.ox_ += dx;
            c.oy_ += dy;
        }
        ~OffsetScope() {
            c_.ox_ -= dx_;
            c_.oy_ -= dy_;
        }
        OffsetScope(const OffsetScope&) = delete;
        OffsetScope& operator=(const OffsetScope&) = delete;

    private:
        Canvas& c_;
        int dx_, dy_;
    };

    // Fades everything drawn until the scope ends; `opacity` is 0..1 and stacks.
    class FadeScope {
    public:
        FadeScope(Canvas& c, float opacity) : c_{c}, saved_{c.op_} {
            const u32 o = static_cast<u32>(std::clamp(opacity, 0.0f, 1.0f) * 256.0f + 0.5f);
            c.op_ = (c.op_ * o + 128) >> 8;
        }
        ~FadeScope() {
            c_.op_ = saved_;
        }
        FadeScope(const FadeScope&) = delete;
        FadeScope& operator=(const FadeScope&) = delete;

    private:
        Canvas& c_;
        u32 saved_;
    };

    bool Invisible() const {
        return op_ == 0 || ClipEmpty();
    }

    void Clear(u32 color);

    // One pixel, in drawing coordinates.
    void Blend(int x, int y, u32 color, u8 coverage) {
        x += ox_;
        y += oy_;
        if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) {
            return;
        }
        Put(px_ + static_cast<std::size_t>(y) * w_ + x, color, coverage);
    }

    // One premultiplied-alpha pixel, in drawing coordinates.
    void BlendPremul(int x, int y, u32 p) {
        x += ox_;
        y += oy_;
        if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) {
            return;
        }
        PutPremul(px_ + static_cast<std::size_t>(y) * w_ + x, p);
    }

    void FillRect(int x, int y, int w, int h, u32 color);
    void FillRoundRect(int x, int y, int w, int h, int r, u32 color) {
        FillRoundAA(x, y, w, h, r, color);
    }
    void RoundBorder(int x, int y, int w, int h, int r, int thickness, u32 border, u32 inner) {
        FillRoundAA(x, y, w, h, r, border);
        FillRoundAA(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                    std::max(0, r - thickness), inner);
    }

    // Square RGBA icon (e.g. a 3DS SMDH icon) scaled with bilinear filtering.
    void BlitIcon(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size) {
        BlitIconRounded(icon, src_size, dx, dy, dst_size, 0);
    }
    void BlitIconRounded(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size,
                         int r);

    // Copies a full-canvas image, e.g. a pre-rendered backdrop.
    void CopyFrom(const std::vector<u32>& src);

    // For a row `dy` from a corner's centre (radius r): pixels left of column ArcPartial(r, dy)
    // (counted from the rect's edge) are uncovered, pixels from ArcSolid(r, dy) on are fully
    // covered, and the ones in between are partly covered.
    static int ArcPartial(float r, float dy) {
        const float out = (r + 0.5f) * (r + 0.5f) - dy * dy;
        return out <= 0.0f ? static_cast<int>(r) : std::max(0, static_cast<int>(std::floor(r - std::sqrt(out) - 0.5f)));
    }
    static int ArcSolid(float r, float dy) {
        const float in = (r - 0.5f) * (r - 0.5f) - dy * dy;
        return in <= 0.0f ? static_cast<int>(std::ceil(r)) : std::max(0, static_cast<int>(std::ceil(r - std::sqrt(in) + 0.5f)));
    }

    // Coverage (0..1) of pixel centre (px, py) inside a rounded rect with float edges.
    static float RoundCoverage(float px, float py, float x, float y, float w, float h, float r) {
        // max/min rather than clamp: with r == h / 2 rounding can cross the bounds.
        const float cx = std::max(x + r, std::min(px, x + w - r));
        const float cy = std::max(y + r, std::min(py, y + h - r));
        const float dx = px - cx, dy = py - cy;
        const float d2 = dx * dx + dy * dy;
        float inside;
        if (d2 > 0.0f) {
            inside = r - std::sqrt(d2);
        } else {
            inside = std::min(std::min(px - x, x + w - px), std::min(py - y, y + h - py));
        }
        return std::clamp(inside + 0.5f, 0.0f, 1.0f);
    }

    // Rounded rect with smooth corners; `color_at(t)` gives the colour at vertical position t (0..1).
    template <typename F>
    void FillRoundAAWith(int x, int y, int w, int h, int r, F&& color_at) {
        if (w <= 0 || h <= 0 || Invisible()) {
            return;
        }
        x += ox_;
        y += oy_;
        r = std::clamp(r, 0, std::min(w, h) / 2);
        const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + h);
        const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + w);
        if (x0 >= x1) {
            return;
        }
        for (int yy = y0; yy < y1; ++yy) {
            const u32 color = color_at(h > 1 ? float(yy - y) / float(h - 1) : 0.0f);
            u32* row = px_ + static_cast<std::size_t>(yy) * w_;
            const bool corner_row = yy < y + r || yy >= y + h - r;
            int inner0 = x0, inner1 = x1;
            if (corner_row) {
                // Only the pixels the arc passes through are partly covered; left of them is
                // empty and right of them is solid.
                const float fy = yy + 0.5f;
                const float dy = std::fabs(fy - (yy < y + r ? y + r : y + h - r));
                const int part = ArcPartial(r, dy);
                const int solid = ArcSolid(r, dy);
                inner0 = std::max(x0, x + solid);
                inner1 = std::min(x1, x + w - solid);
                for (int xx = std::max(x0, x + part); xx < std::min(inner0, x1); ++xx) {
                    const float cov = RoundCoverage(xx + 0.5f, fy, float(x), float(y), float(w), float(h), float(r));
                    Put(row + xx, color, static_cast<u8>(cov * 255.0f + 0.5f));
                }
                for (int xx = std::max(x0, inner1); xx < std::min(x1, x + w - part); ++xx) {
                    const float cov = RoundCoverage(xx + 0.5f, fy, float(x), float(y), float(w), float(h), float(r));
                    Put(row + xx, color, static_cast<u8>(cov * 255.0f + 0.5f));
                }
            }
            if (inner0 < inner1) {
                FillSpan(row, inner0, inner1, color);
            }
        }
    }

    void FillRoundAA(int x, int y, int w, int h, int r, u32 color) {
        FillRoundAAWith(x, y, w, h, r, [color](float) { return color; });
    }

    // Rounded rect whose colour can change per pixel: `color_at(fx, fy)` with both in 0..1.
    template <typename F>
    void FillRoundAAPix(int x, int y, int w, int h, int r, F&& color_at) {
        if (w <= 0 || h <= 0 || Invisible()) {
            return;
        }
        x += ox_;
        y += oy_;
        r = std::clamp(r, 0, std::min(w, h) / 2);
        const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + h);
        const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + w);
        const float iw = w > 1 ? 1.0f / float(w - 1) : 0.0f, ih = h > 1 ? 1.0f / float(h - 1) : 0.0f;
        for (int yy = y0; yy < y1; ++yy) {
            u32* row = px_ + static_cast<std::size_t>(yy) * w_;
            const bool corner_row = yy < y + r || yy >= y + h - r;
            const float fy = float(yy - y) * ih;
            for (int xx = x0; xx < x1; ++xx) {
                u32 cov = 255;
                if (corner_row && (xx < x + r || xx >= x + w - r)) {
                    cov = static_cast<u32>(RoundCoverage(xx + 0.5f, yy + 0.5f, float(x), float(y), float(w),
                                                         float(h), float(r)) * 255.0f + 0.5f);
                    if (cov == 0) {
                        continue;
                    }
                }
                const u32 color = color_at(float(xx - x) * iw, fy);
                if (color >> 24) {
                    Put(row + xx, color, cov);
                }
            }
        }
    }

    void FillRoundGradient(int x, int y, int w, int h, int r, u32 top, u32 bottom) {
        FillRoundAAWith(x, y, w, h, r, [top, bottom](float t) { return Mix(top, bottom, t); });
    }

    // A smooth outline `t` pixels thick; `color_at(fx)` gives the colour at horizontal
    // position fx (0..1), which is how the focus ring gets its gradient.
    template <typename F>
    void RingRoundAAWith(int x, int y, int w, int h, int r, float t, F&& color_at) {
        if (w <= 0 || h <= 0 || Invisible()) {
            return;
        }
        x += ox_;
        y += oy_;
        r = std::clamp(r, 0, std::min(w, h) / 2);
        const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + h);
        const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + w);
        const float ir = std::max(0.0f, r - t);
        const int thick = static_cast<int>(std::ceil(t)) + 1;
        if (x0 >= x1) {
            return;
        }
        // Colours per column, worked out once.
        const float inv_w = w > 1 ? 1.0f / float(w - 1) : 0.0f;
        std::vector<u32> lut(static_cast<std::size_t>(x1 - x0));
        for (int xx = x0; xx < x1; ++xx) {
            lut[static_cast<std::size_t>(xx - x0)] = color_at(float(xx - x) * inv_w);
        }
        const u32* col = lut.data() - x0;
        // A ring thicker than the shape is the whole shape.
        const bool inner_empty = w - 2 * t <= 0.0f || h - 2 * t <= 0.0f;
        for (int yy = y0; yy < y1; ++yy) {
            u32* row = px_ + static_cast<std::size_t>(yy) * w_;
            const float py = yy + 0.5f;
            auto coverage = [&](int xx) {
                const float px = xx + 0.5f;
                const float outer = RoundCoverage(px, py, float(x), float(y), float(w), float(h), float(r));
                const float inner =
                    inner_empty ? 0.0f : RoundCoverage(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ir);
                return std::clamp(outer - inner, 0.0f, 1.0f);
            };
            auto plot = [&](int a, int b) {
                for (int xx = std::max(a, x0); xx < std::min(b, x1); ++xx) {
                    const float cov = coverage(xx);
                    if (cov > 0.0f) {
                        Put(row + xx, col[xx], static_cast<u8>(cov * 255.0f + 0.5f));
                    }
                }
            };
            // Columns [from, to) from each side can vary pixel by pixel; in between, both the
            // outer and the inner shape are on their straight top/bottom edges, so the whole
            // span shares one coverage.
            int from = 0, to = thick;
            if (r > 0 && (py < y + r || py > y + h - r)) {
                const float dy = std::fabs(py - (py < y + r ? y + r : y + h - r));
                from = ArcPartial(float(r), dy);
                to = std::max(to, ArcSolid(float(r), dy) + 1);
            }
            if (!inner_empty && py > y + t - 1.0f && py < y + h - t + 1.0f) {
                int solid = thick;
                if (py < y + r + 0.5f || py > y + h - r - 0.5f) {
                    const float dy = std::fabs(py - (py < y + r + 0.5f ? y + r : y + h - r));
                    solid = static_cast<int>(std::ceil(t)) + (ir > 0.0f ? ArcSolid(ir, dy) : 1) + 1;
                    // Rows along the inner rect's top/bottom edge are partly covered all the way.
                    if (py < y + t + 1.0f || py > y + h - t - 1.0f) {
                        solid = std::max(solid, r + thick);
                    }
                }
                to = std::max(to, solid);
            }
            to = std::min(to, (w + 1) / 2);
            from = std::min(from, to);
            plot(x + from, x + to);
            plot(x + std::max(w - to, to), x + w - from);
            if (to < w - to) {
                const float cov = coverage(x + w / 2);
                if (cov > 0.0f) {
                    const u8 c8 = static_cast<u8>(cov * 255.0f + 0.5f);
                    for (int xx = std::max(x + to, x0); xx < std::min(x + w - to, x1); ++xx) {
                        Put(row + xx, col[xx], c8);
                    }
                }
            }
        }
    }

    void RingRoundAA(int x, int y, int w, int h, int r, float t, u32 color) {
        RingRoundAAWith(x, y, w, h, r, t, [color](float) { return color; });
    }

    // Soft shadow under a rounded card. `spread` is how far it fades out, `drop` moves it down,
    // `strength` is its darkest alpha. The card itself is assumed to cover its own rect, so
    // that part is skipped.
    void SoftShadow(int x, int y, int w, int h, int r, int spread, int drop, u8 strength) {
        Glow(x, y, w, h, r, spread, MakeColor(0, 0, 0, strength), true, drop);
    }
    // A soft halo around a rounded rect in any colour (a coloured shadow or a glow), moved
    // down by `drop`. With `skip_inside` the rect itself is left alone, since a card will cover it.
    void Glow(int x, int y, int w, int h, int r, int spread, u32 color, bool skip_inside, int drop = 0);

    // An image at 1:1, optionally with rounded corners of radius r.
    void DrawImage(const Image& img, int x, int y, int r = 0);
    // An image scaled into (x, y, w, h) with bilinear filtering and optional rounded corners.
    void DrawImageScaled(const Image& img, float x, float y, float w, float h, int r = 0);
    // A coverage mask tinted with `color`.
    void DrawMask(const Mask& m, int x, int y, u32 color);
    // A premultiplied-alpha sprite (see RenderSprite) at 1:1.
    void DrawSprite(const Image& img, int x, int y);

    void Disc(float cx, float cy, float r, u32 color);
    void Circle(float cx, float cy, float r, float thickness, u32 color);

    static u32 Mix(u32 a, u32 b, float t) {
        const u32 k = static_cast<u32>(std::clamp(t, 0.0f, 1.0f) * 256.0f + 0.5f);
        const u32 rb = (((b & 0x00FF00FFu) * k + (a & 0x00FF00FFu) * (256 - k)) >> 8) & 0x00FF00FFu;
        const u32 ga = ((((b >> 8) & 0x00FF00FFu) * k + ((a >> 8) & 0x00FF00FFu) * (256 - k)) >> 8) &
                       0x00FF00FFu;
        return rb | (ga << 8);
    }

private:
    struct ViewTag {};
    explicit Canvas(ViewTag) {}

    // Blends one pixel with coverage `cov` (0..255), applying the colour's alpha and the fade.
    void Put(u32* dst, u32 color, u32 cov) {
        const u32 a = Div255((color >> 24) * cov);
        const u32 a256 = (a * op_ + 128) >> 8;
        const u32 w = a256 + (a256 >> 7);
        if (w == 0) {
            return;
        }
        *dst = w >= 256 ? (color | 0xFF000000u) : Lerp256(*dst, color, w);
    }

    void PutPremul(u32* dst, u32 s) {
        if (op_ < 256) {
            const u32 rb = (((s & 0x00FF00FFu) * op_) >> 8) & 0x00FF00FFu;
            const u32 ga = (((s >> 8) & 0x00FF00FFu) * op_ >> 8) & 0x00FF00FFu;
            s = rb | (ga << 8);
        }
        const u32 a = s >> 24;
        if (a == 0) {
            return;
        }
        if (a == 255) {
            *dst = s;
            return;
        }
        // dst * (1 - a) in two packed lanes; with s premultiplied (s_c <= a) every channel of
        // the sum stays within 255, so the lanes never carry into each other.
        const u32 d = *dst, inv = 256 - (a + (a >> 7));
        const u32 rb = (((d & 0x00FF00FFu) * inv) >> 8) & 0x00FF00FFu;
        const u32 g = (((d & 0x0000FF00u) * inv) >> 8) & 0x0000FF00u;
        *dst = ((s & 0x00FFFFFFu) + rb + g) | 0xFF000000u;
    }

    void FillSpan(u32* row, int x0, int x1, u32 color) {
        const u32 a = ((color >> 24) * op_ + 128) >> 8;
        const u32 w = a + (a >> 7);
        if (w == 0) {
            return;
        }
        if (w >= 256) {
            std::fill(row + x0, row + x1, color | 0xFF000000u);
            return;
        }
        for (int x = x0; x < x1; ++x) {
            row[x] = Lerp256(row[x], color, w);
        }
    }

    std::vector<u32> storage_;
    u32* px_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0; // clip, canvas coordinates
    int ox_ = 0, oy_ = 0;                       // drawing origin
    u32 op_ = 256;                              // fade, 0..256
};

class Font {
public:
    // `primary` is a TTF on the romfs (or disk, for previews); the console's shared fonts
    // follow as fallbacks for anything it lacks.
    bool Init(const char* primary = nullptr);
    void Shutdown();

    int Draw(Canvas& canvas, int x, int baseline, std::string_view text, int size, u32 color);
    int Measure(std::string_view text, int size);
    std::string Truncate(std::string_view text, int size, int maxw);
    std::string TruncateFront(std::string_view text, int size, int maxw);

private:
    struct Glyph {
        int w{}, h{}, left{}, top{}, advance{};
        std::vector<u8> coverage;
    };

    void AddFileFace(const char* path);
#ifdef __SWITCH__
    void AddSharedFace(PlSharedFontType type);
#endif
    const Glyph* GetGlyph(u32 cp, int size);
    static u32 DecodeUtf8(std::string_view s, std::size_t& i);

    bool initialised{};
    bool valid{};
    bool pl_open{};
    FT_Library library{};
    std::vector<FT_Face> faces;
    std::mutex mutex;
    std::unordered_map<u64, Glyph> cache;
};

inline int CenterBaseline(int y, int h, int size) {
    return y + (h + static_cast<int>(size * 0.7f)) / 2;
}

// ---- pictures ------------------------------------------------------------------------------

// High-quality resize (Lanczos-3, premultiplied alpha): sharp when enlarging, alias-free
// when shrinking.
Image Resize(const Image& src, int w, int h);
// Resize to fill (w, h), cropping the longer side.
Image ResizeCover(const Image& src, int w, int h);
// Box-blurs in place (three passes approximate a Gaussian).
void BoxBlur(Image& img, int radius);
// Average colour of the opaque parts, saturated a little; `fallback` when there are none.
u32 AverageColor(const Image& img, u32 fallback);
// Draws `draw` into a w x h premultiplied-alpha sprite: it is drawn over black and over white,
// and the difference gives each pixel's coverage. The origin is (ox, oy) inside the sprite.
Image RenderSprite(int w, int h, int ox, int oy, const std::function<void(Canvas&)>& draw);

// ---- parallel frames -----------------------------------------------------------------------

// A small pool that runs the bands of a frame on the other cores while the calling thread
// takes its share. On the Switch the workers pin themselves to cores 1 and 2 through the hook.
class Workers {
public:
    using StartHook = void (*)(int worker_index);

    static Workers& Get();
    static void SetStartHook(StartHook hook);

    // Calls fn(i) for every i in [0, jobs) across the pool and waits for all of them.
    void Run(int jobs, const std::function<void(int)>& fn);
    int Threads() const {
        return static_cast<int>(threads.size()) + 1;
    }
    void Shutdown();

    ~Workers() {
        Shutdown();
    }

private:
    Workers() = default;
    void Start(int workers);
    void Loop(int index);

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    const std::function<void(int)>* job = nullptr;
    int job_count = 0;
    std::atomic<int> next{0};
    u64 generation = 0;
    int finished = 0;
    bool stopping = false;
};

// Number of bands a frame is split into, and the rows of band `i`: multiples of 16 so each
// band maps onto whole tiles of the Switch's block-linear framebuffer in every rotation.
constexpr int kBands = 6;
inline void BandRows(int height, int i, int& y0, int& y1) {
    const int step = ((height + kBands - 1) / kBands + 15) & ~15;
    y0 = std::min(height, i * step);
    y1 = std::min(height, (i + 1) * step);
}

// Writes rows [y0, y1) of `c` into a 1280x720 RGBA8888 block-linear framebuffer (16-GOB
// blocks, as libnx creates them), rotating by `rotation` degrees clockwise. `stride` is the
// framebuffer's pitch in bytes.
void WriteBlockLinear(const Canvas& c, int y0, int y1, int rotation, u8* fb, u32 stride);

} // namespace SwitchFrontend::Gfx
