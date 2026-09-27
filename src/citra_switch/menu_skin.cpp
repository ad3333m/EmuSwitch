// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/menu_skin.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace SwitchFrontend::Skin {
namespace {

using namespace Palette;
using namespace Layout;
using Gfx::CenterBaseline;

u32 Alpha(u32 color, u8 a) {
    return (color & 0x00FFFFFFu) | (u32{a} << 24);
}

// The teal -> blue -> violet of the focus ring and the rail pill.
u32 AccentAt(float t) {
    const u32 a = MakeColor(0x2F, 0xD6, 0xC9), b = MakeColor(0x3B, 0x82, 0xF6), c = MakeColor(0x8B, 0x5C, 0xF6);
    return t < 0.5f ? Canvas::Mix(a, b, t * 2) : Canvas::Mix(b, c, (t - 0.5f) * 2);
}

struct Mark {
    const char* id;
    const char* text;
    u32 top, bottom; // plate gradient
    u32 ink;         // wordmark colour
};

// Our own wordmarks in each system's classic colours (not the trademarked logo art).
const std::vector<Mark>& Marks() {
    static const std::vector<Mark> marks = {
        {"3ds", "3DS", MakeColor(0xE2, 0x1B, 0x33), MakeColor(0x8E, 0x0A, 0x1C), MakeColor(0xFF, 0xFF, 0xFF)},
        {"ds", "DS", MakeColor(0xEE, 0xF0, 0xF3), MakeColor(0xB4, 0xBA, 0xC3), MakeColor(0x26, 0x29, 0x31)},
        {"gba", "GBA", MakeColor(0x6A, 0x4D, 0xF0), MakeColor(0x35, 0x1F, 0x91), MakeColor(0xFF, 0xFF, 0xFF)},
        {"gb", "GAME BOY", MakeColor(0x9B, 0xBC, 0x0F), MakeColor(0x30, 0x62, 0x30), MakeColor(0x0F, 0x38, 0x0F)},
        {"nes", "NES", MakeColor(0xD5, 0xD8, 0xDD), MakeColor(0x8E, 0x94, 0x9C), MakeColor(0xC8, 0x10, 0x28)},
        {"snes", "SNES", MakeColor(0x7B, 0x68, 0xD8), MakeColor(0x41, 0x33, 0x8C), MakeColor(0xFF, 0xFF, 0xFF)},
        {"n64", "N64", MakeColor(0x1F, 0x8A, 0x3F), MakeColor(0x0B, 0x3D, 0x1A), MakeColor(0xFF, 0xD5, 0x3D)},
        {"vb", "VB", MakeColor(0x3A, 0x00, 0x05), MakeColor(0x0C, 0x00, 0x01), MakeColor(0xFF, 0x2A, 0x2A)},
        {"ps1", "PS", MakeColor(0x4A, 0x50, 0x5A), MakeColor(0x1B, 0x1E, 0x23), MakeColor(0xF1, 0xF2, 0xF4)},
        {"ps2", "PS2", MakeColor(0x13, 0x3F, 0xA8), MakeColor(0x04, 0x13, 0x3C), MakeColor(0xFF, 0xFF, 0xFF)},
        {"psp", "PSP", MakeColor(0x2A, 0x2D, 0x33), MakeColor(0x05, 0x06, 0x08), MakeColor(0xC9, 0xD1, 0xE0)},
        {"md", "GENESIS", MakeColor(0x1C, 0x1C, 0x22), MakeColor(0x02, 0x02, 0x04), MakeColor(0xE6, 0x2B, 0x2B)},
        {"sms", "SMS", MakeColor(0xC0, 0x12, 0x12), MakeColor(0x6A, 0x04, 0x04), MakeColor(0xFF, 0xFF, 0xFF)},
        {"gg", "GAME GEAR", MakeColor(0x30, 0x32, 0x3A), MakeColor(0x0E, 0x0F, 0x13), MakeColor(0x3B, 0x9B, 0xFF)},
        {"dc", "DC", MakeColor(0xF6, 0xF6, 0xF6), MakeColor(0xD4, 0xD6, 0xDA), MakeColor(0xF2, 0x65, 0x22)},
        {"arcade", "ARCADE", MakeColor(0xF8, 0xB0, 0x1A), MakeColor(0xB4, 0x53, 0x09), MakeColor(0x24, 0x12, 0x00)},
        {"pce", "PCE", MakeColor(0xF3, 0xF4, 0xF6), MakeColor(0xCF, 0xD2, 0xD7), MakeColor(0xE4, 0x00, 0x2B)},
        {"ngp", "NGP", MakeColor(0x13, 0xA9, 0xEB), MakeColor(0x03, 0x5A, 0x8C), MakeColor(0xFF, 0xFF, 0xFF)},
        {"ws", "WS", MakeColor(0x6B, 0x7A, 0x90), MakeColor(0x2E, 0x39, 0x4B), MakeColor(0xFF, 0xFF, 0xFF)},
        {"a2600", "2600", MakeColor(0x5A, 0x3A, 0x22), MakeColor(0x24, 0x15, 0x0A), MakeColor(0xF5, 0x9E, 0x0B)},
        {"lynx", "LYNX", MakeColor(0x24, 0x2B, 0x38), MakeColor(0x0A, 0x0D, 0x14), MakeColor(0xFA, 0xCC, 0x15)},
        {"wiiu", "Wii U", MakeColor(0xFF, 0xFF, 0xFF), MakeColor(0xE3, 0xE6, 0xEA), MakeColor(0x13, 0x9C, 0xDC)},
    };
    return marks;
}

const Mark* FindMark(std::string_view id) {
    for (const Mark& m : Marks())
        if (id == m.id) return &m;
    return nullptr;
}

struct Image {
    std::vector<u32> px; // RGBA, straight alpha
    int w{}, h{};
};
std::unordered_map<std::string, Image> g_images;      // by system id
std::unordered_map<std::string, Image> g_game_images; // by game path

// Scales an RGBA image into the box keeping its aspect: `cover` fills and crops,
// otherwise it fits inside. Pixels outside the rounded rect are clipped.
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

// Largest size at which `text` fits in w x h with the given font.
int FitSize(Font& font, std::string_view text, int w, int h) {
    const int probe = 100;
    const int tw = std::max(1, font.Measure(text, probe));
    return std::max(10, std::min(int(h * 0.62f), probe * w / tw));
}

} // namespace

void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h) {
    if (rgba.empty() || w <= 0 || h <= 0) {
        g_images.erase(id);
        return;
    }
    g_images[id] = Image{std::move(rgba), w, h};
}

bool HasSystemImage(const std::string& id) {
    return g_images.count(id) != 0;
}

void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h) {
    if (rgba.empty() || w <= 0 || h <= 0) {
        g_game_images.erase(path);
        return;
    }
    g_game_images[path] = Image{std::move(rgba), w, h};
}

bool HasGameImage(const std::string& path) {
    return g_game_images.count(path) != 0;
}

Grid ComputeGrid(int screen_w, int screen_h) {
    Grid g;
    const int content_w = screen_w - kRailW;
    const int avail = content_w - 64;
    g.cols = std::max(1, (avail + kTileGap) / (kTileW + kTileGap));
    const int used = g.cols * kTileW + (g.cols - 1) * kTileGap;
    g.start_x = kContentX + 32 + (avail - used) / 2;
    const int avail_h = screen_h - kHintH - kContentTop;
    g.visible_rows = std::max(1, (avail_h - 12 + kTileGap) / (kTileH + kTileGap));
    const int grid_h = g.visible_rows * (kTileH + kTileGap) - kTileGap;
    g.top = kContentTop + std::max(8, (avail_h - grid_h) / 2 - 6);
    return g;
}

void DrawBackdrop(Canvas& c) {
    static std::vector<u32> cache;
    static int cw = 0, ch = 0;
    const int w = c.Width(), h = c.Height();
    if (cw != w || ch != h || cache.empty()) {
        cache.assign(std::size_t(w) * h, 0);
        // Vertical near-black gradient, a faint teal glow top-left and violet bottom-right,
        // plus a touch of noise so the gradient never bands.
        u32 seed = 0x9E3779B9u;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float t = float(y) / h;
                float r = 9 - 3 * t, g = 10 - 3 * t, b = 14 - 4 * t;
                const float d1x = (x - w * 0.18f) / (w * 0.55f), d1y = (y + h * 0.1f) / (h * 0.75f);
                const float g1 = std::max(0.0f, 1.0f - (d1x * d1x + d1y * d1y));
                const float d2x = (x - w * 0.92f) / (w * 0.5f), d2y = (y - h * 1.05f) / (h * 0.7f);
                const float g2 = std::max(0.0f, 1.0f - (d2x * d2x + d2y * d2y));
                r += 4 * g1 * g1 + 10 * g2 * g2;
                g += 22 * g1 * g1 + 6 * g2 * g2;
                b += 24 * g1 * g1 + 22 * g2 * g2;
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

void DrawRail(Canvas& c, const Fonts& f, const std::vector<RailEntry>& items, int pill, int ghost) {
    // A glassy strip a shade above the backdrop, with a hairline edge.
    c.FillRect(0, 0, kRailW, c.Height(), Alpha(MakeColor(0x10, 0x12, 0x17), 0xE6));
    c.FillRect(kRailW - 1, 0, 1, c.Height(), kColLine);
    // Brand mark: a small gradient tile with a play triangle.
    const int bx = (kRailW - 44) / 2, by = 26;
    c.FillRoundAAWith(bx, by, 44, 44, 13, [](float t) { return AccentAt(t * 0.9f); });
    for (int row = 0; row < 18; ++row) {
        const int half = row < 9 ? row : 17 - row;
        c.FillRect(bx + 17, by + 13 + row, half + 1, 1, MakeColor(0xFF, 0xFF, 0xFF));
    }
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const int y = kRailFirstY + i * kRailItemStep;
        const bool on = i == pill;
        const int px = 14, pw = kRailW - 28;
        if (on) {
            c.FillRoundAAWith(px, y, pw, kRailItemH, 18, [](float t) {
                return Alpha(Canvas::Mix(MakeColor(0x1C, 0x3A, 0x3C), MakeColor(0x1A, 0x24, 0x40), t), 0xFF);
            });
            c.RingRoundAAWith(px, y, pw, kRailItemH, 18, 1.5f, [](float t) { return Alpha(AccentAt(t), 0x90); });
        } else if (i == ghost) {
            c.FillRoundAA(px, y, pw, kRailItemH, 18, kColSurface);
        }
        const u32 fg = on ? kColText : kColTextDim;
        const int cx = kRailW / 2, cy = y + 30;
        if (items[i].mask) {
            for (int row = 0; row < 32; ++row)
                for (int col = 0; col < 32; ++col)
                    c.Blend(cx - 16 + col, cy - 16 + row, on ? kColAccent : fg, items[i].mask[row * 32 + col]);
        } else {
            const int w = f.bold->Measure("AB", 18);
            f.bold->Draw(c, cx - w / 2, cy + 7, "AB", 18, on ? kColAccent : fg);
        }
        const int tw = f.regular->Measure(items[i].label, 15);
        f.regular->Draw(c, (kRailW - tw) / 2, y + 66, items[i].label, 15, fg);
    }
}

void DrawHeader(Canvas& c, const Fonts& f, std::string_view title, std::string_view subtitle) {
    const int x = kContentX + 32;
    f.bold->Draw(c, x, CenterBaseline(0, kHeaderH, 30) + 2, title, 30, kColText);
    if (!subtitle.empty()) {
        const int sw = f.regular->Measure(subtitle, 18);
        const int pw = sw + 28, ph = 32, py = (kHeaderH - ph) / 2 + 2;
        const int px = c.Width() - 32 - pw;
        c.FillRoundAA(px, py, pw, ph, ph / 2, kColSurface);
        c.RingRoundAA(px, py, pw, ph, ph / 2, 1.0f, kColLine);
        f.regular->Draw(c, px + 14, CenterBaseline(py, ph, 18), subtitle, 18, kColTextDim);
    }
}

void DrawSystemMark(Canvas& c, const Fonts& f, std::string_view id, int x, int y, int w, int h, int radius) {
    const Mark* m = FindMark(id);
    const u32 top = m ? m->top : kColSurfaceHi, bottom = m ? m->bottom : kColSurface;
    c.FillRoundGradient(x, y, w, h, radius, top, bottom);
    // Soft sheen across the top half.
    c.FillRoundAAWith(x, y, w, h / 2, radius, [](float t) { return Alpha(MakeColor(0xFF, 0xFF, 0xFF), u8(22 * (1 - t))); });
    auto it = g_images.find(std::string{id});
    if (it != g_images.end()) {
        const Image& img = it->second;
        bool opaque = true;
        for (std::size_t i = 0; i < img.px.size() && opaque; i += 97)
            opaque = ((img.px[i] >> 24) & 0xFF) == 0xFF;
        if (opaque) {
            BlitImage(c, img, x, y, w, h, true, radius); // a picture: fill the card
        } else {
            const int pad = std::max(6, w / 9);
            BlitImage(c, img, x + pad, y + pad, w - 2 * pad, h - 2 * pad, false, 0); // a logo: sit on the plate
        }
        return;
    }
    const std::string_view text = m ? std::string_view{m->text} : id;
    const u32 ink = m ? m->ink : kColText;
    Font& font = *f.mark;
    const int size = FitSize(font, text, int(w * 0.78f), h);
    const int tw = font.Measure(text, size);
    const int base = y + (h + int(size * 0.7f)) / 2;
    // A faint drop shadow keeps light marks legible on light plates.
    font.Draw(c, x + (w - tw) / 2 + 1, base + 2, text, size, Alpha(MakeColor(0, 0, 0), 0x40));
    font.Draw(c, x + (w - tw) / 2, base, text, size, ink);
}

void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, bool selected, bool focused) {
    const bool lift = selected && focused;
    const int g = lift ? 4 : 0;
    const int tx = x - g, ty = y - g, tw = kTileW + 2 * g, th = kTileH + 2 * g;
    if (lift) {
        c.SoftShadow(tx, ty, tw, th, 22, 14, 8, 0xC0);
        // Accent glow under the card.
        for (int i = 10; i >= 2; i -= 2)
            c.RingRoundAA(tx - i, ty - i, tw + 2 * i, th + 2 * i, 22 + i, 2.0f, Alpha(kColAccent, u8(4 + (10 - i) * 2)));
    } else {
        c.SoftShadow(tx, ty, tw, th, 22, 8, 5, 0x70);
    }
    c.FillRoundGradient(tx, ty, tw, th, 22, lift ? MakeColor(0x20, 0x23, 0x2B) : MakeColor(0x17, 0x19, 0x1F),
                        lift ? MakeColor(0x17, 0x19, 0x20) : MakeColor(0x11, 0x13, 0x18));
    c.RingRoundAA(tx, ty, tw, th, 22, 1.0f, Alpha(MakeColor(0xFF, 0xFF, 0xFF), lift ? 0x22 : 0x12));

    // Artwork: a custom picture, else the 3DS icon, else the system's mark on a plate.
    // Pictures and icons carry a small system mark in the corner.
    const int art = kIconSize + g;
    const int ax = tx + (tw - art) / 2, ay = ty + 16;
    auto custom = t.art_key.empty() ? g_game_images.end() : g_game_images.find(std::string{t.art_key});
    if (custom != g_game_images.end()) {
        c.SoftShadow(ax, ay, art, art, 20, 6, 4, 0x90);
        c.FillRoundAA(ax, ay, art, art, 20, kColSurfaceHi);
        BlitImage(c, custom->second, ax, ay, art, art, true, 20);
        DrawSystemMark(c, f, t.system_id, ax + art - 44, ay + art - 24, 50, 28, 8);
    } else if (t.icon && !t.icon->empty()) {
        c.SoftShadow(ax, ay, art, art, 20, 6, 4, 0x90);
        c.BlitIconRounded(*t.icon, t.icon_size, ax, ay, art, 20);
        DrawSystemMark(c, f, t.system_id, ax + art - 44, ay + art - 24, 50, 28, 8);
    } else {
        c.SoftShadow(ax, ay, art, art, 20, 6, 4, 0x90);
        DrawSystemMark(c, f, t.system_id, ax, ay, art, art, 20);
    }

    // Title and subtitle.
    const int text_w = tw - 28;
    const std::string title = f.bold->Truncate(t.title, 17, text_w);
    const int title_w = f.bold->Measure(title, 17);
    f.bold->Draw(c, tx + (tw - title_w) / 2, ay + art + 30, title, 17, kColText);
    if (!t.subtitle.empty()) {
        const std::string sub = f.regular->Truncate(t.subtitle, 14, text_w);
        const int sw = f.regular->Measure(sub, 14);
        f.regular->Draw(c, tx + (tw - sw) / 2, ay + art + 52, sub, 14, kColTextDim);
    }
    // Tags (LOCKED / SD / CART) as tiny chips in the top-left corner.
    int chip_x = tx + 10;
    for (std::string_view tag : t.tags) {
        const int cw = f.bold->Measure(tag, 11) + 12;
        c.FillRoundAA(chip_x, ty + 10, cw, 18, 9, Alpha(MakeColor(0x00, 0x00, 0x00), 0xB0));
        f.bold->Draw(c, chip_x + 6, CenterBaseline(ty + 10, 18, 11), tag, 11, kColText);
        chip_x += cw + 4;
    }
    if (lift) {
        c.RingRoundAAWith(tx - 3, ty - 3, tw + 6, th + 6, 25, 3.0f, [](float p) { return AccentAt(p); });
    } else if (selected) {
        c.RingRoundAA(tx - 2, ty - 2, tw + 4, th + 4, 24, 2.0f, Alpha(kColAccent, 0x70));
    }
}

int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label) {
    constexpr int chip_h = 28;
    const int letter_w = f.bold->Measure(button, 15);
    const int chip_w = std::max(chip_h, letter_w + 16);
    c.FillRoundAA(x, y, chip_w, chip_h, chip_h / 2, MakeColor(0xE9, 0xEB, 0xEF));
    f.bold->Draw(c, x + (chip_w - letter_w) / 2, CenterBaseline(y, chip_h, 15), button, 15, MakeColor(0x0B, 0x0C, 0x10));
    const int label_x = x + chip_w + 10;
    const int label_w = f.regular->Draw(c, label_x, CenterBaseline(y, chip_h, 17), label, 17, kColTextDim);
    return chip_w + 10 + label_w;
}

void DrawHintBar(Canvas& c) {
    const int y = c.Height() - kHintH;
    c.FillRect(kRailW, y, c.Width() - kRailW, 1, kColLine);
}

void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h) {
    c.FillRoundAA(x, top, 4, track_h, 2, Alpha(MakeColor(0xFF, 0xFF, 0xFF), 0x14));
    c.FillRoundAAWith(x, thumb_y, 4, thumb_h, 2, [](float t) { return AccentAt(t); });
}

void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error) {
    const int tw = f.regular->Measure(text, 18);
    const int w = tw + 48, h = 44;
    const int x = kContentX + (c.Width() - kContentX - w) / 2;
    const int y = c.Height() - kHintH - h - 18;
    c.SoftShadow(x, y, w, h, h / 2, 10, 6, 0xB0);
    c.FillRoundAA(x, y, w, h, h / 2, error ? MakeColor(0x3A, 0x12, 0x16) : MakeColor(0x14, 0x2E, 0x2D));
    c.RingRoundAA(x, y, w, h, h / 2, 1.0f, error ? Alpha(kColError, 0x90) : Alpha(kColAccent, 0x90));
    f.regular->Draw(c, x + 24, CenterBaseline(y, h, 18), text, 18, kColText);
}

void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir) {
    const int cx = kContentX + (c.Width() - kContentX) / 2;
    const int top = (kContentTop + c.Height() - kHintH) / 2 - 110;
    c.FillRoundAAWith(cx - 44, top, 88, 88, 26, [](float t) { return Alpha(AccentAt(t), 0x40); });
    c.RingRoundAAWith(cx - 44, top, 88, 88, 26, 1.5f, [](float t) { return Alpha(AccentAt(t), 0xA0); });
    const int qw = f.bold->Measure("+", 48);
    f.bold->Draw(c, cx - qw / 2, top + 62, "+", 48, kColText);
    auto centred = [&](Font& font, std::string_view s, int y, int size, u32 col) {
        const int w = font.Measure(s, size);
        font.Draw(c, cx - w / 2, y, s, size, col);
    };
    centred(*f.bold, "No games yet", top + 136, 28, kColText);
    centred(*f.regular, "Put games in sdmc:/roms/<system>/  (3ds, ds, gba, ps2, n64, snes...)", top + 172, 17, kColTextDim);
    const std::string dir = f.regular->TruncateFront(roms_dir, 17, c.Width() - kContentX - 80);
    centred(*f.regular, "3DS games can also go in " + dir, top + 198, 17, kColTextDim);
}

} // namespace SwitchFrontend::Skin
