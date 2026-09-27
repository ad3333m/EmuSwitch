// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/menu_skin.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace SwitchFrontend::Skin {
namespace {

using namespace Palette;
using namespace Layout;
using Gfx::CenterBaseline;
using Pt = std::pair<float, float>;

u32 Alpha(u32 color, u8 a) {
    return (color & 0x00FFFFFFu) | (u32{a} << 24);
}

// Teal -> blue -> pink, the focus ring's colours; `phase` slowly turns it.
u32 RingAt(float t, float phase = 0.0f) {
    t = std::fmod(t + phase, 1.0f);
    const u32 a = MakeColor(0x40, 0xE2, 0xD6), b = MakeColor(0x78, 0x78, 0xFF), c = MakeColor(0xEC, 0x68, 0xD6);
    if (t < 0.4f) return Canvas::Mix(a, b, t / 0.4f);
    if (t < 0.8f) return Canvas::Mix(b, c, (t - 0.4f) / 0.4f);
    return Canvas::Mix(c, a, (t - 0.8f) / 0.2f);
}

// ---- vector shapes via distance fields -------------------------------------------------

float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float l2 = dx * dx + dy * dy;
    const float t = l2 > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.0f, 1.0f) : 0.0f;
    const float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

// A polyline with round caps and joins, anti-aliased.
void Stroke(Canvas& c, const std::vector<Pt>& pts, float width, u32 color, bool closed = false) {
    if (pts.empty()) return;
    float x0 = pts[0].first, x1 = x0, y0 = pts[0].second, y1 = y0;
    for (const Pt& p : pts) {
        x0 = std::min(x0, p.first); x1 = std::max(x1, p.first);
        y0 = std::min(y0, p.second); y1 = std::max(y1, p.second);
    }
    const float pad = width / 2 + 2;
    const std::size_t n = pts.size();
    for (int y = int(std::floor(y0 - pad)); y <= int(std::ceil(y1 + pad)); ++y) {
        for (int x = int(std::floor(x0 - pad)); x <= int(std::ceil(x1 + pad)); ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float d = 1e9f;
            if (n == 1) {
                d = SegDist(px, py, pts[0].first, pts[0].second, pts[0].first, pts[0].second);
            }
            for (std::size_t i = 0; i + (closed ? 0 : 1) < n; ++i) {
                const Pt& a = pts[i];
                const Pt& b = pts[(i + 1) % n];
                d = std::min(d, SegDist(px, py, a.first, a.second, b.first, b.second));
            }
            const float cov = std::clamp(width / 2 - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0) c.Blend(x, y, color, u8(cov * 255));
        }
    }
}

void Disc(Canvas& c, float cx, float cy, float r, u32 color) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
        for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            const float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0) c.Blend(x, y, color, u8(cov * 255));
        }
}

float SmoothMin(float a, float b, float k) {
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1 - h);
}

// Signed distance to the gamepad silhouette (negative inside), in pixels.
float PadSdf(float px, float py, float cx, float cy, float s) {
    const float body = SegDist(px, py, cx - 0.25f * s, cy - 0.07f * s, cx + 0.25f * s, cy - 0.07f * s) - 0.19f * s;
    const float gl = SegDist(px, py, cx - 0.24f * s, cy, cx - 0.33f * s, cy + 0.22f * s) - 0.12f * s;
    const float gr = SegDist(px, py, cx + 0.24f * s, cy, cx + 0.33f * s, cy + 0.22f * s) - 0.12f * s;
    return SmoothMin(SmoothMin(body, gl, 0.07f * s), gr, 0.07f * s);
}

// ---- icons ------------------------------------------------------------------------------

void IconHome(Canvas& c, float cx, float cy, float s, u32 col) {
    const float w = s * 0.11f;
    Stroke(c, {{cx - s * 0.40f, cy - s * 0.02f}, {cx, cy - s * 0.38f}, {cx + s * 0.40f, cy - s * 0.02f}}, w, col);
    Stroke(c, {{cx - s * 0.28f, cy - s * 0.12f}, {cx - s * 0.28f, cy + s * 0.34f}, {cx - s * 0.07f, cy + s * 0.34f},
               {cx - s * 0.07f, cy + s * 0.12f}, {cx + s * 0.07f, cy + s * 0.12f}, {cx + s * 0.07f, cy + s * 0.34f},
               {cx + s * 0.28f, cy + s * 0.34f}, {cx + s * 0.28f, cy - s * 0.12f}}, w, col);
}

void IconCog(Canvas& c, float cx, float cy, float s, u32 col) {
    const float w = s * 0.11f;
    for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4;
        Stroke(c, {{cx + std::cos(a) * s * 0.27f, cy + std::sin(a) * s * 0.27f},
                   {cx + std::cos(a) * s * 0.41f, cy + std::sin(a) * s * 0.41f}}, w * 1.15f, col);
    }
    std::vector<Pt> ring, inner;
    for (int i = 0; i < 40; ++i) {
        const float a = i * 2 * 3.14159265f / 40;
        ring.push_back({cx + std::cos(a) * s * 0.27f, cy + std::sin(a) * s * 0.27f});
        inner.push_back({cx + std::cos(a) * s * 0.09f, cy + std::sin(a) * s * 0.09f});
    }
    Stroke(c, ring, w, col, true);
    Stroke(c, inner, w * 0.8f, col, true);
}

void IconDownload(Canvas& c, float cx, float cy, float s, u32 col) {
    const float w = s * 0.11f;
    Stroke(c, {{cx, cy - s * 0.36f}, {cx, cy + s * 0.12f}}, w, col);
    Stroke(c, {{cx - s * 0.18f, cy - s * 0.06f}, {cx, cy + s * 0.12f}, {cx + s * 0.18f, cy - s * 0.06f}}, w, col);
    Stroke(c, {{cx - s * 0.36f, cy + s * 0.12f}, {cx - s * 0.36f, cy + s * 0.34f}, {cx + s * 0.36f, cy + s * 0.34f},
               {cx + s * 0.36f, cy + s * 0.12f}}, w, col);
}

void IconFolder(Canvas& c, float cx, float cy, float s, u32 col) {
    const float w = s * 0.11f;
    Stroke(c, {{cx - s * 0.38f, cy - s * 0.26f}, {cx - s * 0.12f, cy - s * 0.26f}, {cx - s * 0.04f, cy - s * 0.16f},
               {cx + s * 0.38f, cy - s * 0.16f}, {cx + s * 0.38f, cy + s * 0.30f}, {cx - s * 0.38f, cy + s * 0.30f}},
           w, col, true);
}

void DrawDockIcon(Canvas& c, const Fonts& f, const DockItem& item, float cx, float cy, float s, u32 col) {
    switch (item.icon) {
    case DockIcon::Home: IconHome(c, cx, cy, s, col); break;
    case DockIcon::Systems: DrawGamepad(c, cx, cy + s * 0.04f, s * 1.1f, col, s * 0.1f); break;
    case DockIcon::Install: IconDownload(c, cx, cy, s, col); break;
    case DockIcon::Settings: IconCog(c, cx, cy, s, col); break;
    case DockIcon::Folder: IconFolder(c, cx, cy, s, col); break;
    case DockIcon::Text: {
        const char* t = item.text ? item.text : "?";
        const int size = int(s * 0.46f);
        const int w = f.bold->Measure(t, size);
        f.bold->Draw(c, int(cx) - w / 2, int(cy + size * 0.36f), t, size, col);
        break;
    }
    }
}

// ---- pictures ------------------------------------------------------------------------------

struct Image {
    std::vector<u32> px;
    int w{}, h{};
};
std::unordered_map<std::string, Image> g_images;      // by system id
std::unordered_map<std::string, Image> g_game_images; // by game path
Image g_avatar;

void BlitImage(Canvas& c, const Image& img, int bx, int by, int bw, int bh, bool cover, int clip_r) {
    if (img.w <= 0 || img.h <= 0) return;
    const float sx = float(bw) / img.w, sy = float(bh) / img.h;
    const float s = cover ? std::max(sx, sy) : std::min(sx, sy);
    const int dw = std::max(1, int(img.w * s)), dh = std::max(1, int(img.h * s));
    const int dx = bx + (bw - dw) / 2, dy = by + (bh - dh) / 2;
    for (int oy = std::max(dy, by); oy < std::min(dy + dh, by + bh); ++oy) {
        const float fy = (oy - dy + 0.5f) / s - 0.5f;
        const int y0 = std::clamp(int(std::floor(fy)), 0, img.h - 1), y1 = std::min(y0 + 1, img.h - 1);
        const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int ox = std::max(dx, bx); ox < std::min(dx + dw, bx + bw); ++ox) {
            const float fx = (ox - dx + 0.5f) / s - 0.5f;
            const int x0 = std::clamp(int(std::floor(fx)), 0, img.w - 1), x1 = std::min(x0 + 1, img.w - 1);
            const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
            const u32 top = Canvas::Mix(img.px[y0 * img.w + x0], img.px[y0 * img.w + x1], tx);
            const u32 bot = Canvas::Mix(img.px[y1 * img.w + x0], img.px[y1 * img.w + x1], tx);
            const u32 col = Canvas::Mix(top, bot, ty);
            float cov = ((col >> 24) & 0xFF) / 255.0f;
            if (clip_r > 0)
                cov *= Canvas::RoundCoverage(ox + 0.5f, oy + 0.5f, float(bx), float(by), float(bw), float(bh), float(clip_r));
            if (cov > 0.0f) c.Blend(ox, oy, col | 0xFF000000u, u8(cov * 255.0f));
        }
    }
}

bool Opaque(const Image& img) {
    for (std::size_t i = 0; i < img.px.size(); i += 97)
        if (((img.px[i] >> 24) & 0xFF) != 0xFF) return false;
    return true;
}

// A picture fills the box; a logo with transparency sits inside it.
void DrawPicture(Canvas& c, const Image& img, int x, int y, int w, int h, int r) {
    if (Opaque(img)) {
        BlitImage(c, img, x, y, w, h, true, r);
    } else {
        const int pad = std::max(6, w / 9);
        BlitImage(c, img, x + pad, y + pad, w - 2 * pad, h - 2 * pad, false, 0);
    }
}

std::string Initials(std::string_view title) {
    std::string out;
    bool start = true;
    for (char ch : title) {
        if (ch == ' ' || ch == '-' || ch == ':' || ch == '.') { start = true; continue; }
        if (start && std::isalnum(static_cast<unsigned char>(ch))) {
            out += ch;
            if (out.size() == 2) break;
        }
        start = false;
    }
    if (out.empty() && !title.empty()) out = std::string(title.substr(0, 1));
    if (out.size() == 2) out[1] = char(std::tolower(static_cast<unsigned char>(out[1])));
    return out;
}

void Chip(Canvas& c, const Fonts& f, int right, int bottom, std::string_view text, u32 color) {
    const int w = f.bold->Measure(text, 11) + 12;
    const int x = right - w, y = bottom - 20;
    c.FillRoundAA(x, y, w, 20, 10, Alpha(color, 0xF0));
    f.bold->Draw(c, x + 6, CenterBaseline(y, 20, 11), text, 11, MakeColor(0xFF, 0xFF, 0xFF));
}

constexpr int kDockStep = 86;
int DockW(int n) { return n * kDockStep + 32; }
int DockX(const Canvas& c, int n) { return (c.Width() - DockW(n)) / 2; }
int DockY(const Canvas& c) { return c.Height() - kHintH + 8; }

} // namespace

// ---- pictures ----------------------------------------------------------------------------

void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h) {
    if (rgba.empty() || w <= 0 || h <= 0) { g_images.erase(id); return; }
    g_images[id] = Image{std::move(rgba), w, h};
}
bool HasSystemImage(const std::string& id) { return g_images.count(id) != 0; }
void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h) {
    if (rgba.empty() || w <= 0 || h <= 0) { g_game_images.erase(path); return; }
    g_game_images[path] = Image{std::move(rgba), w, h};
}
bool HasGameImage(const std::string& path) { return g_game_images.count(path) != 0; }
void SetAvatar(std::vector<u32> rgba, int w, int h) { g_avatar = Image{std::move(rgba), w, h}; }

// ---- layout --------------------------------------------------------------------------------

Grid ComputeGrid(int screen_w, int screen_h) {
    Grid g;
    const int avail = screen_w - 80;
    g.cols = std::max(1, (avail + kTileGap) / (kTileW + kTileGap));
    const int used = g.cols * kTileW + (g.cols - 1) * kTileGap;
    g.start_x = (screen_w - used) / 2;
    // Between the profile bar and the dock's label bubble, centred.
    const int avail_h = screen_h - kHintH - 44 - kContentTop;
    g.visible_rows = std::max(1, (avail_h + kTileGap) / (kTileH + kTileGap));
    const int grid_h = g.visible_rows * (kTileH + kTileGap) - kTileGap;
    g.top = kContentTop + std::max(0, (avail_h - grid_h) / 2);
    return g;
}

void DrawBackdrop(Canvas& c) {
    static std::vector<u32> cache;
    static int cw = 0, ch = 0;
    const int w = c.Width(), h = c.Height();
    if (cw != w || ch != h || cache.empty()) {
        cache.assign(std::size_t(w) * h, 0);
        // Smooth black with a faint violet glow low on the right and a cool one top-left;
        // a touch of noise keeps the gradient from banding.
        u32 seed = 0x9E3779B9u;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float t = float(y) / h;
                float r = 7 - 2 * t, g = 7 - 2 * t, b = 10 - 3 * t;
                const float d1x = (x - w * 0.15f) / (w * 0.6f), d1y = (y + h * 0.15f) / (h * 0.8f);
                const float g1 = std::max(0.0f, 1.0f - (d1x * d1x + d1y * d1y));
                const float d2x = (x - w * 0.72f) / (w * 0.6f), d2y = (y - h * 0.85f) / (h * 0.7f);
                const float g2 = std::max(0.0f, 1.0f - (d2x * d2x + d2y * d2y));
                r += 6 * g1 * g1 + 26 * g2 * g2;
                g += 10 * g1 * g1 + 12 * g2 * g2;
                b += 16 * g1 * g1 + 34 * g2 * g2;
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                const float n = ((seed & 0xFF) / 255.0f - 0.5f) * 1.2f;
                cache[std::size_t(y) * w + x] = MakeColor(u8(std::clamp(r + n, 0.0f, 255.0f)),
                                                          u8(std::clamp(g + n, 0.0f, 255.0f)),
                                                          u8(std::clamp(b + n, 0.0f, 255.0f)));
            }
        }
        cw = w;
        ch = h;
    }
    c.CopyFrom(cache);
}

// ---- profile bar ------------------------------------------------------------------------------

void DrawTopBar(Canvas& c, const Fonts& f, const TopBar& bar) {
    const int cx = 70, cy = 56, r = 32;
    Disc(c, cx, cy + 3, r + 3, Alpha(MakeColor(0, 0, 0), 0x70));
    Disc(c, cx, cy, r + 3, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x40));
    if (g_avatar.w > 0) {
        BlitImage(c, g_avatar, cx - r, cy - r, 2 * r, 2 * r, true, r);
    } else {
        Disc(c, cx, cy, r, MakeColor(0x5A, 0x52, 0x7A));
        const std::string first = bar.title.empty() ? "?" : std::string(bar.title.substr(0, 1));
        const int w = f.bold->Measure(first, 28);
        f.bold->Draw(c, cx - w / 2, cy + 10, first, 28, kColText);
    }

    const int mid = c.Width() / 2;
    const std::string title = f.bold->Truncate(bar.title, 24, 560);
    const int tw = f.bold->Measure(title, 24);
    f.bold->Draw(c, mid - tw / 2, bar.subtitle.empty() ? 64 : 54, title, 24, kColText);
    if (!bar.subtitle.empty()) {
        const std::string sub = f.regular->Truncate(bar.subtitle, 15, 560);
        const int sw = f.regular->Measure(sub, 15);
        f.regular->Draw(c, mid - sw / 2, 78, sub, 15, kColTextDim);
    }

    // Battery on the right, the clock before it.
    int right = c.Width() - 44;
    if (bar.battery >= 0) {
        if (bar.charging) {
            Stroke(c, {{float(right - 2), 46.0f}, {float(right - 10), 58.0f}, {float(right - 2), 58.0f},
                       {float(right - 10), 70.0f}}, 3.0f, kColText);
            right -= 20;
        }
        const int bw = 40, bh = 22, bx = right - bw - 6, by = 47;
        c.RingRoundAA(bx, by, bw, bh, 6, 2.2f, kColText);
        c.FillRoundAA(bx + bw + 1, by + 7, 4, 8, 2, kColText);
        const u32 fill = bar.battery <= 15 && !bar.charging ? kColError : kColText;
        c.FillRoundAA(bx + 4, by + 4, std::max(2, (bw - 8) * std::clamp(bar.battery, 0, 100) / 100), bh - 8, 3, fill);
        right = bx - 18;
    }
    if (!bar.ampm.empty()) {
        const int aw = f.bold->Measure(bar.ampm, 14);
        f.bold->Draw(c, right - aw, 70, bar.ampm, 14, kColText);
        right -= aw + 4;
    }
    const int kw = f.bold->Measure(bar.clock, 30);
    f.bold->Draw(c, right - kw, 70, bar.clock, 30, kColText);
}

// ---- dock ---------------------------------------------------------------------------------------

int DockHitTest(const Canvas& c, int n, int x, int y) {
    const int dx = DockX(c, n), dy = DockY(c);
    if (y < dy || y > dy + kDockH || x < dx + 16 || x >= dx + 16 + n * kDockStep) return -1;
    return (x - dx - 16) / kDockStep;
}

void DrawDock(Canvas& c, const Fonts& f, const std::vector<DockItem>& items, int active, int cursor, bool focused) {
    const int n = int(items.size());
    const int x = DockX(c, n), y = DockY(c), w = DockW(n);
    c.SoftShadow(x, y, w, kDockH, kDockH / 2, 12, 8, 0xC0);
    c.FillRoundGradient(x, y, w, kDockH, kDockH / 2, MakeColor(0x24, 0x22, 0x2E), MakeColor(0x19, 0x18, 0x21));
    c.RingRoundAA(x, y, w, kDockH, kDockH / 2, 1.2f, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x1C));
    // ZL / ZR keycaps on the corners.
    for (int side = 0; side < 2; ++side) {
        const char* key = side ? "ZR" : "ZL";
        const int kx = side ? x + w - 26 : x - 18, ky = y - 12;
        c.FillRoundAA(kx, ky, 44, 20, 6, MakeColor(0xEC, 0xEA, 0xF4));
        const int kw = f.bold->Measure(key, 12);
        f.bold->Draw(c, kx + (44 - kw) / 2, CenterBaseline(ky, 20, 12), key, 12, MakeColor(0x2A, 0x27, 0x36));
    }
    const int shown = std::clamp(focused ? cursor : active, 0, n - 1);
    for (int i = 0; i < n; ++i) {
        const float cx = x + 16 + i * kDockStep + kDockStep / 2.0f, cy = y + kDockH / 2.0f;
        const bool on = i == active;
        if (focused && i == cursor) {
            c.FillRoundAA(int(cx - 30), int(cy - 26), 60, 52, 18, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x18));
        }
        const u32 col = on ? kColAccent : MakeColor(0xDA, 0xD6, 0xEA);
        DrawDockIcon(c, f, items[i], cx, cy, on ? 42.0f : 38.0f, col);
    }
    // Label bubble over the shown item.
    const char* label = items[shown].label;
    const int lw = f.bold->Measure(label, 16) + 28;
    const float lx = x + 16 + shown * kDockStep + kDockStep / 2.0f;
    const int bx = int(lx) - lw / 2, by = y - 44;
    c.SoftShadow(bx, by, lw, 30, 15, 6, 4, 0x90);
    c.FillRoundAA(bx, by, lw, 30, 15, MakeColor(0x2C, 0x29, 0x38));
    f.bold->Draw(c, bx + 14, CenterBaseline(by, 30, 16), label, 16, kColText);
}

// ---- tiles ----------------------------------------------------------------------------------------

void DrawEmptySlot(Canvas& c, int x, int y) {
    c.SoftShadow(x, y, kTileW, kTileH, 22, 6, 4, 0x70);
    c.FillRoundGradient(x, y, kTileW, kTileH, 22, MakeColor(0x1B, 0x1A, 0x22), MakeColor(0x14, 0x13, 0x19));
    c.RingRoundAA(x, y, kTileW, kTileH, 22, 1.0f, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x10));
    Disc(c, x + kTileW / 2.0f, y + kTileH / 2.0f, 3.2f, Alpha(MakeColor(0xE6, 0xE4, 0xF0), 0x90));
}

void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, bool selected, bool focused, float t_anim) {
    const bool lift = selected && focused;
    const float pulse = lift ? 0.5f + 0.5f * std::sin(t_anim * 3.0f) : 0.0f;
    const int g = lift ? 4 + int(pulse * 2) : 0;
    const int tx = x - g, ty = y - g, tw = kTileW + 2 * g, th = kTileH + 2 * g, r = 22 + g / 2;
    c.SoftShadow(tx, ty, tw, th, r, lift ? 14 : 6, lift ? 9 : 4, lift ? 0xD0 : 0x70);
    c.FillRoundGradient(tx, ty, tw, th, r, MakeColor(0x2A, 0x28, 0x34), MakeColor(0x1D, 0x1C, 0x25));
    auto custom = t.art_key.empty() ? g_game_images.end() : g_game_images.find(std::string{t.art_key});
    if (custom != g_game_images.end()) {
        DrawPicture(c, custom->second, tx, ty, tw, th, r);
    } else if (t.icon && !t.icon->empty()) {
        c.BlitIconRounded(*t.icon, t.icon_size, tx, ty, tw, r);
    } else {
        // Initials, softly lit from the top.
        c.FillRoundAAWith(tx, ty, tw, th / 2, r, [](float k) { return Alpha(MakeColor(0xFF, 0xFF, 0xFF), u8(14 * (1 - k))); });
        const std::string ini = Initials(t.title);
        const int size = 40;
        const int iw = f.mark->Measure(ini, size);
        f.mark->Draw(c, tx + (tw - iw) / 2, ty + th / 2 + 14, ini, size, kColText);
    }
    c.RingRoundAA(tx, ty, tw, th, r, 1.2f, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x18));
    if (!t.system_badge.empty()) Chip(c, f, tx + tw - 8, ty + th - 8, t.system_badge, t.system_color);
    int chip_x = tx + 8;
    for (std::string_view tag : t.tags) {
        const int cw = f.bold->Measure(tag, 11) + 12;
        c.FillRoundAA(chip_x, ty + 8, cw, 18, 9, Alpha(MakeColor(0, 0, 0), 0xB8));
        f.bold->Draw(c, chip_x + 6, CenterBaseline(ty + 8, 18, 11), tag, 11, kColText);
        chip_x += cw + 4;
    }
    if (lift) {
        const float phase = t_anim * 0.12f;
        c.RingRoundAAWith(tx - 5, ty - 5, tw + 10, th + 10, r + 5, 4.0f, [phase](float p) { return RingAt(p, phase); });
    } else if (selected) {
        c.RingRoundAA(tx - 3, ty - 3, tw + 6, th + 6, r + 3, 2.0f, Alpha(kColAccent, 0x80));
    }
}

void DrawTitlePill(Canvas& c, const Fonts& f, int y, std::string_view title, std::string_view system, u32 color) {
    const std::string t = f.bold->Truncate(title, 20, 640);
    const int tw = f.bold->Measure(t, 20), sw = f.regular->Measure(system, 15);
    const int w = tw + sw + 70, h = 40;
    const int x = (c.Width() - w) / 2;
    c.FillRoundAA(x, y, w, h, h / 2, Alpha(MakeColor(0x1C, 0x1B, 0x25), 0xE8));
    c.RingRoundAA(x, y, w, h, h / 2, 1.0f, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x14));
    Disc(c, x + 22.0f, y + h / 2.0f, 5.5f, color);
    f.bold->Draw(c, x + 36, CenterBaseline(y, h, 20), t, 20, kColText);
    f.regular->Draw(c, x + w - 18 - sw, CenterBaseline(y, h, 15), system, 15, kColTextDim);
}

// ---- systems carousel ------------------------------------------------------------------------------

void DrawGamepad(Canvas& c, float cx, float cy, float s, u32 col, float stroke) {
    const int x0 = int(cx - s * 0.5f) - 2, x1 = int(cx + s * 0.5f) + 2;
    const int y0 = int(cy - s * 0.3f) - 2, y1 = int(cy + s * 0.4f) + 2;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float d = PadSdf(x + 0.5f, y + 0.5f, cx, cy, s);
            const float cov = std::clamp(stroke / 2 - std::fabs(d + stroke / 2) + 0.5f, 0.0f, 1.0f);
            if (cov > 0) c.Blend(x, y, col, u8(cov * 255));
        }
    const float w = stroke * 0.9f;
    Stroke(c, {{cx - s * 0.33f, cy - s * 0.07f}, {cx - s * 0.17f, cy - s * 0.07f}}, w, col);
    Stroke(c, {{cx - s * 0.25f, cy - s * 0.15f}, {cx - s * 0.25f, cy + s * 0.01f}}, w, col);
    Disc(c, cx + s * 0.19f, cy - s * 0.12f, stroke * 0.62f, col);
    Disc(c, cx + s * 0.29f, cy - s * 0.03f, stroke * 0.62f, col);
}

void DrawSystemsCarousel(Canvas& c, const Fonts& f, const std::vector<SystemCard>& cards, float anim, int selected,
                         std::string_view name, std::string_view detail, std::string_view status, bool status_ok) {
    if (cards.empty()) return;
    const SystemCard& sel = cards[std::clamp(selected, 0, int(cards.size()) - 1)];
    const float mid_y = kContentTop + (c.Height() - kHintH - 44 - kContentTop) / 2.0f;
    // Big tinted controller on the left.
    for (int i = 5; i >= 1; --i) Disc(c, 210, mid_y, 70.0f + i * 18, Alpha(sel.color, u8(6)));
    DrawGamepad(c, 210, mid_y, 210, sel.color, 15);

    // The focused card and one neighbour each side, sized to fit between the bars.
    const float big = 204, small = 96, cx = 510;
    for (int i = 0; i < int(cards.size()); ++i) {
        const float off = i - anim;
        if (std::fabs(off) > 1.6f) continue;
        const float k = std::max(0.0f, 1.0f - std::fabs(off));
        const float size = small + (big - small) * k;
        const float step1 = big / 2 + 18 + small / 2;
        const float y = mid_y + step1 * std::clamp(off, -1.0f, 1.0f) + (std::fabs(off) > 1 ? (off > 0 ? 1 : -1) * (std::fabs(off) - 1) * (small + 18) : 0.0f);
        const int x = int(cx - size / 2), yy = int(y - size / 2), sz = int(size), r = int(size * 0.12f);
        if (yy < kContentTop - 4 || yy + sz > c.Height() - kHintH - 40) continue;
        c.SoftShadow(x, yy, sz, sz, r, 10, 6, u8(0x80 + 0x40 * k));
        const SystemCard& card = cards[i];
        c.FillRoundGradient(x, yy, sz, sz, r, Canvas::Mix(card.color, MakeColor(0xFF, 0xF4, 0xEC), 0.15f),
                            Canvas::Mix(card.color, MakeColor(0x10, 0x0C, 0x1C), 0.6f));
        auto img = g_images.find(std::string{card.id});
        if (img != g_images.end()) {
            DrawPicture(c, img->second, x, yy, sz, sz, r);
        } else {
            // Faint diagonal lines and the system's short name.
            const int step = std::max(8, sz / 11);
            for (int d = -sz; d < sz; d += step)
                for (int t = 0; t < sz; ++t) {
                    const int px = x + d + t, py = yy + sz - t;
                    if (px >= x + 3 && px < x + sz - 3 && py > yy + 3 && py < yy + sz - 3)
                        c.Blend(px, py, MakeColor(0xFF, 0xFF, 0xFF), 16);
                }
            const int ts = std::max(12, int(sz * (card.badge.size() > 3 ? 0.2f : 0.27f)));
            const int bw = f.mark->Measure(card.badge, ts);
            f.mark->Draw(c, x + (sz - bw) / 2 + 2, yy + sz / 2 + int(ts * 0.36f) + 3, card.badge, ts,
                         Alpha(MakeColor(0, 0, 0), 0x50));
            f.mark->Draw(c, x + (sz - bw) / 2, yy + sz / 2 + int(ts * 0.36f), card.badge, ts,
                         Canvas::Mix(card.color, MakeColor(0xFF, 0xFF, 0xFF), 0.6f));
        }
        c.RingRoundAA(x, yy, sz, sz, r, std::max(2.0f, size * 0.016f), Canvas::Mix(card.color, MakeColor(0xFF, 0xFF, 0xFF), 0.35f));
        if (i == selected)
            c.RingRoundAAWith(x - 5, yy - 5, sz + 10, sz + 10, r + 5, 3.5f, [](float p) { return RingAt(p); });
    }
    // Name, game count and status beside the focused card.
    const int tx = int(cx + big / 2 + 48);
    f.bold->Draw(c, tx, int(mid_y - 16), f.bold->Truncate(name, 40, c.Width() - tx - 40), 40, kColText);
    f.regular->Draw(c, tx, int(mid_y + 22), detail, 19, kColTextDim);
    Disc(c, tx + 6.0f, mid_y + 52, 5.0f, status_ok ? MakeColor(0x6E, 0xE7, 0xB7) : MakeColor(0xFF, 0xCE, 0x78));
    f.regular->Draw(c, tx + 20, int(mid_y + 58), status, 16, kColTextDim);
}

// ---- small pieces --------------------------------------------------------------------------------------

int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label) {
    constexpr int chip_h = 26;
    const int letter_w = f.bold->Measure(button, 14);
    const int chip_w = std::max(chip_h, letter_w + 14);
    c.FillRoundAA(x, y, chip_w, chip_h, chip_h / 2, MakeColor(0xEC, 0xEA, 0xF4));
    f.bold->Draw(c, x + (chip_w - letter_w) / 2, CenterBaseline(y, chip_h, 14), button, 14, MakeColor(0x1A, 0x18, 0x24));
    const int label_w = f.bold->Draw(c, x + chip_w + 8, CenterBaseline(y, chip_h, 17), label, 17, kColText);
    return chip_w + 8 + label_w;
}

void DrawHintBar(Canvas&) {}

void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h) {
    c.FillRoundAA(x, top, 4, track_h, 2, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x14));
    c.FillRoundAAWith(x, thumb_y, 4, thumb_h, 2, [](float t) { return RingAt(t); });
}

void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error) {
    const int tw = f.regular->Measure(text, 18);
    const int w = tw + 48, h = 44;
    const int x = (c.Width() - w) / 2;
    const int y = c.Height() - kHintH - h - 58;
    c.SoftShadow(x, y, w, h, h / 2, 10, 6, 0xB0);
    c.FillRoundAA(x, y, w, h, h / 2, error ? MakeColor(0x3A, 0x12, 0x18) : MakeColor(0x24, 0x22, 0x30));
    c.RingRoundAA(x, y, w, h, h / 2, 1.0f, error ? Alpha(kColError, 0x90) : Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x24));
    f.regular->Draw(c, x + 24, CenterBaseline(y, h, 18), text, 18, kColText);
}

void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir) {
    const int cx = c.Width() / 2;
    const int top = kContentTop + (c.Height() - kHintH - kContentTop) / 2 - 110;
    DrawGamepad(c, float(cx), top + 40.0f, 120, kColTextDim, 9);
    auto centred = [&](Font& font, std::string_view s, int y, int size, u32 col) {
        const int w = font.Measure(s, size);
        font.Draw(c, cx - w / 2, y, s, size, col);
    };
    centred(*f.bold, "No games yet", top + 126, 28, kColText);
    centred(*f.regular, "Put games in sdmc:/roms/<system>/  (3ds, ds, gba, ps2, n64, snes...)", top + 162, 17, kColTextDim);
    const std::string dir = f.regular->TruncateFront(roms_dir, 17, c.Width() - 120);
    centred(*f.regular, "3DS games can also go in " + dir, top + 188, 17, kColTextDim);
}

} // namespace SwitchFrontend::Skin
