// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/menu_gfx.h"

#include <array>

namespace SwitchFrontend::Gfx {
namespace {

// Linear blend of all four channels, k in [0, 256].
inline u32 Lerp4(u32 a, u32 b, u32 k) {
    const u32 rb = (((b & 0x00FF00FFu) * k + (a & 0x00FF00FFu) * (256 - k)) >> 8) & 0x00FF00FFu;
    const u32 ga =
        ((((b >> 8) & 0x00FF00FFu) * k + ((a >> 8) & 0x00FF00FFu) * (256 - k)) >> 8) & 0x00FF00FFu;
    return rb | (ga << 8);
}

// ---- glow masks: one per card size, shared by every card of that size ----

std::mutex g_glow_mutex;
std::unordered_map<u64, std::shared_ptr<const Mask>> g_glow_cache;

std::shared_ptr<const Mask> GlowMask(int w, int h, int r, int spread) {
    const u64 key = (u64(u16(w)) << 48) | (u64(u16(h)) << 32) | (u64(u16(r)) << 16) | u64(u16(spread));
    {
        std::lock_guard lock{g_glow_mutex};
        if (auto it = g_glow_cache.find(key); it != g_glow_cache.end()) {
            return it->second;
        }
    }
    auto m = std::make_shared<Mask>();
    m->w = w + 2 * spread;
    m->h = h + 2 * spread;
    m->a.resize(std::size_t(m->w) * m->h);
    const float hw = w * 0.5f, hh = h * 0.5f, fr = float(std::min(r, std::min(w, h) / 2));
    const float inv = spread > 0 ? 1.0f / spread : 1.0f;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            // Signed distance to the rounded rect, centred on the mask.
            const float px = std::fabs(x + 0.5f - m->w * 0.5f) - (hw - fr);
            const float py = std::fabs(y + 0.5f - m->h * 0.5f) - (hh - fr);
            const float ox = std::max(px, 0.0f), oy = std::max(py, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(px, py), 0.0f) - fr;
            float a;
            if (d <= 0.0f) {
                a = 1.0f;
            } else {
                const float t = d * inv;
                a = std::exp(-t * t * 4.5f);
            }
            m->a[std::size_t(y) * m->w + x] = static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    std::lock_guard lock{g_glow_mutex};
    if (g_glow_cache.size() > 48) {
        g_glow_cache.clear();
    }
    return g_glow_cache.emplace(key, std::move(m)).first->second;
}

float Lanczos3(float x) {
    x = std::fabs(x);
    if (x < 1e-6f) {
        return 1.0f;
    }
    if (x >= 3.0f) {
        return 0.0f;
    }
    constexpr float pi = 3.14159265358979f;
    const float a = pi * x, b = a / 3.0f;
    return (std::sin(a) / a) * (std::sin(b) / b);
}

struct Taps {
    int first = 0;
    std::vector<float> w;
};

// Filter taps for resampling `src` samples into `dst`.
std::vector<Taps> MakeTaps(int src, int dst) {
    std::vector<Taps> out(static_cast<std::size_t>(dst));
    const float scale = float(dst) / float(src);
    const float stretch = scale < 1.0f ? 1.0f / scale : 1.0f; // widen the kernel when shrinking
    const float support = 3.0f * stretch;
    for (int i = 0; i < dst; ++i) {
        const float center = (i + 0.5f) / scale;
        const int lo = static_cast<int>(std::floor(center - support));
        const int hi = static_cast<int>(std::ceil(center + support));
        Taps& t = out[static_cast<std::size_t>(i)];
        t.first = std::clamp(lo, 0, src - 1);
        const int last = std::clamp(hi, 0, src - 1);
        t.w.assign(static_cast<std::size_t>(last - t.first + 1), 0.0f);
        float sum = 0.0f;
        for (int j = lo; j <= hi; ++j) {
            const float wgt = Lanczos3((j + 0.5f - center) / stretch);
            if (wgt == 0.0f) {
                continue;
            }
            const int k = std::clamp(j, 0, src - 1) - t.first;
            t.w[static_cast<std::size_t>(k)] += wgt;
            sum += wgt;
        }
        if (sum != 0.0f) {
            for (float& v : t.w) {
                v /= sum;
            }
        }
    }
    return out;
}

} // namespace

// ---- Canvas ----------------------------------------------------------------------------------

void Canvas::Clear(u32 color) {
    if (ClipEmpty()) {
        return;
    }
    for (int y = cy0_; y < cy1_; ++y) {
        u32* row = px_ + static_cast<std::size_t>(y) * w_;
        std::fill(row + cx0_, row + cx1_, color | 0xFF000000u);
    }
}

void Canvas::FillRect(int x, int y, int w, int h, u32 color) {
    if (Invisible()) {
        return;
    }
    x += ox_;
    y += oy_;
    const int x0 = std::max(cx0_, x), y0 = std::max(cy0_, y);
    const int x1 = std::min(cx1_, x + w), y1 = std::min(cy1_, y + h);
    if (x0 >= x1) {
        return;
    }
    for (int yy = y0; yy < y1; ++yy) {
        FillSpan(px_ + static_cast<std::size_t>(yy) * w_, x0, x1, color);
    }
}

void Canvas::CopyFrom(const std::vector<u32>& src) {
    if (src.size() != static_cast<std::size_t>(w_) * h_ || ClipEmpty()) {
        return;
    }
    for (int y = cy0_; y < cy1_; ++y) {
        const std::size_t off = static_cast<std::size_t>(y) * w_ + cx0_;
        std::memcpy(px_ + off, src.data() + off, static_cast<std::size_t>(cx1_ - cx0_) * sizeof(u32));
    }
}

void Canvas::BlitIconRounded(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size,
                             int r) {
    if (icon.size() < static_cast<std::size_t>(src_size) * src_size || src_size <= 0) {
        return;
    }
    Image img;
    img.w = img.h = src_size;
    // Borrowing the pixels would be nicer; icons are small, so the copy is cheap.
    img.px = icon;
    DrawImageScaled(img, float(dx), float(dy), float(dst_size), float(dst_size), r);
}

void Canvas::Glow(int x, int y, int w, int h, int r, int spread, u32 color, bool skip_inside, int drop) {
    if (w <= 0 || h <= 0 || Invisible()) {
        return;
    }
    const int mx = x + ox_ - spread, my = y + oy_ + drop - spread;
    const int mw = w + 2 * spread, mh = h + 2 * spread;
    if (my >= cy1_ || my + mh <= cy0_ || mx >= cx1_ || mx + mw <= cx0_) {
        return;
    }
    const std::shared_ptr<const Mask> mask = GlowMask(w, h, r, spread);
    const int ax = x + ox_, ay = y + oy_; // the card, which isn't moved by `drop`
    r = std::clamp(r, 0, std::min(w, h) / 2);
    const int y0 = std::max(cy0_, my), y1 = std::min(cy1_, my + mh);
    const int x0 = std::max(cx0_, mx), x1 = std::min(cx1_, mx + mw);
    const u8* a = mask->a.data();
    for (int yy = y0; yy < y1; ++yy) {
        u32* row = px_ + static_cast<std::size_t>(yy) * w_;
        const u8* mrow = a + static_cast<std::size_t>(yy - my) * mw - mx;
        // Columns [skip0, skip1) sit under the card itself.
        int skip0 = x1, skip1 = x1;
        if (skip_inside && yy >= ay && yy < ay + h) {
            // The span the card covers completely on this row.
            int solid = 0;
            if (yy < ay + r || yy >= ay + h - r) {
                const float dy = std::fabs(yy + 0.5f - (yy < ay + r ? ay + r : ay + h - r));
                solid = ArcSolid(float(r), dy);
            }
            skip0 = std::clamp(ax + solid, x0, x1);
            skip1 = std::clamp(ax + w - solid, skip0, x1);
        }
        auto span = [&](int from, int to) {
            for (int xx = from; xx < to; ++xx) {
                const u32 m = mrow[xx];
                if (m) {
                    Put(row + xx, color, m);
                }
            }
        };
        span(x0, skip0);
        span(skip1, x1);
    }
}

void Canvas::DrawImage(const Image& img, int x, int y, int r) {
    if (img.Empty() || Invisible()) {
        return;
    }
    x += ox_;
    y += oy_;
    const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + img.h);
    const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + img.w);
    if (x0 >= x1) {
        return;
    }
    r = std::clamp(r, 0, std::min(img.w, img.h) / 2);
    const bool plain = img.opaque && op_ >= 256;
    for (int yy = y0; yy < y1; ++yy) {
        u32* row = px_ + static_cast<std::size_t>(yy) * w_;
        const u32* src = img.px.data() + static_cast<std::size_t>(yy - y) * img.w - x;
        const bool corner_row = r > 0 && (yy < y + r || yy >= y + img.h - r);
        int in0 = x0, in1 = x1;
        if (corner_row) {
            in0 = std::max(x0, x + r);
            in1 = std::min(x1, x + img.w - r);
            for (int xx = x0; xx < x1; ++xx) {
                if (xx == in0 && in0 < in1) {
                    xx = in1 - 1;
                    continue;
                }
                const float cov = RoundCoverage(xx + 0.5f, yy + 0.5f, float(x), float(y), float(img.w),
                                                float(img.h), float(r));
                if (cov > 0.0f) {
                    Put(row + xx, src[xx], static_cast<u32>(cov * 255.0f + 0.5f));
                }
            }
        }
        if (in0 >= in1) {
            continue;
        }
        if (plain) {
            std::memcpy(row + in0, src + in0, static_cast<std::size_t>(in1 - in0) * sizeof(u32));
        } else {
            for (int xx = in0; xx < in1; ++xx) {
                Put(row + xx, src[xx], 255);
            }
        }
    }
}

void Canvas::DrawImageScaled(const Image& img, float x, float y, float w, float h, int r) {
    if (img.Empty() || w < 1.0f || h < 1.0f || Invisible()) {
        return;
    }
    // Snap to whole pixels: the destination rect is what the rounded corners follow.
    const int ix = static_cast<int>(std::lround(x)) + ox_, iy = static_cast<int>(std::lround(y)) + oy_;
    const int iw = std::max(1, static_cast<int>(std::lround(w))), ih = std::max(1, static_cast<int>(std::lround(h)));
    if (iw == img.w && ih == img.h) {
        OffsetScope back{*this, -ox_, -oy_};
        DrawImage(img, ix, iy, r);
        return;
    }
    const int y0 = std::max(cy0_, iy), y1 = std::min(cy1_, iy + ih);
    const int x0 = std::max(cx0_, ix), x1 = std::min(cx1_, ix + iw);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    r = std::clamp(r, 0, std::min(iw, ih) / 2);
    // Per-column source positions, 8 fractional bits.
    std::vector<u32> cols(static_cast<std::size_t>(x1 - x0));
    const float sx = float(img.w) / iw, sy = float(img.h) / ih;
    for (int xx = x0; xx < x1; ++xx) {
        const float fx = std::clamp((xx - ix + 0.5f) * sx - 0.5f, 0.0f, float(img.w - 1));
        const u32 i0 = static_cast<u32>(fx);
        const u32 k = static_cast<u32>((fx - float(i0)) * 256.0f);
        cols[static_cast<std::size_t>(xx - x0)] = (i0 << 9) | k;
    }
    const u32* px = img.px.data();
    for (int yy = y0; yy < y1; ++yy) {
        u32* row = px_ + static_cast<std::size_t>(yy) * w_;
        const float fy = std::clamp((yy - iy + 0.5f) * sy - 0.5f, 0.0f, float(img.h - 1));
        const int j0 = static_cast<int>(fy);
        const int j1 = std::min(j0 + 1, img.h - 1);
        const u32 ky = static_cast<u32>((fy - float(j0)) * 256.0f);
        const u32* r0 = px + static_cast<std::size_t>(j0) * img.w;
        const u32* r1 = px + static_cast<std::size_t>(j1) * img.w;
        const bool corner_row = r > 0 && (yy < iy + r || yy >= iy + ih - r);
        for (int xx = x0; xx < x1; ++xx) {
            const u32 c = cols[static_cast<std::size_t>(xx - x0)];
            const u32 i0 = c >> 9, k = c & 0x1FF;
            const u32 i1 = std::min<u32>(i0 + 1, static_cast<u32>(img.w - 1));
            const u32 top = Lerp4(r0[i0], r0[i1], k);
            const u32 bot = Lerp4(r1[i0], r1[i1], k);
            const u32 col = Lerp4(top, bot, ky);
            u32 cov = 255;
            if (corner_row && (xx < ix + r || xx >= ix + iw - r)) {
                cov = static_cast<u32>(RoundCoverage(xx + 0.5f, yy + 0.5f, float(ix), float(iy), float(iw),
                                                     float(ih), float(r)) * 255.0f + 0.5f);
            }
            if (cov) {
                Put(row + xx, col, cov);
            }
        }
    }
}

void Canvas::DrawMask(const Mask& m, int x, int y, u32 color) {
    if (m.w <= 0 || m.h <= 0 || Invisible()) {
        return;
    }
    x += ox_;
    y += oy_;
    const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + m.h);
    const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + m.w);
    for (int yy = y0; yy < y1; ++yy) {
        u32* row = px_ + static_cast<std::size_t>(yy) * w_;
        const u8* src = m.a.data() + static_cast<std::size_t>(yy - y) * m.w - x;
        for (int xx = x0; xx < x1; ++xx) {
            if (src[xx]) {
                Put(row + xx, color, src[xx]);
            }
        }
    }
}

void Canvas::DrawSprite(const Image& img, int x, int y) {
    if (img.Empty() || Invisible()) {
        return;
    }
    x += ox_;
    y += oy_;
    const int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + img.h);
    const int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + img.w);
    for (int yy = y0; yy < y1; ++yy) {
        u32* row = px_ + static_cast<std::size_t>(yy) * w_;
        const u32* src = img.px.data() + static_cast<std::size_t>(yy - y) * img.w - x;
        for (int xx = x0; xx < x1; ++xx) {
            if (src[xx] >> 24) {
                PutPremul(row + xx, src[xx]);
            }
        }
    }
}

void Canvas::Disc(float cx, float cy, float r, u32 color) {
    if (r <= 0.0f || Invisible()) {
        return;
    }
    cx += ox_;
    cy += oy_;
    const int y0 = std::max(cy0_, static_cast<int>(std::floor(cy - r - 1)));
    const int y1 = std::min(cy1_, static_cast<int>(std::ceil(cy + r + 1)));
    const int x0 = std::max(cx0_, static_cast<int>(std::floor(cx - r - 1)));
    const int x1 = std::min(cx1_, static_cast<int>(std::ceil(cx + r + 1)));
    for (int y = y0; y < y1; ++y) {
        u32* row = px_ + static_cast<std::size_t>(y) * w_;
        const float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - cx;
            const float cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) {
                Put(row + x, color, static_cast<u32>(cov * 255.0f + 0.5f));
            }
        }
    }
}

void Canvas::Circle(float cx, float cy, float r, float thickness, u32 color) {
    if (r <= 0.0f || Invisible()) {
        return;
    }
    cx += ox_;
    cy += oy_;
    const float outer = r + thickness * 0.5f + 1.0f;
    const int y0 = std::max(cy0_, static_cast<int>(std::floor(cy - outer)));
    const int y1 = std::min(cy1_, static_cast<int>(std::ceil(cy + outer)));
    const int x0 = std::max(cx0_, static_cast<int>(std::floor(cx - outer)));
    const int x1 = std::min(cx1_, static_cast<int>(std::ceil(cx + outer)));
    for (int y = y0; y < y1; ++y) {
        u32* row = px_ + static_cast<std::size_t>(y) * w_;
        const float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - cx;
            const float d = std::fabs(std::sqrt(dx * dx + dy * dy) - r);
            const float cov = std::clamp(thickness * 0.5f - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) {
                Put(row + x, color, static_cast<u32>(cov * 255.0f + 0.5f));
            }
        }
    }
}

// ---- Font ------------------------------------------------------------------------------------

bool Font::Init(const char* primary) {
    std::lock_guard lock{mutex};
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

void Font::Shutdown() {
    std::lock_guard lock{mutex};
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

int Font::Draw(Canvas& canvas, int x, int baseline, std::string_view text, int size, u32 color) {
    int pen = x;
    std::size_t i = 0;
    // The glyphs are looked up even when nothing is visible: the first pass of a frame
    // warms the cache before the bands draw in parallel.
    const bool visible = !canvas.Invisible() && canvas.RowsVisible(baseline - size * 2, size * 3);
    while (i < text.size()) {
        const u32 cp = DecodeUtf8(text, i);
        const Glyph* g = GetGlyph(cp, size);
        if (!g) {
            continue;
        }
        if (visible && g->w > 0) {
            const int gx = pen + g->left;
            const int gy = baseline - g->top;
            if (canvas.RowsVisible(gy, g->h)) {
                for (int row = 0; row < g->h; ++row) {
                    const u8* cov = g->coverage.data() + static_cast<std::size_t>(row) * g->w;
                    for (int col = 0; col < g->w; ++col) {
                        if (cov[col]) {
                            canvas.Blend(gx + col, gy + row, color, cov[col]);
                        }
                    }
                }
            }
        }
        pen += g->advance;
    }
    return pen - x;
}

int Font::Measure(std::string_view text, int size) {
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

std::string Font::Truncate(std::string_view text, int size, int maxw) {
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

std::string Font::TruncateFront(std::string_view text, int size, int maxw) {
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

void Font::AddFileFace(const char* path) {
    FT_Face face{};
    if (FT_New_Face(library, path, 0, &face) == 0) {
        faces.push_back(face);
    }
}

#ifdef __SWITCH__
void Font::AddSharedFace(PlSharedFontType type) {
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

const Font::Glyph* Font::GetGlyph(u32 cp, int size) {
    const u64 key = (static_cast<u64>(size) << 32) | cp;
    std::lock_guard lock{mutex};
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
        std::memcpy(g.coverage.data() + static_cast<std::size_t>(row) * g.w,
                    slot->bitmap.buffer + row * slot->bitmap.pitch, static_cast<std::size_t>(g.w));
    }
    // Unordered-map nodes never move, so the pointer outlives the lock.
    return &cache.emplace(key, std::move(g)).first->second;
}

u32 Font::DecodeUtf8(std::string_view s, std::size_t& i) {
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

// ---- pictures ----------------------------------------------------------------------------------

Image Resize(const Image& src, int w, int h) {
    Image out;
    if (src.Empty() || w <= 0 || h <= 0) {
        return out;
    }
    out.w = w;
    out.h = h;
    out.opaque = src.opaque;
    out.px.resize(static_cast<std::size_t>(w) * h);
    if (w == src.w && h == src.h) {
        out.px = src.px;
        return out;
    }
    // Premultiplied float copy so transparent pixels don't bleed their colour into edges.
    std::vector<float> in(static_cast<std::size_t>(src.w) * src.h * 4);
    for (std::size_t i = 0; i < src.px.size(); ++i) {
        const u32 p = src.px[i];
        const float a = float(p >> 24) / 255.0f;
        in[i * 4 + 0] = float(p & 0xFF) * a;
        in[i * 4 + 1] = float((p >> 8) & 0xFF) * a;
        in[i * 4 + 2] = float((p >> 16) & 0xFF) * a;
        in[i * 4 + 3] = a;
    }
    const std::vector<Taps> hx = MakeTaps(src.w, w);
    const std::vector<Taps> vy = MakeTaps(src.h, h);
    std::vector<float> mid(static_cast<std::size_t>(w) * src.h * 4, 0.0f);
    for (int y = 0; y < src.h; ++y) {
        const float* row = in.data() + static_cast<std::size_t>(y) * src.w * 4;
        float* dst = mid.data() + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const Taps& t = hx[static_cast<std::size_t>(x)];
            float acc[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < t.w.size(); ++k) {
                const float* p = row + (static_cast<std::size_t>(t.first) + k) * 4;
                const float wk = t.w[k];
                acc[0] += p[0] * wk;
                acc[1] += p[1] * wk;
                acc[2] += p[2] * wk;
                acc[3] += p[3] * wk;
            }
            std::memcpy(dst + x * 4, acc, sizeof(acc));
        }
    }
    for (int y = 0; y < h; ++y) {
        const Taps& t = vy[static_cast<std::size_t>(y)];
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < t.w.size(); ++k) {
                const float* p = mid.data() + ((static_cast<std::size_t>(t.first) + k) * w + x) * 4;
                const float wk = t.w[k];
                acc[0] += p[0] * wk;
                acc[1] += p[1] * wk;
                acc[2] += p[2] * wk;
                acc[3] += p[3] * wk;
            }
            const float a = std::clamp(acc[3], 0.0f, 1.0f);
            const float inv = a > 1e-4f ? 1.0f / a : 0.0f;
            auto ch = [&](float v) { return static_cast<u32>(std::clamp(v * inv, 0.0f, 255.0f) + 0.5f); };
            const u32 a8 = src.opaque ? 255u : static_cast<u32>(a * 255.0f + 0.5f);
            out.px[static_cast<std::size_t>(y) * w + x] = ch(acc[0]) | (ch(acc[1]) << 8) | (ch(acc[2]) << 16) | (a8 << 24);
        }
    }
    return out;
}

Image ResizeCover(const Image& src, int w, int h) {
    if (src.Empty() || w <= 0 || h <= 0) {
        return {};
    }
    // Crop the source to the target's aspect ratio, centred.
    const float want = float(w) / float(h), have = float(src.w) / float(src.h);
    int cw = src.w, ch = src.h;
    if (have > want) {
        cw = std::max(1, static_cast<int>(std::lround(src.h * want)));
    } else if (have < want) {
        ch = std::max(1, static_cast<int>(std::lround(src.w / want)));
    }
    if (cw == src.w && ch == src.h) {
        return Resize(src, w, h);
    }
    Image crop;
    crop.w = cw;
    crop.h = ch;
    crop.opaque = src.opaque;
    crop.px.resize(static_cast<std::size_t>(cw) * ch);
    const int ox = (src.w - cw) / 2, oy = (src.h - ch) / 2;
    for (int y = 0; y < ch; ++y) {
        std::memcpy(crop.px.data() + static_cast<std::size_t>(y) * cw,
                    src.px.data() + static_cast<std::size_t>(y + oy) * src.w + ox,
                    static_cast<std::size_t>(cw) * sizeof(u32));
    }
    return Resize(crop, w, h);
}

void BoxBlur(Image& img, int radius) {
    if (img.Empty() || radius <= 0) {
        return;
    }
    const int w = img.w, h = img.h;
    std::vector<u32> tmp(img.px.size());
    auto pass = [&](const u32* src, u32* dst, int count, int len, int step_in, int step_line) {
        for (int line = 0; line < count; ++line) {
            const u32* s = src + static_cast<std::size_t>(line) * step_line;
            u32* d = dst + static_cast<std::size_t>(line) * step_line;
            int sum[4] = {0, 0, 0, 0};
            auto at = [&](int i) { return s[static_cast<std::size_t>(std::clamp(i, 0, len - 1)) * step_in]; };
            for (int i = -radius; i <= radius; ++i) {
                const u32 p = at(i);
                for (int c = 0; c < 4; ++c) {
                    sum[c] += static_cast<int>((p >> (c * 8)) & 0xFF);
                }
            }
            const int n = 2 * radius + 1;
            for (int i = 0; i < len; ++i) {
                u32 v = 0;
                for (int c = 0; c < 4; ++c) {
                    v |= static_cast<u32>(sum[c] / n) << (c * 8);
                }
                d[static_cast<std::size_t>(i) * step_in] = v;
                const u32 add = at(i + radius + 1), sub = at(i - radius);
                for (int c = 0; c < 4; ++c) {
                    sum[c] += static_cast<int>((add >> (c * 8)) & 0xFF) - static_cast<int>((sub >> (c * 8)) & 0xFF);
                }
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(img.px.data(), tmp.data(), h, w, 1, w);
        pass(tmp.data(), img.px.data(), w, h, w, 1);
    }
}

u32 AverageColor(const Image& img, u32 fallback) {
    if (img.Empty()) {
        return fallback;
    }
    double r = 0, g = 0, b = 0, n = 0;
    const std::size_t step = std::max<std::size_t>(1, img.px.size() / 4096);
    for (std::size_t i = 0; i < img.px.size(); i += step) {
        const u32 p = img.px[i];
        const double a = double(p >> 24) / 255.0;
        // Favour colourful pixels so a grey border doesn't wash the result out.
        const double pr = p & 0xFF, pg = (p >> 8) & 0xFF, pb = (p >> 16) & 0xFF;
        const double sat = std::max({pr, pg, pb}) - std::min({pr, pg, pb});
        const double wgt = a * (0.25 + sat / 255.0);
        r += pr * wgt;
        g += pg * wgt;
        b += pb * wgt;
        n += wgt;
    }
    if (n <= 0.0) {
        return fallback;
    }
    r /= n;
    g /= n;
    b /= n;
    // Push the saturation up a touch and keep it in a range that reads as a glow.
    const double mean = (r + g + b) / 3.0;
    r = mean + (r - mean) * 1.35;
    g = mean + (g - mean) * 1.35;
    b = mean + (b - mean) * 1.35;
    const double peak = std::max({r, g, b, 1.0});
    const double scale = std::clamp(190.0 / peak, 0.6, 2.2);
    auto c8 = [&](double v) { return static_cast<u8>(std::clamp(v * scale, 0.0, 255.0)); };
    return MakeColor(c8(r), c8(g), c8(b));
}

Image RenderSprite(int w, int h, int ox, int oy, const std::function<void(Canvas&)>& draw) {
    Image out;
    if (w <= 0 || h <= 0) {
        return out;
    }
    Canvas black, white;
    black.Resize(w, h);
    white.Resize(w, h);
    black.Clear(MakeColor(0, 0, 0));
    white.Clear(MakeColor(0xFF, 0xFF, 0xFF));
    {
        Canvas::OffsetScope ob{black, ox, oy};
        Canvas::OffsetScope ow{white, ox, oy};
        draw(black);
        draw(white);
    }
    out.w = w;
    out.h = h;
    out.opaque = false;
    out.px.resize(static_cast<std::size_t>(w) * h);
    const u32* b = black.Data();
    const u32* wh = white.Data();
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        // Over black a pixel is a*c; over white it is a*c + (1 - a) * 255.
        const u32 pb = b[i], pw = wh[i];
        int a = 255;
        for (int ch = 0; ch < 3; ++ch) {
            const int cb = static_cast<int>((pb >> (ch * 8)) & 0xFF), cw = static_cast<int>((pw >> (ch * 8)) & 0xFF);
            a = std::min(a, 255 - (cw - cb));
        }
        a = std::clamp(a, 0, 255);
        if (a < 3) {
            // Too faint to see; skipping it keeps drawing the sprite cheap.
            out.px[i] = 0;
            continue;
        }
        u32 r = pb & 0xFF, g = (pb >> 8) & 0xFF, bl = (pb >> 16) & 0xFF;
        r = std::min<u32>(r, static_cast<u32>(a));
        g = std::min<u32>(g, static_cast<u32>(a));
        bl = std::min<u32>(bl, static_cast<u32>(a));
        out.px[i] = r | (g << 8) | (bl << 16) | (static_cast<u32>(a) << 24);
    }
    return out;
}

// ---- Workers ------------------------------------------------------------------------------------

namespace {
Workers::StartHook g_start_hook = nullptr;
}

void Workers::SetStartHook(StartHook hook) {
    g_start_hook = hook;
}

Workers& Workers::Get() {
    static Workers pool;
    if (pool.threads.empty() && !pool.stopping) {
        // The Switch gives an application three cores: this thread keeps one, the workers take
        // the other two.
        pool.Start(2);
    }
    return pool;
}

void Workers::Start(int workers) {
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([this, i] { Loop(i); });
    }
}

void Workers::Loop(int index) {
    if (g_start_hook) {
        g_start_hook(index);
    }
    u64 seen = 0;
    while (true) {
        const std::function<void(int)>* fn;
        int count;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping) {
                return;
            }
            seen = generation;
            fn = job;
            count = job_count;
        }
        for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            (*fn)(i);
        }
        std::lock_guard lock{mutex};
        if (++finished == static_cast<int>(threads.size())) {
            done.notify_one();
        }
    }
}

void Workers::Run(int jobs, const std::function<void(int)>& fn) {
    if (threads.empty()) {
        for (int i = 0; i < jobs; ++i) {
            fn(i);
        }
        return;
    }
    {
        std::lock_guard lock{mutex};
        job = &fn;
        job_count = jobs;
        next.store(0);
        finished = 0;
        ++generation;
    }
    wake.notify_all();
    for (int i = next.fetch_add(1); i < jobs; i = next.fetch_add(1)) {
        fn(i);
    }
    // Every worker has to check in, so none still holds `fn` once this returns.
    std::unique_lock lock{mutex};
    done.wait(lock, [&] { return finished == static_cast<int>(threads.size()); });
    job = nullptr;
}

void Workers::Shutdown() {
    {
        std::lock_guard lock{mutex};
        if (threads.empty()) {
            return;
        }
        stopping = true;
    }
    wake.notify_all();
    for (std::thread& t : threads) {
        t.join();
    }
    threads.clear();
    std::lock_guard lock{mutex};
    stopping = false;
}

// ---- framebuffer -----------------------------------------------------------------------------------

namespace {

// Byte offset of the 8x8-row GOB (gx, gy) in a 16-GOB-high block-linear surface.
inline std::size_t GobOffset(u32 gx, u32 gy, u32 width_gobs) {
    return (static_cast<std::size_t>((gy >> 4) * width_gobs + gx) * 16 + (gy & 15)) * 512;
}

// Offset inside a GOB of the 16-byte unit holding row r (0..7), bytes [u*16, u*16+16).
constexpr std::array<std::array<u32, 4>, 8> kUnit = [] {
    std::array<std::array<u32, 4>, 8> t{};
    for (u32 r = 0; r < 8; ++r) {
        for (u32 u = 0; u < 4; ++u) {
            t[r][u] = (u >> 1) * 256 + (r >> 1) * 64 + (u & 1) * 32 + (r & 1) * 16;
        }
    }
    return t;
}();

} // namespace

void WriteBlockLinear(const Canvas& c, int y0, int y1, int rotation, u8* fb, u32 stride) {
    const u32 width_gobs = stride / 64;
    const u32* src = c.Data();
    const int cw = c.Width(), ch = c.Height();
    if (y0 >= y1) {
        return;
    }
    if (rotation == 0) {
        for (int gy = y0 / 8; gy < (y1 + 7) / 8; ++gy) {
            for (u32 gx = 0; gx < width_gobs && static_cast<int>(gx * 16) < cw; ++gx) {
                u8* gob = fb + GobOffset(gx, static_cast<u32>(gy), width_gobs);
                for (int r = 0; r < 8; ++r) {
                    const int y = gy * 8 + r;
                    if (y >= ch) {
                        break;
                    }
                    const u32* line = src + static_cast<std::size_t>(y) * cw + gx * 16;
                    for (u32 u = 0; u < 4; ++u) {
                        std::memcpy(gob + kUnit[static_cast<std::size_t>(r)][u], line + u * 4, 16);
                    }
                }
            }
        }
        return;
    }
    // The rotated layouts fetch pixel by pixel. fb(xf, yf) comes from `pick`.
    auto write_region = [&](int fx0, int fx1, int fy0, int fy1, auto&& pick) {
        for (int gy = fy0 / 8; gy < (fy1 + 7) / 8; ++gy) {
            for (int gx = fx0 / 16; gx < (fx1 + 15) / 16; ++gx) {
                u8* gob = fb + GobOffset(static_cast<u32>(gx), static_cast<u32>(gy), width_gobs);
                for (int r = 0; r < 8; ++r) {
                    const int yf = gy * 8 + r;
                    if (yf >= kPanelH) {
                        break;
                    }
                    for (u32 u = 0; u < 4; ++u) {
                        u32 quad[4];
                        for (int k = 0; k < 4; ++k) {
                            const int xf = gx * 16 + static_cast<int>(u) * 4 + k;
                            quad[k] = pick(xf, yf);
                        }
                        std::memcpy(gob + kUnit[static_cast<std::size_t>(r)][u], quad, 16);
                    }
                }
            }
        }
    };
    if (rotation == 180) {
        // fb(xf, yf) = canvas(cw - 1 - xf, ch - 1 - yf); these rows come out as fb rows [ch - y1, ch - y0).
        write_region(0, kPanelW, ch - y1, ch - y0, [&](int xf, int yf) {
            return src[static_cast<std::size_t>(ch - 1 - yf) * cw + (cw - 1 - xf)];
        });
    } else if (rotation == 90) {
        // fb(xf, yf) = canvas(yf, ch - 1 - xf): canvas rows [y0, y1) are fb columns [ch - y1, ch - y0).
        write_region(ch - y1, ch - y0, 0, kPanelH, [&](int xf, int yf) {
            return src[static_cast<std::size_t>(ch - 1 - xf) * cw + yf];
        });
    } else {
        // 270: fb(xf, yf) = canvas(cw - 1 - yf, xf): canvas rows [y0, y1) are fb columns [y0, y1).
        write_region(y0, y1, 0, kPanelH, [&](int xf, int yf) {
            return src[static_cast<std::size_t>(xf) * cw + (cw - 1 - yf)];
        });
    }
}

} // namespace SwitchFrontend::Gfx
