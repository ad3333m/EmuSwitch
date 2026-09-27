// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The skin's vector art: dock icons and the shaded controller on the Systems page. Both are
// built from signed distance fields once per size, cached, and only tinted per frame.

#include "citra_switch/menu_skin_art.h"

#include <functional>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace SwitchFrontend::Skin::Shapes {
namespace {

using Gfx::Mask;

struct V2 {
    float x, y;
};
V2 operator-(V2 a, V2 b) {
    return {a.x - b.x, a.y - b.y};
}
float Len(V2 a) {
    return std::sqrt(a.x * a.x + a.y * a.y);
}
float Dot(V2 a, V2 b) {
    return a.x * b.x + a.y * b.y;
}
float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}
float Smooth(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

// ---- distance fields (negative inside) ----

float Circle(V2 p, V2 c, float r) {
    return Len(p - c) - r;
}
// A capsule from a to b.
float Capsule(V2 p, V2 a, V2 b, float r) {
    const V2 pa = p - a, ba = b - a;
    const float h = Clamp01(Dot(pa, ba) / std::max(1e-9f, Dot(ba, ba)));
    return Len({pa.x - ba.x * h, pa.y - ba.y * h}) - r;
}
// A rounded box centred on c with half extents (hx, hy).
float Box(V2 p, V2 c, float hx, float hy, float r) {
    const float qx = std::fabs(p.x - c.x) - hx + r, qy = std::fabs(p.y - c.y) - hy + r;
    return Len({std::max(qx, 0.0f), std::max(qy, 0.0f)}) + std::min(std::max(qx, qy), 0.0f) - r;
}
// A box rotated by angle a (radians) around its centre.
float BoxRot(V2 p, V2 c, float hx, float hy, float r, float a) {
    const float s = std::sin(a), co = std::cos(a);
    const V2 d = p - c;
    return Box({d.x * co + d.y * s, -d.x * s + d.y * co}, {0, 0}, hx, hy, r);
}
// Any triangle.
float Triangle(V2 p, V2 p0, V2 p1, V2 p2) {
    const V2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    const V2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    auto edge = [](V2 v, V2 e) {
        const float h = Clamp01(Dot(v, e) / std::max(1e-9f, Dot(e, e)));
        const V2 q{v.x - e.x * h, v.y - e.y * h};
        return Dot(q, q);
    };
    const float s = (e0.x * e2.y - e0.y * e2.x) >= 0 ? 1.0f : -1.0f;
    const float d = std::min(std::min(edge(v0, e0), edge(v1, e1)), edge(v2, e2));
    const float in0 = s * (v0.x * e0.y - v0.y * e0.x);
    const float in1 = s * (v1.x * e1.y - v1.y * e1.x);
    const float in2 = s * (v2.x * e2.y - v2.y * e2.x);
    const float side = std::min(std::min(in0, in1), in2);
    return -std::sqrt(d) * (side >= 0 ? 1.0f : -1.0f);
}
float Union(float a, float b) {
    return std::min(a, b);
}
float Cut(float a, float b) {
    return std::max(a, -b);
}
float SmoothUnion(float a, float b, float k) {
    const float h = Clamp01(0.5f + 0.5f * (b - a) / k);
    return b + (a - b) * h - k * h * (1 - h);
}
float SmoothCut(float a, float b, float k) {
    return -SmoothUnion(-a, b, k);
}

// ---- icons ----

// Icons live in a unit square centred on the origin.
float IconSdf(DockIconShape shape, V2 p) {
    switch (shape) {
    case DockIconShape::Home: {
        const float roof = Triangle(p, {-0.37f, -0.03f}, {0.0f, -0.35f}, {0.37f, -0.03f}) - 0.035f;
        const float body = Box(p, {0.0f, 0.12f}, 0.255f, 0.22f, 0.05f);
        const float door = Box(p, {0.0f, 0.25f}, 0.07f, 0.13f, 0.035f);
        return Cut(Union(roof, body), door);
    }
    case DockIconShape::Controller: {
        float body = Box(p, {0.0f, -0.04f}, 0.33f, 0.15f, 0.13f);
        body = SmoothUnion(body, Capsule(p, {-0.24f, -0.02f}, {-0.33f, 0.20f}, 0.12f), 0.06f);
        body = SmoothUnion(body, Capsule(p, {0.24f, -0.02f}, {0.33f, 0.20f}, 0.12f), 0.06f);
        const float dpad = Union(Box(p, {-0.19f, -0.04f}, 0.095f, 0.03f, 0.012f),
                                 Box(p, {-0.19f, -0.04f}, 0.03f, 0.095f, 0.012f));
        const float buttons = Union(Circle(p, {0.16f, -0.08f}, 0.042f), Circle(p, {0.24f, 0.0f}, 0.042f));
        return Cut(Cut(body, dpad), buttons);
    }
    case DockIconShape::Download: {
        float d = Capsule(p, {0.0f, -0.36f}, {0.0f, 0.10f}, 0.05f);
        d = Union(d, Capsule(p, {-0.17f, -0.06f}, {0.0f, 0.11f}, 0.05f));
        d = Union(d, Capsule(p, {0.17f, -0.06f}, {0.0f, 0.11f}, 0.05f));
        d = Union(d, Capsule(p, {-0.36f, 0.12f}, {-0.36f, 0.30f}, 0.05f));
        d = Union(d, Capsule(p, {-0.36f, 0.30f}, {0.36f, 0.30f}, 0.05f));
        return Union(d, Capsule(p, {0.36f, 0.12f}, {0.36f, 0.30f}, 0.05f));
    }
    case DockIconShape::Gear: {
        float d = Circle(p, {0, 0}, 0.27f);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159265f / 4;
            d = Union(d, BoxRot(p, {std::cos(a) * 0.31f, std::sin(a) * 0.31f}, 0.075f, 0.065f, 0.02f, a));
        }
        return Cut(d, Circle(p, {0, 0}, 0.11f));
    }
    case DockIconShape::Folder: {
        const float back = Box(p, {-0.16f, -0.21f}, 0.18f, 0.09f, 0.04f);
        const float body = Box(p, {0.0f, 0.04f}, 0.39f, 0.26f, 0.06f);
        const float flap = Box(p, {0.0f, 0.09f}, 0.39f, 0.21f, 0.06f);
        // A thin gap between the back and the front flap reads as a folder at small sizes.
        return Union(Cut(Union(back, body), Box(p, {0.0f, -0.12f}, 0.40f, 0.012f, 0.0f)), flap);
    }
    case DockIconShape::Bolt: {
        return Union(Triangle(p, {0.10f, -0.46f}, {-0.24f, 0.07f}, {0.05f, 0.07f}),
                     Triangle(p, {-0.05f, -0.07f}, {0.24f, -0.07f}, {-0.10f, 0.46f})) - 0.01f;
    }
    case DockIconShape::Picture: {
        // A framed landscape: sun and two hills.
        const float frame = Box(p, {0.0f, 0.0f}, 0.40f, 0.32f, 0.07f);
        const float inner = Box(p, {0.0f, 0.0f}, 0.33f, 0.25f, 0.035f);
        const float hills = Union(Triangle(p, {-0.33f, 0.25f}, {-0.08f, -0.05f}, {0.17f, 0.25f}),
                                  Triangle(p, {0.02f, 0.25f}, {0.19f, 0.04f}, {0.36f, 0.25f}));
        const float sun = Circle(p, {0.17f, -0.12f}, 0.065f);
        return Union(Cut(frame, inner), Union(std::max(hills, inner), sun));
    }
    }
    return 1.0f;
}

std::mutex g_icon_mutex;
std::map<std::pair<int, int>, std::shared_ptr<const Mask>> g_icons;

// ---- the controller ----

constexpr float kLeft = -0.47f, kRight = 0.47f, kTop = -0.25f, kBottom = 0.43f;

// Shell of the pad, in units of its width.
float BodySdf(V2 p) {
    float d = Box(p, {0.0f, -0.005f}, 0.295f, 0.14f, 0.11f);
    d = SmoothUnion(d, Capsule(p, {-0.24f, 0.0f}, {-0.335f, 0.215f}, 0.108f), 0.075f);
    d = SmoothUnion(d, Capsule(p, {0.24f, 0.0f}, {0.335f, 0.215f}, 0.108f), 0.075f);
    // The gentle dip along the top edge, between the shoulders.
    d = SmoothCut(d, Circle(p, {0.0f, -0.40f}, 0.25f), 0.06f);
    return d;
}

const V2 kLStick{-0.195f, -0.03f};
const V2 kRStick{0.098f, 0.072f};
const V2 kDpad{-0.098f, 0.072f};
const V2 kFace{0.195f, -0.03f};
constexpr float kFaceGap = 0.05f;
constexpr float kButtonR = 0.026f;

struct Rgb {
    float r, g, b;
};

// Source-over of colour `c` with alpha `a` onto an RGBA float pixel.
void Over(float* dst, Rgb c, float a) {
    a = Clamp01(a);
    dst[0] = dst[0] * (1 - a) + c.r * a;
    dst[1] = dst[1] * (1 - a) + c.g * a;
    dst[2] = dst[2] * (1 - a) + c.b * a;
    dst[3] = dst[3] + a * (1 - dst[3]);
}

// A domed round part lit from the upper left: sticks and buttons.
Rgb Dome(V2 p, V2 c, float r, Rgb base, float gloss) {
    const float dx = (p.x - c.x) / r, dy = (p.y - c.y) / r;
    const float d2 = std::min(1.0f, dx * dx + dy * dy);
    const float nz = std::sqrt(1 - d2);
    const float lambert = std::max(0.0f, -0.45f * dx - 0.55f * dy + 0.70f * nz);
    const float spec = std::pow(std::max(0.0f, -0.35f * dx - 0.5f * dy + 0.79f * nz), 24.0f) * gloss;
    const float k = 0.55f + 0.6f * lambert;
    return {std::min(255.0f, base.r * k + spec * 255), std::min(255.0f, base.g * k + spec * 255),
            std::min(255.0f, base.b * k + spec * 255)};
}

float Cov(float d_units, float px_per_unit) {
    return Clamp01(0.5f - d_units * px_per_unit);
}

// Everything but the shell's colour, per pixel: what sits under the shell (shadow, shoulder
// buttons), the shell's coverage and lighting, what sits on it, and where the accent glows.
struct PadLayers {
    int w = 0, h = 0;
    std::vector<u32> under, over;
    std::vector<u8> cov, shade, spec, glow;
};

void RenderPadRows(PadLayers& L, float width, int y0, int y1) {
    const float px = width; // pixels per unit
    const Rgb graphite{40, 41, 50}, well{16, 16, 21}, cap{50, 51, 61}, dpad{34, 35, 43};
    const Rgb x_col{70, 120, 235}, a_col{232, 72, 72}, b_col{240, 196, 64}, y_col{64, 186, 118};
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < L.w; ++x) {
            const V2 p{kLeft + (x + 0.5f) / px, kTop + (y + 0.5f) / px};
            const std::size_t i = static_cast<std::size_t>(y) * L.w + x;

            // Under the shell: a soft floor shadow and the shoulder buttons.
            float under[4] = {0, 0, 0, 0};
            {
                const float sx = p.x / 0.36f, sy = (p.y - 0.345f) / 0.05f;
                const float a = 0.55f * std::exp(-(sx * sx + sy * sy) * 1.6f);
                Over(under, {0, 0, 0}, a);
                for (int side = -1; side <= 1; side += 2) {
                    const float d = Capsule(p, {side * 0.27f, -0.118f}, {side * 0.13f, -0.152f}, 0.034f);
                    const float c = Cov(d, px);
                    if (c > 0) {
                        const float top = Smooth(0.0f, -0.04f, p.y + 0.15f);
                        const float k = 0.75f + 0.5f * top;
                        Over(under, {graphite.r * k, graphite.g * k, graphite.b * k}, c);
                    }
                }
            }

            // The shell.
            const float d = BodySdf(p);
            const float cov = Cov(d, px);
            float shade = 0, spec = 0;
            if (cov > 0) {
                const float e = 0.5f / px;
                const float nx = BodySdf({p.x + e, p.y}) - BodySdf({p.x - e, p.y});
                const float ny = BodySdf({p.x, p.y + e}) - BodySdf({p.x, p.y - e});
                const float nl = std::max(1e-6f, std::sqrt(nx * nx + ny * ny));
                const float inside = -d;
                const float vy = Clamp01((p.y + 0.16f) / 0.5f);
                shade = 1.16f - 0.44f * vy;
                shade *= 0.74f + 0.26f * Smooth(0.0f, 0.04f, inside);
                // Rim light along edges that face up, and a broad sheen on the upper left.
                spec = 0.55f * Smooth(0.014f, 0.0f, inside) * std::max(0.0f, -ny / nl);
                const float hx = (p.x + 0.13f) / 0.16f, hy = (p.y + 0.08f) / 0.07f;
                spec += 0.16f * std::exp(-(hx * hx + hy * hy));
                // The grips curve away from the light.
                if (p.y > 0.1f) {
                    shade *= 1.0f - 0.18f * Smooth(0.1f, 0.3f, p.y);
                }
            }

            // On the shell: sticks, d-pad, face buttons and the small centre buttons.
            float over[4] = {0, 0, 0, 0};
            float glow = 0;
            auto stick = [&](V2 c) {
                const float wd = Circle(p, c, 0.07f);
                const float wc = Cov(wd, px);
                if (wc <= 0) {
                    return;
                }
                // The well, darker towards its rim.
                const float rim = Smooth(-0.02f, 0.0f, wd);
                Over(over, {well.r + 10 * (1 - rim), well.g + 10 * (1 - rim), well.b + 12 * (1 - rim)}, wc);
                // A thin accent ring around the stick.
                glow = std::max(glow, Cov(std::fabs(Circle(p, c, 0.0615f)) - 0.0035f, px));
                const float cd = Circle(p, c, 0.052f);
                const float cc = Cov(cd, px);
                if (cc > 0) {
                    Rgb col = Dome(p, c, 0.052f, cap, 0.35f);
                    // Concave thumb rest with a lit lower lip.
                    const float dish = Circle(p, c, 0.036f);
                    if (dish < 0) {
                        const float lip = Smooth(-0.01f, 0.0f, dish) * Clamp01((p.y - c.y) / 0.036f);
                        const float k = 0.82f + 0.35f * lip;
                        col = {col.r * k, col.g * k, col.b * k};
                    }
                    Over(over, col, cc);
                    // Grip ring.
                    const float ring = std::fabs(Circle(p, c, 0.044f)) - 0.002f;
                    Over(over, {20, 20, 26}, 0.35f * Cov(ring, px));
                }
            };
            stick(kLStick);
            stick(kRStick);
            {
                const float rd = Circle(p, kDpad, 0.066f);
                const float rc = Cov(rd, px);
                if (rc > 0) {
                    Over(over, {well.r + 8, well.g + 8, well.b + 10}, rc * 0.85f);
                }
                const float arm = 0.05f, half = 0.0185f;
                const float pd = Union(Box(p, kDpad, arm, half, 0.007f), Box(p, kDpad, half, arm, 0.007f));
                const float pc = Cov(pd, px);
                if (pc > 0) {
                    // Lit from the upper left: bevel on the edges facing it.
                    const float e = 0.5f / px;
                    auto sd = [&](V2 q) {
                        return Union(Box(q, kDpad, arm, half, 0.007f), Box(q, kDpad, half, arm, 0.007f));
                    };
                    const float gx = sd({p.x + e, p.y}) - sd({p.x - e, p.y});
                    const float gy = sd({p.x, p.y + e}) - sd({p.x, p.y - e});
                    const float edge = Smooth(0.008f, 0.0f, -pd);
                    const float light = edge * (-(gx * 0.6f + gy * 0.8f)) / std::max(1e-6f, std::sqrt(gx * gx + gy * gy));
                    const float k = 1.0f + 0.55f * light;
                    Rgb col{dpad.r * k, dpad.g * k, dpad.b * k};
                    // A shallow dimple in the middle.
                    const float dim = Circle(p, kDpad, 0.012f);
                    if (dim < 0) {
                        col = {col.r * 0.8f, col.g * 0.8f, col.b * 0.8f};
                    }
                    Over(over, col, pc);
                }
            }
            {
                const struct {
                    V2 off;
                    Rgb col;
                } buttons[4] = {{{0, -kFaceGap}, x_col}, {{kFaceGap, 0}, a_col}, {{0, kFaceGap}, b_col}, {{-kFaceGap, 0}, y_col}};
                for (const auto& b : buttons) {
                    const V2 c{kFace.x + b.off.x, kFace.y + b.off.y};
                    // A dark collar first so each button sits in the shell.
                    const float collar = Cov(Circle(p, c, kButtonR + 0.006f), px);
                    if (collar <= 0) {
                        continue;
                    }
                    Over(over, {14, 14, 18}, collar * 0.7f);
                    const float bc = Cov(Circle(p, c, kButtonR), px);
                    if (bc > 0) {
                        Over(over, Dome(p, c, kButtonR, b.col, 0.8f), bc);
                    }
                }
            }
            {
                // Minus, plus, capture and home.
                const Rgb small{26, 26, 33};
                const float minus = Box(p, {-0.075f, -0.085f}, 0.022f, 0.0065f, 0.006f);
                const float plus = Union(Box(p, {0.075f, -0.085f}, 0.022f, 0.0065f, 0.006f),
                                         Box(p, {0.075f, -0.085f}, 0.0065f, 0.022f, 0.006f));
                const float capture = Box(p, {-0.045f, -0.012f}, 0.016f, 0.016f, 0.005f);
                const float home = Circle(p, {0.045f, -0.012f}, 0.019f);
                const float sd = Union(Union(minus, plus), Union(capture, home));
                const float sc = Cov(sd, px);
                if (sc > 0) {
                    const float lit = Clamp01(0.5f - (p.y + 0.09f) * 4);
                    Over(over, {small.r + 14 * lit, small.g + 14 * lit, small.b + 16 * lit}, sc);
                }
                glow = std::max(glow, Cov(std::fabs(Circle(p, {0.045f, -0.012f}, 0.0115f)) - 0.0028f, px));
                // A light bar under the centre buttons.
                glow = std::max(glow, 0.8f * Cov(Capsule(p, {-0.03f, 0.03f}, {0.03f, 0.03f}, 0.0035f), px));
            }

            auto pack = [](const float* v) {
                const float a = Clamp01(v[3]);
                return static_cast<u32>(std::clamp(v[0], 0.0f, 255.0f)) |
                       (static_cast<u32>(std::clamp(v[1], 0.0f, 255.0f)) << 8) |
                       (static_cast<u32>(std::clamp(v[2], 0.0f, 255.0f)) << 16) |
                       (static_cast<u32>(a * 255.0f + 0.5f) << 24);
            };
            // Over() leaves premultiplied colour; store it straight.
            auto unpremul = [](float* v) {
                if (v[3] > 1e-4f) {
                    v[0] /= v[3];
                    v[1] /= v[3];
                    v[2] /= v[3];
                }
            };
            unpremul(under);
            unpremul(over);
            L.under[i] = pack(under);
            L.over[i] = pack(over);
            L.cov[i] = static_cast<u8>(cov * 255.0f + 0.5f);
            L.shade[i] = static_cast<u8>(std::clamp(shade / 1.4f, 0.0f, 1.0f) * 255.0f + 0.5f);
            L.spec[i] = static_cast<u8>(Clamp01(spec) * 255.0f + 0.5f);
            L.glow[i] = static_cast<u8>(Clamp01(glow) * 255.0f + 0.5f);
        }
    }
}

std::mutex g_pad_mutex;
std::map<int, std::shared_ptr<const PadLayers>> g_pads;

} // namespace

std::shared_ptr<const Gfx::Mask> IconMask(DockIconShape shape, int size, bool may_build) {
    const auto key = std::make_pair(static_cast<int>(shape), size);
    {
        std::lock_guard lock{g_icon_mutex};
        if (auto it = g_icons.find(key); it != g_icons.end()) {
            return it->second;
        }
    }
    if (!may_build) {
        return nullptr;
    }
    auto m = std::make_shared<Mask>();
    m->w = m->h = size;
    m->a.resize(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const V2 p{(x + 0.5f) / size - 0.5f, (y + 0.5f) / size - 0.5f};
            const float c = Cov(IconSdf(shape, p), float(size));
            m->a[static_cast<std::size_t>(y) * size + x] = static_cast<u8>(c * 255.0f + 0.5f);
        }
    }
    std::lock_guard lock{g_icon_mutex};
    return g_icons.emplace(key, std::move(m)).first->second;
}

bool PadReady(int width) {
    std::lock_guard lock{g_pad_mutex};
    return g_pads.count(width) != 0;
}

void BuildPad(int width, const std::function<void(int, const std::function<void(int)>&)>& parallel) {
    if (PadReady(width)) {
        return;
    }
    auto L = std::make_shared<PadLayers>();
    L->w = static_cast<int>(std::ceil((kRight - kLeft) * width));
    L->h = static_cast<int>(std::ceil((kBottom - kTop) * width));
    const std::size_t n = static_cast<std::size_t>(L->w) * L->h;
    L->under.assign(n, 0);
    L->over.assign(n, 0);
    L->cov.assign(n, 0);
    L->shade.assign(n, 0);
    L->spec.assign(n, 0);
    L->glow.assign(n, 0);
    constexpr int kChunks = 12;
    PadLayers* raw = L.get();
    const int h = L->h;
    parallel(kChunks, [raw, width, h](int i) {
        const int y0 = h * i / kChunks, y1 = h * (i + 1) / kChunks;
        RenderPadRows(*raw, float(width), y0, y1);
    });
    std::lock_guard lock{g_pad_mutex};
    if (g_pads.size() > 4) {
        g_pads.clear();
    }
    g_pads.emplace(width, std::move(L));
}

void DrawPad(Canvas& c, float cx, float cy, int width, u32 accent) {
    std::shared_ptr<const PadLayers> L;
    {
        std::lock_guard lock{g_pad_mutex};
        if (auto it = g_pads.find(width); it != g_pads.end()) {
            L = it->second;
        }
    }
    if (!L || c.Invisible()) {
        return;
    }
    const int x0 = static_cast<int>(std::lround(cx + kLeft * width));
    const int y0 = static_cast<int>(std::lround(cy + kTop * width));
    // The shell takes the system's colour, eased away from neon so it looks like plastic.
    const float ar = float(accent & 0xFF), ag = float((accent >> 8) & 0xFF), ab = float((accent >> 16) & 0xFF);
    const float grey = (ar + ag + ab) / 3.0f;
    const float sr = grey + (ar - grey) * 0.85f, sg = grey + (ag - grey) * 0.85f, sb = grey + (ab - grey) * 0.85f;
    const u32 glow_col = Canvas::Mix(accent, MakeColor(0xFF, 0xFF, 0xFF), 0.35f);
    const int gr = static_cast<int>(glow_col & 0xFF), gg = static_cast<int>((glow_col >> 8) & 0xFF),
              gb = static_cast<int>((glow_col >> 16) & 0xFF);
    // Shell colour at full shade, 16.16 fixed point, so each pixel is a few multiplies.
    const int k_r = static_cast<int>(sr * 1.4f * 256.0f / 255.0f * 256.0f);
    const int k_g = static_cast<int>(sg * 1.4f * 256.0f / 255.0f * 256.0f);
    const int k_b = static_cast<int>(sb * 1.4f * 256.0f / 255.0f * 256.0f);
    for (int y = 0; y < L->h; ++y) {
        if (!c.RowsVisible(y0 + y, 1)) {
            continue;
        }
        const std::size_t base = static_cast<std::size_t>(y) * L->w;
        for (int x = 0; x < L->w; ++x) {
            const std::size_t i = base + x;
            const u32 under = L->under[i], over = L->over[i];
            const int cov = L->cov[i], glow = L->glow[i];
            if (!(under >> 24) && !cov && !(over >> 24) && !glow) {
                continue;
            }
            // Premultiplied layers, bottom to top, all in 0..255.
            auto lay = [](int& r, int& g, int& b, int& a, int cr, int cg, int cb, int ca) {
                const int inv = 255 - ca;
                r = (cr * ca + r * inv) / 255;
                g = (cg * ca + g * inv) / 255;
                b = (cb * ca + b * inv) / 255;
                a = ca + a * inv / 255;
            };
            int r = 0, g = 0, b = 0, a = 0;
            if (under >> 24) {
                lay(r, g, b, a, static_cast<int>(under & 0xFF), static_cast<int>((under >> 8) & 0xFF),
                    static_cast<int>((under >> 16) & 0xFF), static_cast<int>(under >> 24));
            }
            if (cov) {
                const int shade = L->shade[i], spec = L->spec[i];
                const int br = std::min(255, ((k_r * shade) >> 16) + spec);
                const int bg = std::min(255, ((k_g * shade) >> 16) + spec);
                const int bb = std::min(255, ((k_b * shade) >> 16) + spec);
                lay(r, g, b, a, br, bg, bb, cov);
            }
            if (over >> 24) {
                lay(r, g, b, a, static_cast<int>(over & 0xFF), static_cast<int>((over >> 8) & 0xFF),
                    static_cast<int>((over >> 16) & 0xFF), static_cast<int>(over >> 24));
            }
            if (glow) {
                lay(r, g, b, a, gr, gg, gb, glow);
            }
            // `lay` blends straight colours into a premultiplied-from-zero accumulator, so
            // (r, g, b) are premultiplied by `a` already.
            c.BlendPremul(x0 + x, y0 + y,
                          static_cast<u32>(std::min(r, a)) | (static_cast<u32>(std::min(g, a)) << 8) |
                              (static_cast<u32>(std::min(b, a)) << 16) | (static_cast<u32>(a) << 24));
        }
    }
}

} // namespace SwitchFrontend::Skin::Shapes
