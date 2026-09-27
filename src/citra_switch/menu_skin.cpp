// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/menu_skin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "citra_switch/menu_skin_art.h"

namespace SwitchFrontend::Skin {
namespace {

using namespace Palette;
using namespace Layout;
using Gfx::CenterBaseline;
using Gfx::WithAlpha;
using Shapes::DockIconShape;
using u64 = std::uint64_t;

constexpr u32 kWhite = MakeColor(0xFF, 0xFF, 0xFF);
constexpr u32 kBlack = MakeColor(0, 0, 0);

u32 White(u8 a) {
    return WithAlpha(kWhite, a);
}
u32 Black(u8 a) {
    return WithAlpha(kBlack, a);
}
u8 A(float a) {
    return static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Teal -> violet -> pink, the focus ring's colours; `phase` slowly turns it.
u32 RingAt(float t, float phase = 0.0f) {
    t = t + phase;
    t -= std::floor(t);
    const u32 a = MakeColor(0x5E, 0xE7, 0xDF), b = MakeColor(0x8B, 0x7C, 0xFF), c = MakeColor(0xF0, 0x7C, 0xD8);
    if (t < 0.4f) return Canvas::Mix(a, b, t / 0.4f);
    if (t < 0.8f) return Canvas::Mix(b, c, (t - 0.4f) / 0.4f);
    return Canvas::Mix(c, a, (t - 0.8f) / 0.2f);
}

// ---- frame state ------------------------------------------------------------------------------

double g_now = 0.0;
double g_last_now = -1.0;
std::atomic<bool> g_may_build{true};
std::atomic<int> g_budget{1 << 20};
std::atomic<std::uint64_t> g_frame{0};
int g_screen_w = Gfx::kPanelW;
int g_screen_h = Gfx::kPanelH;

bool MayBuild() {
    return g_may_build.load(std::memory_order_relaxed);
}

// Takes one unit of this frame's building budget, so a page of new pictures is spread over a
// few frames instead of stalling one.
bool TakeBudget() {
    if (!MayBuild()) {
        return false;
    }
    return g_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

// ---- backdrop: a slow field of coloured light, computed at 1/8 size and filtered up ----

float g_amb[3] = {60, 90, 160};
u32 g_amb_target = MakeColor(60, 90, 160);

struct Spot {
    float x = 0, y = 0, radius = 1;
    float col[3] = {0, 0, 0};
    u32 target_col = 0;
    float strength = 0, target = 0;
} g_spot;

struct Field {
    int w = 0, h = 0, lw = 0, lh = 0;
    std::vector<int> v; // 3 ints per sample, value * 256
} g_field;

const std::array<u8, 64 * 64>& Noise() {
    static const std::array<u8, 64 * 64> table = [] {
        std::array<u8, 64 * 64> t{};
        std::uint32_t s = 0x9E3779B9u;
        for (u8& v : t) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            v = static_cast<u8>(s >> 24);
        }
        return t;
    }();
    return table;
}

void BuildField(double t) {
    Field& F = g_field;
    F.w = g_screen_w;
    F.h = g_screen_h;
    F.lw = F.w / 8 + 2;
    F.lh = F.h / 8 + 2;
    F.v.resize(static_cast<std::size_t>(F.lw) * F.lh * 3);
    const float ft = static_cast<float>(t);
    struct Blob {
        float cx, cy, rx, ry, r, g, b, k;
    };
    const Blob blobs[] = {
        {0.16f + 0.05f * std::sin(ft * 0.23f), 0.06f + 0.06f * std::cos(ft * 0.19f), 0.78f, 0.95f, g_amb[0], g_amb[1],
         g_amb[2], 0.34f},
        {0.88f + 0.04f * std::cos(ft * 0.17f), 0.95f + 0.05f * std::sin(ft * 0.21f), 0.72f, 0.85f, 118, 64, 214, 0.22f},
        {0.56f + 0.12f * std::sin(ft * 0.11f), 0.52f + 0.08f * std::cos(ft * 0.13f), 0.5f, 0.6f, 24, 120, 150, 0.07f},
    };
    const Spot& sp = g_spot;
    const float spot_k = sp.strength * 0.55f;
    const float inv_r2 = 1.0f / std::max(1.0f, sp.radius * sp.radius);
    for (int j = 0; j < F.lh; ++j) {
        const float fy = std::min(1.0f, (j * 8.0f) / F.h);
        for (int i = 0; i < F.lw; ++i) {
            const float fx = std::min(1.0f, (i * 8.0f) / F.w);
            float r = 9 - 5 * fy, g = 10 - 5 * fy, b = 16 - 8 * fy;
            for (const Blob& bl : blobs) {
                const float dx = (fx - bl.cx) / bl.rx, dy = (fy - bl.cy) / bl.ry;
                float f = std::max(0.0f, 1.0f - (dx * dx + dy * dy));
                f = f * f * bl.k;
                r += bl.r * f;
                g += bl.g * f;
                b += bl.b * f;
            }
            if (spot_k > 0.001f) {
                // Squashed vertically: a pool on the floor rather than a ball.
                const float dx = i * 8.0f - sp.x, dy = (j * 8.0f - sp.y) * 1.7f;
                float f = std::max(0.0f, 1.0f - (dx * dx + dy * dy) * inv_r2);
                f = f * f * spot_k;
                r += sp.col[0] * f;
                g += sp.col[1] * f;
                b += sp.col[2] * f;
            }
            const float vx = fx - 0.5f, vy = fy - 0.45f;
            const float vig = std::max(0.0f, 1.0f - 0.55f * (vx * vx + vy * vy * 1.2f));
            int* o = &F.v[(static_cast<std::size_t>(j) * F.lw + i) * 3];
            o[0] = static_cast<int>(std::clamp(r * vig, 0.0f, 255.0f) * 256.0f);
            o[1] = static_cast<int>(std::clamp(g * vig, 0.0f, 255.0f) * 256.0f);
            o[2] = static_cast<int>(std::clamp(b * vig, 0.0f, 255.0f) * 256.0f);
        }
    }
}

// ---- pictures ----------------------------------------------------------------------------------

struct Picture {
    Image img;
    u32 accent = 0;
    std::uint64_t id = 0;
};

std::atomic<std::uint64_t> g_next_pic{1};
std::mutex g_pic_mutex;
std::unordered_map<std::string, std::shared_ptr<const Picture>> g_system_pics;
std::unordered_map<std::string, std::shared_ptr<const Picture>> g_game_pics;
// 3DS icons, by game path and a checksum of the icon.
std::unordered_map<std::uint64_t, std::shared_ptr<const Picture>> g_icon_pics;
// Default system cards, drawn once per system.
std::unordered_map<std::string, std::shared_ptr<const Picture>> g_card_pics;
// Logos put in for the consoles' Home sections.
std::unordered_map<std::string, std::shared_ptr<const Picture>> g_logo_pics;
Image g_avatar;
std::uint64_t g_avatar_id = 0;

std::shared_ptr<const Picture> MakePicture(Image img, u32 accent = 0) {
    auto p = std::make_shared<Picture>();
    p->accent = accent ? accent : Gfx::AverageColor(img, MakeColor(80, 110, 170));
    p->img = std::move(img);
    p->id = g_next_pic.fetch_add(1);
    return p;
}

std::shared_ptr<const Picture> Find(const std::unordered_map<std::string, std::shared_ptr<const Picture>>& map,
                                    std::string_view key) {
    std::lock_guard lock{g_pic_mutex};
    const auto it = map.find(std::string{key});
    return it == map.end() ? nullptr : it->second;
}

std::shared_ptr<const Picture> IconPicture(const TileInfo& t) {
    if (!t.icon || t.icon->empty() || t.icon_size <= 0 ||
        t.icon->size() < static_cast<std::size_t>(t.icon_size) * t.icon_size) {
        return nullptr;
    }
    std::uint64_t key = std::hash<std::string_view>{}(t.art_key);
    const std::vector<u32>& px = *t.icon;
    for (std::size_t i = 0; i < px.size(); i += 97) {
        key = key * 1099511628211ull + px[i];
    }
    {
        std::lock_guard lock{g_pic_mutex};
        if (auto it = g_icon_pics.find(key); it != g_icon_pics.end()) {
            return it->second;
        }
    }
    // Cheap (a copy and an average), and the same on every thread, so it may be built anywhere.
    Image img;
    img.w = img.h = t.icon_size;
    img.px = px;
    img.opaque = true;
    for (u32& p : img.px) {
        if ((p >> 24) != 0xFF) {
            img.opaque = false;
            break;
        }
    }
    auto pic = MakePicture(std::move(img));
    std::lock_guard lock{g_pic_mutex};
    if (g_icon_pics.size() > 1024) {
        g_icon_pics.clear();
    }
    return g_icon_pics.emplace(key, std::move(pic)).first->second;
}

// ---- the sprite cache: pictures at the exact size they're drawn, and baked glass ----

struct CacheEntry {
    std::shared_ptr<const Image> img;
    std::uint64_t last = 0;
    std::size_t bytes = 0;
};
std::mutex g_cache_mutex;
std::unordered_map<std::uint64_t, CacheEntry> g_cache;
std::size_t g_cache_bytes = 0;
constexpr std::size_t kCacheBudget = 32u << 20;

std::uint64_t Key(std::initializer_list<std::uint64_t> parts) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::uint64_t p : parts) {
        h ^= p + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0x100000001B3ull;
    }
    return h;
}

// The cached image for `key`, built with `build` if missing and building is allowed now.
// `budgeted` builds (the slow picture resizes) are spread over frames.
std::shared_ptr<const Image> Cached(std::uint64_t key, bool budgeted, const std::function<Image()>& build) {
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    {
        std::lock_guard lock{g_cache_mutex};
        if (auto it = g_cache.find(key); it != g_cache.end()) {
            it->second.last = frame;
            return it->second.img;
        }
    }
    if (!MayBuild() || (budgeted && !TakeBudget())) {
        return nullptr;
    }
    auto img = std::make_shared<const Image>(build());
    std::lock_guard lock{g_cache_mutex};
    CacheEntry& e = g_cache[key];
    if (e.img) {
        g_cache_bytes -= e.bytes;
    }
    e.img = img;
    e.last = frame;
    e.bytes = img->px.size() * sizeof(u32);
    g_cache_bytes += e.bytes;
    return img;
}

void TrimCache(bool all) {
    std::lock_guard lock{g_cache_mutex};
    if (all) {
        g_cache.clear();
        g_cache_bytes = 0;
        return;
    }
    if (g_cache_bytes <= kCacheBudget) {
        return;
    }
    // Oldest first, never what the last frame used.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
    order.reserve(g_cache.size());
    for (const auto& [k, e] : g_cache) {
        order.emplace_back(e.last, k);
    }
    std::sort(order.begin(), order.end());
    const std::uint64_t frame = g_frame.load();
    for (const auto& [last, k] : order) {
        if (g_cache_bytes <= kCacheBudget * 3 / 4 || last + 1 >= frame) {
            break;
        }
        auto it = g_cache.find(k);
        g_cache_bytes -= it->second.bytes;
        g_cache.erase(it);
    }
}

// Draws `fg` over `bg` at (ox, oy), straight alpha.
void Composite(Image& bg, const Image& fg, int ox, int oy) {
    for (int y = 0; y < fg.h; ++y) {
        const int by = y + oy;
        if (by < 0 || by >= bg.h) continue;
        for (int x = 0; x < fg.w; ++x) {
            const int bx = x + ox;
            if (bx < 0 || bx >= bg.w) continue;
            const u32 f = fg.px[static_cast<std::size_t>(y) * fg.w + x];
            u32& b = bg.px[static_cast<std::size_t>(by) * bg.w + bx];
            const u32 a = f >> 24;
            b = a == 255 ? f : Gfx::Lerp256(b, f, a + (a >> 7));
        }
    }
}

// Share of pixels that are (nearly) opaque.
float OpaqueShare(const Image& img) {
    if (img.opaque) return 1.0f;
    std::size_t n = 0, solid = 0;
    const std::size_t step = std::max<std::size_t>(1, img.px.size() / 4096);
    for (std::size_t i = 0; i < img.px.size(); i += step, ++n) {
        solid += (img.px[i] >> 24) >= 250 ? 1 : 0;
    }
    return n ? float(solid) / float(n) : 0.0f;
}

// A square version of `src`: photos fill it, pictures far from square (box art) sit on a
// blurred copy of themselves, and logos with transparency keep a margin.
Image BuildSquare(const Image& src, int s) {
    if (OpaqueShare(src) < 0.6f) {
        const int inner = std::max(1, s - 2 * std::max(4, s / 9));
        const float k = std::min(float(inner) / src.w, float(inner) / src.h);
        const int w = std::max(1, static_cast<int>(std::lround(src.w * k)));
        const int h = std::max(1, static_cast<int>(std::lround(src.h * k)));
        const Image fg = Gfx::Resize(src, w, h);
        Image out;
        out.w = out.h = s;
        out.opaque = false;
        out.px.assign(static_cast<std::size_t>(s) * s, 0);
        const int ox = (s - w) / 2, oy = (s - h) / 2;
        for (int y = 0; y < h; ++y) {
            std::copy_n(fg.px.data() + static_cast<std::size_t>(y) * w, w,
                        out.px.data() + static_cast<std::size_t>(y + oy) * s + ox);
        }
        return out;
    }
    const float aspect = float(src.w) / float(src.h);
    if (src.opaque && aspect > 0.8f && aspect < 1.25f) {
        return Gfx::ResizeCover(src, s, s);
    }
    // Blurred, darkened fill from the picture itself, then the whole picture on top.
    Image solid = src;
    solid.opaque = true;
    for (u32& p : solid.px) {
        p |= 0xFF000000u;
    }
    const int small = std::max(8, s / 6);
    Image bg = Gfx::ResizeCover(solid, small, small);
    Gfx::BoxBlur(bg, 2);
    bg = Gfx::Resize(bg, s, s);
    for (u32& p : bg.px) {
        p = Canvas::Mix(p, kBlack, 0.42f) | 0xFF000000u;
    }
    const float k = std::min(float(s) / src.w, float(s) / src.h);
    const int w = std::max(1, static_cast<int>(std::lround(src.w * k)));
    const int h = std::max(1, static_cast<int>(std::lround(src.h * k)));
    Composite(bg, Gfx::Resize(src, w, h), (s - w) / 2, (s - h) / 2);
    bg.opaque = true;
    return bg;
}


std::string Initials(std::string_view title) {
    std::string out;
    bool start = true;
    for (char ch : title) {
        if (ch == ' ' || ch == '-' || ch == ':' || ch == '.') {
            start = true;
            continue;
        }
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

// ---- glass ----------------------------------------------------------------------------------------

// Frosted glass: a translucent body over whatever is below, lit along its top edge.
void GlassDirect(Canvas& c, int x, int y, int w, int h, int r, bool raised, float strength) {
    if (raised) {
        c.Glow(x, y, w, h, r, 30, Black(A(0.55f * strength)), true, 10);
    }
    const u8 top = A(0.62f * strength + 0.18f), bot = A(0.70f * strength + 0.18f);
    const float rows = float(std::max(1, h - 1));
    // One pass: the body's gradient, a sheen over its upper half and a bright hairline along
    // the top edge, all following the rounded outline.
    c.FillRoundAAWith(x, y, w, h, r, [top, bot, rows](float t) {
        const u32 base = Canvas::Mix(MakeColor(0x2C, 0x2C, 0x3A, top), MakeColor(0x18, 0x18, 0x22, bot), t);
        const float row = t * rows;
        float lift = t < 0.5f ? 0.08f * (1 - 2 * t) * (1 - 2 * t) : 0.0f;
        lift += row < 1.0f ? 0.30f : row < 2.0f ? 0.10f : 0.0f;
        return Canvas::Mix(base, kWhite, std::min(1.0f, lift));
    });
    c.RingRoundAA(x, y, w, h, r, 1.0f, White(0x22));
}

// The same, baked once per size into a sprite.
void Glass(Canvas& c, int x, int y, int w, int h, int r, bool raised, float strength = 1.0f) {
    const int m = raised ? 30 : 1, drop = raised ? 10 : 0;
    const auto sprite = Cached(Key({2, u64(w), u64(h), u64(r), u64(raised), u64(strength * 1000)}), false, [&] {
        return Gfx::RenderSprite(w + 2 * m, h + 2 * m + drop, m, m, [&](Canvas& t) { GlassDirect(t, 0, 0, w, h, r, raised, strength); });
    });
    if (sprite) {
        c.DrawSprite(*sprite, x - m, y - m);
    } else {
        GlassDirect(c, x, y, w, h, r, raised, strength);
    }
}

void DrawIcon(Canvas& c, DockIconShape shape, float cx, float cy, int size, u32 color) {
    const auto mask = Shapes::IconMask(shape, size, MayBuild());
    if (mask) {
        c.DrawMask(*mask, static_cast<int>(std::lround(cx - size / 2.0f)),
                   static_cast<int>(std::lround(cy - size / 2.0f)), color);
    }
}

void Chip(Canvas& c, const Fonts& f, int right, int bottom, std::string_view text, u32 color) {
    const int w = f.bold->Measure(text, 11) + 14;
    const int x = right - w, y = bottom - 20;
    c.FillRoundAA(x, y, w, 20, 10, WithAlpha(Canvas::Mix(color, kBlack, 0.12f), 0xEE));
    c.RingRoundAA(x, y, w, 20, 10, 1.0f, White(0x30));
    f.bold->Draw(c, x + 7, CenterBaseline(y, 20, 11), text, 11, kWhite);
}

constexpr int kDockStep = 86;
int DockW(int n) { return n * kDockStep + 32; }
int DockX(int n) { return (g_screen_w - DockW(n)) / 2; }
int DockY() { return g_screen_h - kHintH + 8; }

// ---- carousel geometry ----

constexpr float kCardBig = 204.0f, kCardSmall = 96.0f, kCardX = 540.0f;
float CarouselMidY() {
    return kContentTop + (g_screen_h - kHintH - 44 - kContentTop) / 2.0f;
}
struct CardRect {
    int x, y, s;
    float k; // 1 focused .. 0 neighbour
};
CardRect CardAt(int i, float anim) {
    const float off = i - anim;
    const float k = std::max(0.0f, 1.0f - std::fabs(off));
    const float size = kCardSmall + (kCardBig - kCardSmall) * k;
    const float step1 = kCardBig / 2 + 18 + kCardSmall / 2;
    const float y = CarouselMidY() + step1 * std::clamp(off, -1.0f, 1.0f) +
                    (std::fabs(off) > 1 ? (off > 0 ? 1 : -1) * (std::fabs(off) - 1) * (kCardSmall + 18) : 0.0f);
    return {static_cast<int>(kCardX - size / 2), static_cast<int>(y - size / 2), static_cast<int>(size), k};
}
int CarouselTextX() {
    return static_cast<int>(kCardX + kCardBig / 2 + 52);
}
struct Rect {
    int x, y, w, h;
};
Rect PictureButton() {
    return {CarouselTextX(), static_cast<int>(CarouselMidY()) + 80, 238, 40};
}

// The default look of a system card, drawn once into a picture.
std::shared_ptr<const Picture> DefaultCard(const Fonts& f, const SystemCard& card) {
    const std::string key = std::string{card.id} + "#" + std::to_string(card.color);
    if (auto p = Find(g_card_pics, key)) {
        return p;
    }
    if (!TakeBudget()) {
        return nullptr;
    }
    constexpr int s = static_cast<int>(kCardBig);
    Canvas tmp;
    tmp.Resize(s, s);
    const u32 col = card.color;
    tmp.FillRoundAAWith(0, 0, s, s, 0, [col](float t) {
        return Canvas::Mix(Canvas::Mix(col, kWhite, 0.10f), Canvas::Mix(col, MakeColor(0x0C, 0x0A, 0x16), 0.62f), t);
    });
    // Soft light from the top left and a large faint ring.
    tmp.Disc(s * 0.18f, s * 0.12f, s * 0.62f, White(0x16));
    tmp.Circle(s * 0.82f, s * 0.92f, s * 0.46f, 10.0f, White(0x0E));
    tmp.Circle(s * 0.82f, s * 0.92f, s * 0.30f, 4.0f, White(0x0A));
    const int ts = card.badge.size() > 3 ? 42 : 56;
    const int bw = f.mark->Measure(card.badge, ts);
    const int bx = (s - bw) / 2, by = s / 2 + static_cast<int>(ts * 0.36f);
    f.mark->Draw(tmp, bx + 2, by + 4, card.badge, ts, Black(0x60));
    f.mark->Draw(tmp, bx, by, card.badge, ts, Canvas::Mix(col, kWhite, 0.82f));
    Image img;
    img.w = img.h = s;
    img.px.assign(tmp.Data(), tmp.Data() + static_cast<std::size_t>(s) * s);
    auto pic = MakePicture(std::move(img), col | 0xFF000000u);
    std::lock_guard lock{g_pic_mutex};
    return g_card_pics.emplace(key, std::move(pic)).first->second;
}

} // namespace

// ---- frames ----------------------------------------------------------------------------------------

void BeginFrame(double now, int screen_w, int screen_h) {
    const double dt = g_last_now < 0 ? 1.0 : std::clamp(now - g_last_now, 0.0, 1.0);
    g_last_now = now;
    g_now = now;
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    g_frame.fetch_add(1);
    const float k = 1.0f - static_cast<float>(std::exp(-dt * 2.4));
    const float target[3] = {float(g_amb_target & 0xFF), float((g_amb_target >> 8) & 0xFF),
                             float((g_amb_target >> 16) & 0xFF)};
    for (int i = 0; i < 3; ++i) {
        g_amb[i] += (target[i] - g_amb[i]) * k;
    }
    Spot& sp = g_spot;
    const float ks = 1.0f - static_cast<float>(std::exp(-dt * 5.0));
    sp.strength += (sp.target - sp.strength) * ks;
    for (int i = 0; i < 3; ++i) {
        sp.col[i] += (float((sp.target_col >> (i * 8)) & 0xFF) - sp.col[i]) * ks;
    }
    BuildField(now);
    TrimCache(false);
    g_budget.store(4);
    g_may_build.store(true);
}

void EndWarmup() {
    g_may_build.store(false);
}

void SetAmbient(u32 color) {
    g_amb_target = color;
}

void SetSpot(float x, float y, float radius, u32 color, float strength) {
    Spot& sp = g_spot;
    if (sp.strength < 0.01f && strength > 0.0f) {
        // Appearing: start in the new colour rather than sliding from the old one.
        for (int i = 0; i < 3; ++i) {
            sp.col[i] = float((color >> (i * 8)) & 0xFF);
        }
    }
    sp.x = x;
    sp.y = y;
    sp.radius = radius;
    sp.target_col = color;
    sp.target = strength;
}

void TrimCaches() {
    TrimCache(true);
    std::lock_guard lock{g_pic_mutex};
    g_icon_pics.clear();
}

void DrawBackdrop(Canvas& c) {
    const Field& F = g_field;
    if (F.v.empty() || F.w != c.Width() || F.h != c.Height()) {
        c.Clear(kColBg);
        return;
    }
    // Three channels ride in one vector (NEON on the Switch); the fourth lane carries alpha.
    typedef std::int32_t v4i __attribute__((vector_size(16)));
    typedef std::uint8_t v4b __attribute__((vector_size(4)));
    const auto& noise = Noise();
    std::vector<v4i> row(static_cast<std::size_t>(F.lw));
    u32* px = c.Data();
    const int x0 = c.ClipX0(), x1 = c.ClipX1();
    for (int y = c.ClipY0(); y < c.ClipY1(); ++y) {
        const int j = y >> 3, fy = y & 7;
        const int* a = &F.v[static_cast<std::size_t>(j) * F.lw * 3];
        const int* b = &F.v[static_cast<std::size_t>(j + 1) * F.lw * 3];
        v4i* rp = row.data();
        for (int i = 0; i < F.lw; ++i) {
            const v4i va = {a[i * 3], a[i * 3 + 1], a[i * 3 + 2], 255 * 256};
            const v4i vb = {b[i * 3], b[i * 3 + 1], b[i * 3 + 2], 255 * 256};
            rp[i] = (va * (8 - fy) + vb * fy) >> 3;
        }
        u32* out = px + static_cast<std::size_t>(y) * F.w;
        const u8* nrow = noise.data() + (y & 63) * 64;
        for (int x = x0; x < x1;) {
            const int i = x >> 3;
            const v4i d = rp[i + 1] - rp[i];
            v4i v = rp[i] * 8 + d * (x & 7); // eighths
            const int end = std::min(x1, (i + 1) * 8);
            for (; x < end; ++x) {
                // Values stay below 255 * 256 and the noise below 256, so nothing overflows a byte.
                const v4i o = ((v >> 3) + static_cast<int>(nrow[x & 63])) >> 8;
                const v4b bytes = __builtin_convertvector(o, v4b);
                std::memcpy(out + x, &bytes, sizeof(u32));
                v += d;
            }
        }
    }
}

// ---- pictures ----------------------------------------------------------------------------------------

void SetSystemImage(const std::string& id, Image img) {
    std::lock_guard lock{g_pic_mutex};
    if (img.Empty()) {
        g_system_pics.erase(id);
        return;
    }
    g_system_pics[id] = MakePicture(std::move(img));
}
bool HasSystemImage(const std::string& id) {
    std::lock_guard lock{g_pic_mutex};
    return g_system_pics.count(id) != 0;
}
void SetSystemLogo(const std::string& id, Image img) {
    std::lock_guard lock{g_pic_mutex};
    if (img.Empty()) {
        g_logo_pics.erase(id);
        return;
    }
    g_logo_pics[id] = MakePicture(std::move(img));
}
bool HasSystemLogo(const std::string& id) {
    std::lock_guard lock{g_pic_mutex};
    return g_logo_pics.count(id) != 0;
}
void SetGameImage(const std::string& path, Image img) {
    std::lock_guard lock{g_pic_mutex};
    if (img.Empty()) {
        g_game_pics.erase(path);
        return;
    }
    g_game_pics[path] = MakePicture(std::move(img));
}
bool HasGameImage(const std::string& path) {
    std::lock_guard lock{g_pic_mutex};
    return g_game_pics.count(path) != 0;
}
namespace {
Image FromRgba(std::vector<u32> rgba, int w, int h) {
    Image img;
    if (rgba.empty() || w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * h) {
        return img;
    }
    img.w = w;
    img.h = h;
    img.px = std::move(rgba);
    img.opaque = std::all_of(img.px.begin(), img.px.end(), [](u32 p) { return (p >> 24) == 0xFF; });
    return img;
}
} // namespace
void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h) {
    SetSystemImage(id, FromRgba(std::move(rgba), w, h));
}
void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h) {
    SetGameImage(path, FromRgba(std::move(rgba), w, h));
}
void SetAvatar(std::vector<u32> rgba, int w, int h) {
    g_avatar = FromRgba(std::move(rgba), w, h);
    ++g_avatar_id;
}

// ---- layout --------------------------------------------------------------------------------------------

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

// ---- profile bar -----------------------------------------------------------------------------------------

void DrawTopBar(Canvas& c, const Fonts& f, const TopBar& bar) {
    const int cx = 66, cy = 54, r = 30;
    // Avatar in a gradient ring, baked once.
    auto avatar = [&](Canvas& t, int ax, int ay) {
        t.Glow(ax - r, ay - r, 2 * r, 2 * r, r, 14, Black(0x80), true, 4);
        t.RingRoundAAWith(ax - r - 5, ay - r - 5, 2 * r + 10, 2 * r + 10, r + 5, 2.5f,
                          [](float p) { return RingAt(p * 0.6f, 0.05f); });
        if (!g_avatar.Empty()) {
            t.DrawImageScaled(g_avatar, float(ax - r), float(ay - r), float(2 * r), float(2 * r), r);
        } else {
            t.FillRoundGradient(ax - r, ay - r, 2 * r, 2 * r, r, MakeColor(0x6A, 0x60, 0x96), MakeColor(0x3A, 0x34, 0x5C));
            const std::string first = bar.title.empty() ? "?" : std::string(bar.title.substr(0, 1));
            const int w = f.bold->Measure(first, 28);
            f.bold->Draw(t, ax - w / 2, ay + 10, first, 28, kColText);
        }
    };
    constexpr int kAv = 56; // half the sprite
    const std::string_view letter = g_avatar.Empty() && !bar.title.empty() ? bar.title.substr(0, 1) : std::string_view{};
    const auto av = Cached(Key({6, g_avatar_id, std::hash<std::string_view>{}(letter)}), false, [&] {
        return Gfx::RenderSprite(2 * kAv, 2 * kAv, kAv, kAv, [&](Canvas& t) { avatar(t, 0, 0); });
    });
    if (av) {
        c.DrawSprite(*av, cx - kAv, cy - kAv);
    } else {
        avatar(c, cx, cy);
    }

    const int mid = c.Width() / 2;
    auto title_block = [&](std::string_view title, std::string_view subtitle, float alpha, int dy) {
        if (alpha <= 0.01f) {
            return;
        }
        Canvas::FadeScope fade{c, alpha};
        const std::string t = f.bold->Truncate(title, 24, 560);
        const int tw = f.bold->Measure(t, 24);
        f.bold->Draw(c, mid - tw / 2, (subtitle.empty() ? 62 : 52) + dy, t, 24, kColText);
        if (!subtitle.empty()) {
            const std::string sub = f.regular->Truncate(subtitle, 15, 560);
            const int sw = f.regular->Measure(sub, 15);
            f.regular->Draw(c, mid - sw / 2, 76 + dy, sub, 15, kColTextDim);
        }
    };
    const float b = std::clamp(bar.blend, 0.0f, 1.0f);
    const float eased = 1 - (1 - b) * (1 - b);
    if (b < 1.0f) {
        title_block(bar.prev_title, bar.prev_subtitle, 1 - eased, static_cast<int>(-8 * eased));
    }
    title_block(bar.title, bar.subtitle, eased, static_cast<int>(8 * (1 - eased)));

    // Clock and battery in a glass capsule on the right.
    const int kw = f.bold->Measure(bar.clock, 26);
    const int aw = bar.ampm.empty() ? 0 : f.bold->Measure(bar.ampm, 13) + 5;
    const int batt_w = bar.battery >= 0 ? 36 + 12 + (bar.charging ? 14 : 0) : 0;
    const int cap_w = 22 + kw + aw + batt_w + 20, cap_h = 46;
    const int cap_x = c.Width() - 34 - cap_w, cap_y = 31;
    Glass(c, cap_x, cap_y, cap_w, cap_h, cap_h / 2, false, 0.55f);
    int x = cap_x + 22;
    f.bold->Draw(c, x, cap_y + 32, bar.clock, 26, kColText);
    x += kw + 5;
    if (!bar.ampm.empty()) {
        f.bold->Draw(c, x, cap_y + 32, bar.ampm, 13, kColTextDim);
        x += aw;
    }
    if (bar.battery >= 0) {
        x += 7;
        const int bw = 34, bh = 18, by = cap_y + (cap_h - bh) / 2;
        c.RingRoundAA(x, by, bw, bh, 6, 2.0f, White(0xE0));
        c.FillRoundAA(x + bw + 1, by + 6, 3, 6, 1, White(0xE0));
        const bool low = bar.battery <= 15 && !bar.charging;
        const u32 fill = low ? kColError : bar.charging ? MakeColor(0x6E, 0xE7, 0xA7) : kColText;
        c.FillRoundAA(x + 4, by + 4, std::max(2, (bw - 8) * std::clamp(bar.battery, 0, 100) / 100), bh - 8, 2, fill);
        if (bar.charging) {
            DrawIcon(c, DockIconShape::Bolt, float(x + bw + 13), float(cap_y + cap_h / 2), 16, kColText);
        }
    }
}

// ---- dock ------------------------------------------------------------------------------------------------

int DockHitTest(const Canvas& c, int n, int x, int y) {
    (void)c;
    const int dx = DockX(n), dy = DockY();
    if (y < dy || y > dy + kDockH || x < dx + 16 || x >= dx + 16 + n * kDockStep) return -1;
    return (x - dx - 16) / kDockStep;
}

void DrawDock(Canvas& c, const Fonts& f, const std::vector<DockItem>& items, const DockState& s) {
    const int n = static_cast<int>(items.size());
    if (n == 0) {
        return;
    }
    const int x = DockX(n), y = DockY(), w = DockW(n);
    // The glass body and the ZL / ZR keycaps at its ends never change: baked once.
    constexpr int kKeyW = 40, kKeyH = 26, kKeyGap = 12, kMargin = 30;
    auto body = [&](Canvas& t, int bx, int by) {
        GlassDirect(t, bx, by, w, kDockH, kDockH / 2, true, 0.75f);
        for (int side = 0; side < 2; ++side) {
            const char* key = side ? "ZR" : "ZL";
            const int kx = side ? bx + w + kKeyGap : bx - kKeyGap - kKeyW, ky = by + (kDockH - kKeyH) / 2;
            t.FillRoundAA(kx, ky, kKeyW, kKeyH, 9, White(0x14));
            t.RingRoundAA(kx, ky, kKeyW, kKeyH, 9, 1.0f, White(0x26));
            const int tw = f.bold->Measure(key, 12);
            f.bold->Draw(t, kx + (kKeyW - tw) / 2, CenterBaseline(ky, kKeyH, 12), key, 12, kColTextDim);
        }
    };
    const int left = kKeyGap + kKeyW + kMargin;
    const auto sprite = Cached(Key({5, u64(w)}), false, [&] {
        return Gfx::RenderSprite(w + 2 * left, kDockH + 2 * kMargin + 10, left, kMargin, [&](Canvas& t) { body(t, 0, 0); });
    });
    if (sprite) {
        c.DrawSprite(*sprite, x - left, y - kMargin);
    } else {
        body(c, x, y);
    }

    // The highlight glides between items.
    const float hx = x + 16 + s.slide * kDockStep + kDockStep / 2.0f;
    const int pw = 66, ph = kDockH - 14;
    const int px = static_cast<int>(std::lround(hx - pw / 2.0f)), py = y + 7;
    if (s.focused) {
        c.Glow(px, py, pw, ph, ph / 2, 16, WithAlpha(kColAccent, 0x50), true);
        c.FillRoundAA(px, py, pw, ph, ph / 2, WithAlpha(kColAccent, 0x2C));
        c.RingRoundAAWith(px, py, pw, ph, ph / 2, 1.6f, [&](float p) { return WithAlpha(RingAt(p, s.t * 0.1f), 0xD0); });
    } else {
        c.FillRoundAA(px, py, pw, ph, ph / 2, White(0x18));
        c.RingRoundAA(px, py, pw, ph, ph / 2, 1.0f, White(0x1C));
    }

    for (int i = 0; i < n; ++i) {
        const float cx = x + 16 + i * kDockStep + kDockStep / 2.0f, cy = y + kDockH / 2.0f;
        const bool on = i == s.active;
        // Items near the highlight lift slightly.
        const float near = std::max(0.0f, 1.0f - std::fabs(s.slide - i));
        const u32 col = on ? kColAccent : Canvas::Mix(White(0xC8), kWhite, near);
        const int size = 34 + static_cast<int>(4 * near);
        const float iy = cy - 1.5f * near;
        switch (items[i].icon) {
        case DockIcon::Home: DrawIcon(c, DockIconShape::Home, cx, iy, size, col); break;
        case DockIcon::Systems: DrawIcon(c, DockIconShape::Controller, cx, iy + 1, size + 4, col); break;
        case DockIcon::Install: DrawIcon(c, DockIconShape::Download, cx, iy, size, col); break;
        case DockIcon::Settings: DrawIcon(c, DockIconShape::Gear, cx, iy, size, col); break;
        case DockIcon::Folder: DrawIcon(c, DockIconShape::Folder, cx, iy, size, col); break;
        case DockIcon::Text: {
            const char* t = items[i].text ? items[i].text : "?";
            const int ts = 17 + static_cast<int>(2 * near);
            const int tw = f.bold->Measure(t, ts);
            f.bold->Draw(c, static_cast<int>(cx) - tw / 2, static_cast<int>(iy + ts * 0.36f), t, ts, col);
            break;
        }
        }
        if (on) {
            c.Disc(cx, float(y + kDockH - 8), 2.2f, kColAccent);
        }
    }

    // Label bubble over the highlight.
    if (s.label > 0.01f) {
        Canvas::FadeScope fade{c, s.label};
        const int shown = std::clamp(static_cast<int>(std::lround(s.slide)), 0, n - 1);
        const char* label = items[static_cast<std::size_t>(shown)].label;
        const int lw = f.bold->Measure(label, 16) + 30;
        const int bx = static_cast<int>(hx) - lw / 2, by = y - 44 + static_cast<int>(6 * (1 - s.label));
        Glass(c, bx, by, lw, 32, 16, true, 0.9f);
        f.bold->Draw(c, bx + 15, CenterBaseline(by, 32, 16), label, 16, kColText);
    }
}

// ---- tiles --------------------------------------------------------------------------------------------------

u32 TileAccent(const TileInfo& t) {
    if (!t.art_key.empty()) {
        if (auto p = Find(g_game_pics, t.art_key)) {
            return p->accent;
        }
    }
    if (auto p = IconPicture(t)) {
        return p->accent;
    }
    return t.system_color;
}

void DrawEmptySlot(Canvas& c, int x, int y, float alpha) {
    if (alpha <= 0.01f) {
        return;
    }
    Canvas::FadeScope fade{c, alpha};
    c.RingRoundAA(x, y, kTileW, kTileH, kTileR, 1.2f, White(0x16));
    c.Disc(x + kTileW / 2.0f, y + kTileH / 2.0f, 2.6f, White(0x40));
}

void DrawFocusRing(Canvas& c, float x, float y, float w, float t, float alpha) {
    if (alpha <= 0.01f) {
        return;
    }
    Canvas::FadeScope fade{c, alpha};
    const int ix = static_cast<int>(std::lround(x)) - 6, iy = static_cast<int>(std::lround(y)) - 6;
    const int iw = static_cast<int>(std::lround(w)) + 12;
    const int r = kTileR + 7;
    c.RingRoundAAWith(ix, iy, iw, iw, r, 3.2f, [t](float p) { return RingAt(p, t * 0.12f); });
    // A faint outer halo so the ring reads on bright pictures too.
    c.RingRoundAA(ix - 2, iy - 2, iw + 4, iw + 4, r + 2, 2.0f, Black(0x50));
}

namespace {

int TileRadius(int s) {
    return kTileR + (s - kTileW) / 8;
}

// Gloss over the top and a fine bright edge, baked into a finished face.
void FinishFace(Canvas& c, int s, int r, float edge) {
    c.FillRoundAAWith(0, 0, s, s * 2 / 5, r, [](float k) { return White(A(0.13f * (1 - k) * (1 - k))); });
    c.RingRoundAA(0, 0, s, s, r, 1.2f, White(A(edge)));
}

Image CanvasImage(Canvas& c) {
    Image img;
    img.w = c.Width();
    img.h = c.Height();
    img.px.assign(c.Data(), c.Data() + static_cast<std::size_t>(img.w) * img.h);
    img.opaque = true;
    return img;
}

// A game's face at size s: its picture (or its initials on the system's colour) with the
// glassy finish, opaque, square; corners are rounded when it's drawn.
std::shared_ptr<const Image> TileFace(const Fonts& f, const TileInfo& t, const std::shared_ptr<const Picture>& pic, int s) {
    const int r = TileRadius(s);
    const float edge = s > kTileW ? 0.26f : 0.16f;
    if (pic) {
        return Cached(Key({3, pic->id, u64(s), t.system_color}), true, [&] {
            const Image face = BuildSquare(pic->img, s);
            Canvas tmp;
            tmp.Resize(s, s);
            if (!face.opaque) {
                const u32 col = t.system_color;
                tmp.FillRoundAAWith(0, 0, s, s, 0, [col](float k) {
                    return Canvas::Mix(Canvas::Mix(col, kWhite, 0.10f), Canvas::Mix(col, kBlack, 0.45f), k);
                });
            }
            tmp.DrawImage(face, 0, 0);
            FinishFace(tmp, s, r, edge);
            return CanvasImage(tmp);
        });
    }
    return Cached(Key({4, std::hash<std::string_view>{}(t.title), u64(s), t.system_color}), true, [&] {
        const u32 col = t.system_color;
        Canvas tmp;
        tmp.Resize(s, s);
        tmp.FillRoundAAWith(0, 0, s, s, 0, [col](float k) {
            return Canvas::Mix(Canvas::Mix(col, kBlack, 0.40f), Canvas::Mix(col, MakeColor(0x08, 0x08, 0x10), 0.80f), k);
        });
        tmp.Disc(s * 0.86f, s * 0.10f, s * 0.62f, White(0x10));
        tmp.Circle(s * 0.10f, s * 0.98f, s * 0.42f, 6.0f, White(0x0A));
        const std::string ini = Initials(t.title);
        const int ts = std::max(16, static_cast<int>(s * 0.31f));
        const int iw = f.mark->Measure(ini, ts);
        f.mark->Draw(tmp, (s - iw) / 2 + 1, s / 2 + static_cast<int>(ts * 0.36f) + 3, ini, ts, Black(0x50));
        f.mark->Draw(tmp, (s - iw) / 2, s / 2 + static_cast<int>(ts * 0.36f), ini, ts, kColText);
        FinishFace(tmp, s, r, edge);
        return CanvasImage(tmp);
    });
}

std::shared_ptr<const Picture> TilePicture(const TileInfo& t) {
    if (!t.art_key.empty()) {
        if (auto p = Find(g_game_pics, t.art_key)) {
            return p;
        }
    }
    return IconPicture(t);
}

} // namespace

void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, const TileLook& look) {
    if (look.appear <= 0.01f) {
        return;
    }
    const float lift = look.lift;
    const float size = kTileW + (kTileFocus - kTileW) * lift;
    const int s = std::max(8, static_cast<int>(std::lround(size)));
    const float cxf = x + kTileW / 2.0f;
    const float cyf = y + kTileH / 2.0f - 3.0f * std::max(0.0f, lift) + (1.0f - look.appear) * 26.0f;
    const int tx = static_cast<int>(std::lround(cxf - s / 2.0f)), ty = static_cast<int>(std::lround(cyf - s / 2.0f));
    const int r = TileRadius(s);
    const int rest = lift > 0.5f ? kTileFocus : kTileW;
    const std::shared_ptr<const Picture> pic = TilePicture(t);
    // Looked up (and, in the warm-up pass, built) even when off this band.
    const std::shared_ptr<const Image> face = TileFace(f, t, pic, rest);
    if (!c.RowsVisible(ty - 48, s + 96)) {
        return;
    }
    Canvas::FadeScope fade{c, look.appear * (look.dim ? 0.82f : 1.0f)};
    const float lk = std::clamp(lift, 0.0f, 1.0f);

    // Shadow, and a glow in the picture's own colour when focused.
    c.SoftShadow(tx, ty, s, s, r, 10 + static_cast<int>(12 * lk), 4 + static_cast<int>(8 * lk), A(0.42f + 0.28f * lk));
    if (lk > 0.02f) {
        c.Glow(tx, ty, s, s, r, 34, WithAlpha(pic ? pic->accent : t.system_color, A(0.5f * lk)), true, 8);
    }

    if (face) {
        if (face->w == s) {
            c.DrawImage(*face, tx, ty, r);
        } else {
            c.DrawImageScaled(*face, float(tx), float(ty), float(s), float(s), r);
        }
    } else if (pic) {
        // Not built yet: the plain picture, scaled, keeps the tile from popping in.
        c.FillRoundAA(tx, ty, s, s, r, Canvas::Mix(t.system_color, kBlack, 0.5f));
        const float k = std::min(float(s) / pic->img.w, float(s) / pic->img.h);
        c.DrawImageScaled(pic->img, tx + (s - pic->img.w * k) / 2, ty + (s - pic->img.h * k) / 2, pic->img.w * k,
                          pic->img.h * k, r / 2);
    } else {
        c.FillRoundGradient(tx, ty, s, s, r, Canvas::Mix(t.system_color, kBlack, 0.40f),
                            Canvas::Mix(t.system_color, kBlack, 0.80f));
    }

    // Now and then a band of light sweeps across the focused tile.
    if (lk > 0.6f) {
        constexpr float kPeriod = 4.5f, kSweep = 0.85f;
        const float phase = std::fmod(look.t + 1.2f, kPeriod) / kSweep;
        if (phase < 1.0f) {
            const float pos = -0.25f + 1.5f * phase;
            c.FillRoundAAPix(tx, ty, s, s, r, [pos, lk](float fx, float fy) {
                const float d = std::fabs((fx + fy) * 0.5f - pos);
                return White(A(0.22f * lk * std::max(0.0f, 1.0f - d / 0.07f)));
            });
        }
    }

    if (!t.system_badge.empty()) Chip(c, f, tx + s - 8, ty + s - 8, t.system_badge, t.system_color);
    int chip_x = tx + 8;
    for (std::string_view tag : t.tags) {
        const int cw = f.bold->Measure(tag, 11) + 12;
        c.FillRoundAA(chip_x, ty + 8, cw, 18, 9, Black(0xB8));
        f.bold->Draw(c, chip_x + 6, CenterBaseline(ty + 8, 18, 11), tag, 11, kColText);
        chip_x += cw + 4;
    }
}

void DrawTitlePill(Canvas& c, const Fonts& f, int y, std::string_view title, std::string_view system, u32 color) {
    const std::string t = f.bold->Truncate(title, 20, 640);
    const int tw = f.bold->Measure(t, 20), sw = f.regular->Measure(system, 15);
    const int w = tw + sw + 70, h = 40;
    const int x = (c.Width() - w) / 2;
    Glass(c, x, y, w, h, h / 2, true, 0.8f);
    c.Disc(x + 22.0f, y + h / 2.0f, 5.5f, color);
    f.bold->Draw(c, x + 36, CenterBaseline(y, h, 20), t, 20, kColText);
    f.regular->Draw(c, x + w - 18 - sw, CenterBaseline(y, h, 15), system, 15, kColTextDim);
}

// ---- systems carousel --------------------------------------------------------------------------------------

void DrawController(Canvas& c, float cx, float cy, float width, u32 accent, float t) {
    const int w = static_cast<int>(width);
    if (!Shapes::PadReady(w)) {
        if (!MayBuild()) {
            return;
        }
        Shapes::BuildPad(w, [](int n, const std::function<void(int)>& fn) { Gfx::Workers::Get().Run(n, fn); });
    }
    // A slow float, as if it were hovering over its shadow.
    const float bob = std::sin(t * 1.25f) * 4.0f;
    // Once the colour settles the finished controller is kept as a sprite; while it is still
    // changing, it is composed straight onto the canvas.
    static u32 last_accent = 0;
    const bool settled = accent == last_accent;
    if (MayBuild()) {
        last_accent = accent;
    }
    std::shared_ptr<const Image> sprite;
    if (settled) {
        const int sw = static_cast<int>(std::ceil(0.94f * w)) + 2, sh = static_cast<int>(std::ceil(0.68f * w)) + 2;
        sprite = Cached(Key({8, u64(w), accent}), false, [&] {
            return Gfx::RenderSprite(sw, sh, sw / 2, static_cast<int>(0.25f * w) + 1,
                                     [&](Canvas& tc) { Shapes::DrawPad(tc, 0.0f, 0.0f, w, accent); });
        });
        if (sprite) {
            c.DrawSprite(*sprite, static_cast<int>(std::lround(cx)) - sw / 2,
                         static_cast<int>(std::lround(cy + bob)) - (static_cast<int>(0.25f * w) + 1));
            return;
        }
    }
    Shapes::DrawPad(c, std::round(cx), std::round(cy + bob), w, accent);
}

void DrawGamepad(Canvas& c, float cx, float cy, float size, u32 color, float) {
    DrawIcon(c, DockIconShape::Controller, cx, cy, static_cast<int>(size), color);
}

int CarouselHitTest(const Canvas& c, int count, float anim, int x, int y) {
    (void)c;
    for (int i = 0; i < count; ++i) {
        if (std::fabs(i - anim) > 1.6f) continue;
        const CardRect r = CardAt(i, anim);
        if (x >= r.x && x < r.x + r.s && y >= r.y && y < r.y + r.s) return i;
    }
    return -1;
}

bool CarouselPictureButtonHit(const Canvas& c, int x, int y) {
    (void)c;
    const Rect b = PictureButton();
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

namespace {

// A console card's face at size s: its picture (or its default look) made square, with a gloss
// and a fine edge. Corners are rounded when it's drawn.
std::shared_ptr<const Image> CardFace(const Fonts& f, const SystemCard& card, int s) {
    std::shared_ptr<const Picture> pic = Find(g_system_pics, card.id);
    if (!pic) {
        pic = DefaultCard(f, card);
    }
    if (!pic) {
        return nullptr;
    }
    return Cached(Key({7, pic->id, u64(s), card.color}), true, [&] {
        const Image sq = BuildSquare(pic->img, s);
        Canvas tmp;
        tmp.Resize(s, s);
        if (!sq.opaque) {
            const u32 col = card.color;
            tmp.FillRoundAAWith(0, 0, s, s, 0, [col](float k) {
                return Canvas::Mix(Canvas::Mix(col, kWhite, 0.10f), Canvas::Mix(col, kBlack, 0.55f), k);
            });
        }
        tmp.DrawImage(sq, 0, 0);
        const int rr = static_cast<int>(s * 0.14f);
        tmp.FillRoundAAWith(0, 0, s, s * 2 / 5, rr, [](float k) { return White(A(0.12f * (1 - k) * (1 - k))); });
        tmp.RingRoundAA(0, 0, s, s, rr, 1.4f, White(0x30));
        return CanvasImage(tmp);
    });
}

// A small filled triangle, pointing up or down, centred on (cx, cy); edges are antialiased.
void Arrow(Canvas& c, float cx, float cy, float size, bool up, u32 color) {
    const float hw = size, hh = size * 0.8f, top = cy - hh / 2;
    const int x0 = static_cast<int>(std::floor(cx - hw - 1)), x1 = static_cast<int>(std::ceil(cx + hw + 1));
    const int y0 = static_cast<int>(std::floor(top - 1)), y1 = static_cast<int>(std::ceil(top + hh + 1));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const float v0 = (y + (sy + 0.5f) / 4.0f - top) / hh;
                if (v0 < 0.0f || v0 > 1.0f) {
                    continue;
                }
                const float half = hw * (up ? v0 : 1.0f - v0);
                for (int sx = 0; sx < 4; ++sx) {
                    hits += std::fabs(x + (sx + 0.5f) / 4.0f - cx) <= half ? 1 : 0;
                }
            }
            if (hits > 0) {
                c.Blend(x, y, color, static_cast<u8>(hits * 255 / 16));
            }
        }
    }
}

} // namespace

void DrawSystemsCarousel(Canvas& c, const Fonts& f, const std::vector<SystemCard>& cards, float anim, int selected,
                         const CarouselText& text, u32 accent, float t) {
    if (cards.empty()) return;
    const float mid_y = CarouselMidY();
    // The controller on the left, in the focused system's colour (the pool of light under it is
    // the backdrop's spot, see SetSpot).
    DrawController(c, 225.0f, mid_y + 6.0f, 330.0f, accent, t);

    const int lo = std::max(0, static_cast<int>(std::floor(anim)) - 2);
    const int hi = std::min(static_cast<int>(cards.size()) - 1, static_cast<int>(std::ceil(anim)) + 2);
    // Neighbours first, the focused card last so its glow lies over them.
    std::vector<int> order;
    for (int i = lo; i <= hi; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [anim](int a, int b) { return std::fabs(a - anim) > std::fabs(b - anim); });
    for (int i : order) {
        if (std::fabs(i - anim) > 1.6f) continue;
        const CardRect cr = CardAt(i, anim);
        if (cr.y < kContentTop - 4 || cr.y + cr.s > c.Height() - kHintH - 40) continue;
        const SystemCard& card = cards[static_cast<std::size_t>(i)];
        const int r = static_cast<int>(cr.s * 0.14f);
        const float fade = 0.55f + 0.45f * cr.k;
        Canvas::FadeScope fs{c, fade};
        c.SoftShadow(cr.x, cr.y, cr.s, cr.s, r, 16, 8, A(0.5f + 0.2f * cr.k));
        if (cr.k > 0.05f) {
            c.Glow(cr.x, cr.y, cr.s, cr.s, r, 44, WithAlpha(card.color, A(0.5f * cr.k)), true, 10);
        }
        const int rest = cr.k > 0.5f ? static_cast<int>(kCardBig) : static_cast<int>(kCardSmall);
        const std::shared_ptr<const Image> face = CardFace(f, card, rest);
        if (face && face->w == cr.s) {
            c.DrawImage(*face, cr.x, cr.y, r);
        } else if (face) {
            c.DrawImageScaled(*face, float(cr.x), float(cr.y), float(cr.s), float(cr.s), r);
        } else {
            c.FillRoundGradient(cr.x, cr.y, cr.s, cr.s, r, card.color, Canvas::Mix(card.color, kBlack, 0.6f));
        }
        if (i == selected) {
            c.RingRoundAAWith(cr.x - 6, cr.y - 6, cr.s + 12, cr.s + 12, r + 6, 3.2f,
                              [t](float p) { return RingAt(p, t * 0.12f); });
            if (text.moving) {
                // Being moved: arrows above and below, nudging the way it can go.
                const float bob = std::sin(t * 6.0f) * 3.0f;
                const float cx = cr.x + cr.s / 2.0f;
                Arrow(c, cx, cr.y - 26.0f - bob, 12.0f, true, kColAccent);
                Arrow(c, cx, cr.y + cr.s + 26.0f + bob, 12.0f, false, kColAccent);
            }
        }
    }

    // Name, game count, status and the picture button beside the focused card.
    const int tx = CarouselTextX();
    f.bold->Draw(c, tx, static_cast<int>(mid_y - 18), f.bold->Truncate(text.name, 40, c.Width() - tx - 40), 40, kColText);
    f.regular->Draw(c, tx, static_cast<int>(mid_y + 20), text.detail, 19, kColTextDim);
    c.Disc(tx + 6.0f, mid_y + 47, 4.5f, text.status_ok ? MakeColor(0x6E, 0xE7, 0xB7) : MakeColor(0xFF, 0xCE, 0x78));
    f.regular->Draw(c, tx + 18, static_cast<int>(mid_y + 53), text.status, 16, kColTextDim);

    if (!text.picture_button || text.moving) {
        return;
    }
    const Rect b = PictureButton();
    Glass(c, b.x, b.y, b.w, b.h, b.h / 2, false, 0.7f);
    const int chip = 24;
    c.FillRoundAA(b.x + 9, b.y + (b.h - chip) / 2, chip, chip, chip / 2, White(0xEE));
    const int yw = f.bold->Measure("Y", 13);
    f.bold->Draw(c, b.x + 9 + (chip - yw) / 2, CenterBaseline(b.y + (b.h - chip) / 2, chip, 13), "Y", 13,
                 MakeColor(0x1A, 0x18, 0x24));
    DrawIcon(c, DockIconShape::Picture, float(b.x + 52), float(b.y + b.h / 2), 22, kColText);
    f.bold->Draw(c, b.x + 70, CenterBaseline(b.y, b.h, 16), text.has_picture ? "Change picture" : "Set a picture", 16,
                 kColText);
}

// ---- Home sections and a console's games -----------------------------------------------------------------

int DrawSystemLogo(Canvas& c, const Fonts& f, const SystemCard& card, int x, int y, int h) {
    if (auto logo = Find(g_logo_pics, card.id)) {
        // A logo keeps its shape: as tall as the band, as wide as that makes it (within reason).
        const float k = std::min(float(h) / logo->img.h, 340.0f / logo->img.w);
        const int w = std::max(1, static_cast<int>(std::lround(logo->img.w * k)));
        const int lh = std::max(1, static_cast<int>(std::lround(logo->img.h * k)));
        const auto img = Cached(Key({13, logo->id, u64(w), u64(lh)}), true, [&] { return Gfx::Resize(logo->img, w, lh); });
        if (img) {
            c.DrawImage(*img, x, y + (h - lh) / 2);
        }
        return w;
    }
    // No logo: the console's short name, heavy and slanted, in a pill of its colour.
    const int ts = std::max(12, static_cast<int>(h * 0.5f));
    const int tw = f.mark->Measure(card.badge, ts);
    const int w = tw + h;
    const auto pill = Cached(Key({12, std::hash<std::string_view>{}(card.id), u64(h), card.color}), false, [&] {
        return Gfx::RenderSprite(w + 12, h + 12, 6, 6, [&](Canvas& t) {
            const u32 col = card.color;
            t.FillRoundAAWith(0, 0, w, h, h / 2, [col](float k) {
                return Canvas::Mix(Canvas::Mix(col, kWhite, 0.16f), Canvas::Mix(col, kBlack, 0.38f), k);
            });
            t.FillRoundAAWith(0, 0, w, h / 2, h / 2, [](float k) { return White(A(0.18f * (1 - k))); });
            t.RingRoundAA(0, 0, w, h, h / 2, 1.2f, White(0x48));
            const int bx = (w - tw) / 2, by = h / 2 + static_cast<int>(ts * 0.36f);
            f.mark->Draw(t, bx + 1, by + 2, card.badge, ts, Black(0x60));
            f.mark->Draw(t, bx, by, card.badge, ts, kWhite);
        });
    });
    if (pill) {
        c.Glow(x, y, w, h, h / 2, 14, WithAlpha(card.color, 0x50), true, 3);
        c.DrawSprite(*pill, x - 6, y - 6);
    }
    return w;
}

void DrawSectionHeader(Canvas& c, const Fonts& f, const SystemCard& card, std::string_view name,
                       std::string_view count, int x, int y, int w, float alpha) {
    // No early return for rows off this band: the warm-up pass has to build the logo.
    if (alpha <= 0.01f) {
        return;
    }
    Canvas::FadeScope fade{c, alpha};
    constexpr int kBand = 36;
    const int by = y + 4;
    const bool has_logo = HasSystemLogo(std::string{card.id});
    int tx = x + DrawSystemLogo(c, f, card, x, by, has_logo ? 40 : kBand) + 16;
    const int base = CenterBaseline(by, kBand, 22);
    if (!has_logo) {
        f.bold->Draw(c, tx, base, name, 22, kColText);
        tx += f.bold->Measure(name, 22) + 14;
    }
    f.regular->Draw(c, tx, CenterBaseline(by, kBand, 16), count, 16, kColTextDim);
    tx += f.regular->Measure(count, 16) + 18;
    if (tx < x + w) {
        // A hairline that fades out to the right.
        const int lw = x + w - tx;
        c.FillRoundAAPix(tx, by + kBand / 2, lw, 1, 0, [](float fx, float) { return White(A(0.16f * (1.0f - fx))); });
    }
}

void DrawSystemCard(Canvas& c, const Fonts& f, const SystemCard& card, int x, int y, int s) {
    const int r = static_cast<int>(s * 0.14f);
    c.SoftShadow(x, y, s, s, r, 18, 8, A(0.6f));
    c.Glow(x, y, s, s, r, 40, WithAlpha(card.color, A(0.45f)), true, 10);
    if (const auto face = CardFace(f, card, s)) {
        c.DrawImage(*face, x, y, r);
    } else {
        c.FillRoundGradient(x, y, s, s, r, card.color, Canvas::Mix(card.color, kBlack, 0.6f));
    }
}

void DrawGameRow(Canvas& c, const Fonts& f, const TileInfo& t, std::string_view detail, int x, int y, int w,
                 int h, float focus) {
    const int s = h - 16;
    const std::shared_ptr<const Picture> pic = TilePicture(t);
    // Looked up (and, in the warm-up pass, built) even when off this band.
    const std::shared_ptr<const Image> face = TileFace(f, t, pic, s);
    if (!c.RowsVisible(y - 24, h + 48)) {
        return;
    }
    const int r = std::min(18, h / 2);
    const float k = std::clamp(focus, 0.0f, 1.0f);
    if (k > 0.01f) {
        c.Glow(x, y, w, h, r, 18, WithAlpha(kColAccent, A(0.22f * k)), true, 2);
        c.FillRoundAAWith(x, y, w, h, r, [k](float v) { return White(A((0.13f - 0.05f * v) * k)); });
        c.RingRoundAA(x, y, w, h, r, 1.2f, White(A(0.16f * k)));
    } else {
        c.FillRoundAA(x, y, w, h, r, White(0x09));
    }
    const int px = x + 8, py = y + 8;
    const int pr = std::max(8, TileRadius(s));
    c.SoftShadow(px, py, s, s, pr, 8, 3, A(0.4f));
    if (face) {
        c.DrawImage(*face, px, py, pr);
    } else {
        c.FillRoundGradient(px, py, s, s, pr, Canvas::Mix(t.system_color, kBlack, 0.40f),
                            Canvas::Mix(t.system_color, kBlack, 0.80f));
    }
    const int tx = px + s + 18;
    const int right = x + w - (k > 0.5f ? 110 : 20);
    f.bold->Draw(c, tx, y + h / 2 - 3, f.bold->Truncate(t.title, 21, right - tx), 21, kColText);
    f.regular->Draw(c, tx, y + h / 2 + 21, f.regular->Truncate(detail, 15, right - tx), 15, kColTextDim);
    if (k > 0.5f) {
        // "A Play" on the focused row.
        Canvas::FadeScope fs{c, (k - 0.5f) * 2.0f};
        const int chip = 26, cx = x + w - 96, cy = y + (h - chip) / 2;
        c.FillRoundAA(cx, cy, chip, chip, chip / 2, White(0xEE));
        const int aw = f.bold->Measure("A", 14);
        f.bold->Draw(c, cx + (chip - aw) / 2, CenterBaseline(cy, chip, 14), "A", 14, MakeColor(0x1A, 0x18, 0x24));
        f.bold->Draw(c, cx + chip + 9, CenterBaseline(cy, chip, 17), "Play", 17, kColText);
    }
}

// ---- panels ------------------------------------------------------------------------------------------------

void DrawScrim(Canvas& c, float alpha) {
    c.FillRect(0, 0, c.Width(), c.Height(), MakeColor(0x03, 0x03, 0x07, A(0.66f * alpha)));
}

void DrawPanel(Canvas& c, int x, int y, int w, int h, int r) {
    Glass(c, x, y, w, h, r, true, 1.0f);
}

void DrawModal(Canvas& c, int x, int y, int w, int h) {
    DrawScrim(c, 1.0f);
    DrawPanel(c, x, y, w, h, 20);
}

void DrawRow(Canvas& c, int x, int y, int w, int h, bool focused) {
    const int r = std::min(12, h / 2);
    if (focused) {
        c.FillRoundAAWith(x, y, w, h, r, [](float t) { return White(A(0.12f - 0.04f * t)); });
        c.RingRoundAA(x, y, w, h, r, 1.0f, White(0x22));
        c.FillRoundGradient(x + 6, y + 8, 4, h - 16, 2, kColAccent, kColAccent2);
    } else {
        c.FillRoundAA(x, y, w, h, r, White(0x0A));
    }
}

void DrawPill(Canvas& c, int x, int y, int w, int h, bool active) {
    if (active) {
        c.Glow(x, y, w, h, h / 2, 12, WithAlpha(kColAccent, 0x40), true, 3);
        c.FillRoundAAPix(x, y, w, h, h / 2, [](float fx, float) { return Canvas::Mix(kColAccent, MakeColor(0xA8, 0x9E, 0xFF), fx); });
    } else {
        c.FillRoundAA(x, y, w, h, h / 2, White(0x10));
        c.RingRoundAA(x, y, w, h, h / 2, 1.0f, White(0x1A));
    }
}

void DrawProgress(Canvas& c, int x, int y, int w, int h, float frac, float t) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    c.FillRoundAA(x, y, w, h, h / 2, White(0x16));
    const int fw = std::max(h, static_cast<int>(w * frac));
    if (frac <= 0.0f) {
        return;
    }
    const float sweep = std::fmod(t * 0.6f, 1.4f) - 0.2f;
    c.FillRoundAAPix(x, y, fw, h, h / 2, [fw, w, sweep](float fx, float) {
        const float gx = fx * fw / std::max(1, w);
        const u32 base = Canvas::Mix(kColAccent, kColAccent2, gx);
        const float d = std::fabs(gx - sweep);
        return Canvas::Mix(base, kWhite, 0.35f * std::max(0.0f, 1.0f - d / 0.12f));
    });
}

void DrawSpinner(Canvas& c, float cx, float cy, float r, float t) {
    constexpr int kDots = 10;
    for (int i = 0; i < kDots; ++i) {
        const float a = (i / float(kDots)) * 6.2831853f + t * 5.0f;
        const float k = (i + 1) / float(kDots);
        c.Disc(cx + std::cos(a) * r, cy + std::sin(a) * r, 2.0f + 2.2f * k, WithAlpha(RingAt(k * 0.8f), A(0.25f + 0.75f * k)));
    }
}

void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h) {
    c.FillRoundAA(x, top, 4, track_h, 2, White(0x12));
    c.FillRoundGradient(x, thumb_y, 4, thumb_h, 2, kColAccent, kColAccent2);
}

// ---- small pieces --------------------------------------------------------------------------------------------

int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label) {
    constexpr int chip_h = 26;
    const int letter_w = f.bold->Measure(button, 14);
    const int chip_w = std::max(chip_h, letter_w + 14);
    c.FillRoundAA(x, y, chip_w, chip_h, chip_h / 2, White(0xEE));
    f.bold->Draw(c, x + (chip_w - letter_w) / 2, CenterBaseline(y, chip_h, 14), button, 14, MakeColor(0x1A, 0x18, 0x24));
    const int label_w = f.bold->Draw(c, x + chip_w + 8, CenterBaseline(y, chip_h, 17), label, 17, kColText);
    return chip_w + 8 + label_w;
}

void DrawHintBar(Canvas&) {}

void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error, float alpha) {
    if (alpha <= 0.01f) {
        return;
    }
    const float e = 1 - (1 - alpha) * (1 - alpha);
    Canvas::FadeScope fade{c, e};
    const int tw = f.regular->Measure(text, 18);
    const int w = std::min(c.Width() - 60, tw + 64), h = 46;
    const int x = (c.Width() - w) / 2;
    const int y = c.Height() - kHintH - h - 58 + static_cast<int>(18 * (1 - e));
    Glass(c, x, y, w, h, h / 2, true, 1.0f);
    if (error) {
        c.RingRoundAA(x, y, w, h, h / 2, 1.4f, WithAlpha(kColError, 0xB0));
    }
    c.Disc(x + 24.0f, y + h / 2.0f, 5.0f, error ? kColError : kColAccent);
    f.regular->Draw(c, x + 40, CenterBaseline(y, h, 18), f.regular->Truncate(text, 18, w - 60), 18, kColText);
}

void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir, float t) {
    const int cx = c.Width() / 2;
    const int top = kContentTop + (c.Height() - kHintH - kContentTop) / 2 - 130;
    DrawController(c, float(cx), top + 58.0f, 220.0f, MakeColor(0x4A, 0x9E, 0xB8), t);
    auto centred = [&](Font& font, std::string_view s, int y, int size, u32 col) {
        const int w = font.Measure(s, size);
        font.Draw(c, cx - w / 2, y, s, size, col);
    };
    centred(*f.bold, "No games yet", top + 172, 28, kColText);
    centred(*f.regular, "Put games in sdmc:/roms/<system>/  (3ds, ds, gba, ps2, n64, snes...)", top + 206, 17, kColTextDim);
    const std::string dir = f.regular->TruncateFront(roms_dir, 17, c.Width() - 120);
    centred(*f.regular, "3DS games can also go in " + dir, top + 232, 17, kColTextDim);
}

} // namespace SwitchFrontend::Skin
