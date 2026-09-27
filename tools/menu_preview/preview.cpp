// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders EmuSwitch's menu skin to PNGs on a desktop, so the look can be checked
// without a Switch, and times a frame the way the app draws it (a warm-up pass, then
// the bands in parallel). Built by .github/workflows/menu-preview.yml:
//   preview <fonts dir> <out dir> [box art dir]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "citra_switch/menu_gfx.h"
#include "citra_switch/menu_skin.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace SwitchFrontend;
using namespace SwitchFrontend::Gfx;
using namespace SwitchFrontend::Skin::Layout;

namespace {

// Stand-in for a 3DS SMDH icon: a colourful 48x48 gradient with a blob.
std::vector<u32> FakeIcon(int seed) {
    std::vector<u32> px(48 * 48);
    const float hue = seed * 0.61f;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x) {
            const float t = (x + y) / 94.0f;
            const float r = 0.5f + 0.5f * std::sin(hue + t * 2.0f);
            const float g = 0.5f + 0.5f * std::sin(hue + 2.1f + t * 2.0f);
            const float b = 0.5f + 0.5f * std::sin(hue + 4.2f + t * 2.0f);
            const float dx = x - 24.0f, dy = y - 26.0f;
            const float k = dx * dx + dy * dy < 150.0f ? 1.0f : 0.72f;
            px[y * 48 + x] = MakeColor(u8(255 * r * k), u8(255 * g * k), u8(255 * b * k));
        }
    return px;
}

// A fake avatar: warm gradient with a lighter circle.
std::vector<u32> FakeAvatar() {
    std::vector<u32> px(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float dx = x - 32.0f, dy = y - 26.0f;
            const bool head = dx * dx + dy * dy < 150.0f || (y > 44 && std::fabs(dx) < 22.0f);
            px[y * 64 + x] = head ? MakeColor(0xF4, 0xD9, 0xC4) : MakeColor(u8(40 + x), u8(90 + y), 200);
        }
    return px;
}

Image LoadPng(const std::string& path) {
    Image img;
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) {
        return img;
    }
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * h);
    std::memcpy(img.px.data(), data, img.px.size() * 4);
    stbi_image_free(data);
    img.opaque = true;
    for (u32 p : img.px) {
        if ((p >> 24) != 0xFF) {
            img.opaque = false;
            break;
        }
    }
    // The app keeps pictures at most 320 pixels on a side.
    const float k = std::min(1.0f, 320.0f / std::max(w, h));
    if (k < 1.0f) {
        img = Resize(img, std::max(1, int(w * k + 0.5f)), std::max(1, int(h * k + 0.5f)));
    }
    return img;
}

struct FakeGame {
    std::string title, badge;
    u32 color;
    bool icon;
    std::string art; // box art file in the art dir, if any
};

void SavePng(Canvas& c, const std::string& path) {
    std::vector<u32> px(c.Data(), c.Data() + std::size_t(c.Width()) * c.Height());
    for (u32& p : px) p |= 0xFF000000u;
    stbi_write_png(path.c_str(), c.Width(), c.Height(), 4, px.data(), c.Width() * 4);
    std::printf("wrote %s\n", path.c_str());
}

// One frame, drawn the way the app draws it.
template <typename Scene>
void Frame(Canvas& canvas, double now, Scene&& scene, bool parallel = true) {
    Skin::BeginFrame(now, canvas.Width(), canvas.Height());
    Canvas warm = canvas.View(0, 0);
    scene(warm);
    Skin::EndWarmup();
    const std::function<void(int)> band = [&](int i) {
        int y0, y1;
        BandRows(canvas.Height(), i, y0, y1);
        Canvas v = canvas.View(y0, y1);
        scene(v);
    };
    if (parallel) {
        Workers::Get().Run(kBands, band);
    } else {
        for (int i = 0; i < kBands; ++i) band(i);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string fonts = argc > 1 ? argv[1] : "fonts";
    const std::string out = argc > 2 ? argv[2] : ".";
    const std::string art = argc > 3 ? argv[3] : "";
    Font regular, bold, mark;
    regular.Init((fonts + "/Inter-Medium.ttf").c_str());
    bold.Init((fonts + "/Inter-Bold.ttf").c_str());
    mark.Init((fonts + "/Inter-BlackItalic.ttf").c_str());
    const Skin::Fonts f{&regular, &bold, &mark};
    Skin::SetAvatar(FakeAvatar(), 64, 64);

    const std::vector<Skin::DockItem> dock = {
        {"Home", Skin::DockIcon::Home},         {"Systems", Skin::DockIcon::Systems},
        {"Install", Skin::DockIcon::Install},   {"Settings", Skin::DockIcon::Settings},
        {"Paths", Skin::DockIcon::Folder},      {"Artic", Skin::DockIcon::Text, "AB"},
    };
    const u32 red = MakeColor(0xE2, 0x1B, 0x33);
    const std::vector<FakeGame> games = {
        {"Animal Crossing: New Leaf", "3DS", red, true, ""},
        {"Castlevania: Aria of Sorrow", "GBA", MakeColor(0x8B, 0x5C, 0xF6), false, ""},
        {"Final Fantasy X", "PS2", MakeColor(0x38, 0xBD, 0xF8), false, ""},
        {"Fire Emblem Awakening", "3DS", red, true, ""},
        {"Super Mario World", "SNES", MakeColor(0x7E, 0x6C, 0xD8), false, "Nintendo_-_Super_Nintendo_Entertainment_System.png"},
        {"Mario Kart 7", "3DS", red, true, "Nintendo_-_Nintendo_3DS.png"},
        {"Metroid Prime Hunters", "DS", MakeColor(0x3D, 0x7B, 0xFF), false, ""},
        {"Pokemon Emerald", "GBA", MakeColor(0x8B, 0x5C, 0xF6), false, "Nintendo_-_Game_Boy_Advance.png"},
        {"Super Mario 64", "N64", MakeColor(0x10, 0x9A, 0x4E), false, "Nintendo_-_Nintendo_64.png"},
        {"The Wind Waker HD", "Wii U", MakeColor(0x2D, 0xD4, 0xBF), false, ""},
        {"Tekken 3", "PS1", MakeColor(0x9C, 0xA3, 0xB5), false, ""},
    };
    std::vector<std::vector<u32>> icons;
    for (std::size_t i = 0; i < games.size(); ++i) icons.push_back(FakeIcon(int(i) + 3));
    if (!art.empty()) {
        for (const FakeGame& g : games) {
            if (!g.art.empty()) {
                Image img = LoadPng(art + "/" + g.art);
                if (!img.Empty()) Skin::SetGameImage(g.title, std::move(img));
            }
        }
    }

    auto hints = [&](Canvas& c, std::vector<std::pair<const char*, const char*>> left,
                     std::vector<std::pair<const char*, const char*>> right) {
        const int y = c.Height() - 44;
        int x = 40;
        for (auto& [b, l] : left) x += Skin::DrawHint(c, f, x, y, b, l) + 24;
        int total = 0;
        for (auto& [b, l] : right) total += Skin::DrawHint(c, f, -2000, -2000, b, l) + 24;
        x = c.Width() - 40 - total + 24;
        for (auto& [b, l] : right) x += Skin::DrawHint(c, f, x, y, b, l) + 24;
    };

    auto tile_info = [&](int i) {
        Skin::TileInfo ti;
        ti.title = games[i].title;
        ti.system_badge = games[i].badge;
        ti.system_color = games[i].color;
        ti.art_key = games[i].title;
        if (games[i].icon) {
            ti.icon = &icons[i];
            ti.icon_size = 48;
        }
        return ti;
    };

    // `lift` and `appear` let a frame sit mid-animation.
    auto home = [&](Canvas& c, int selected, bool dock_focus, float t, float lift, float appear) {
        Skin::DrawBackdrop(c);
        Skin::TopBar bar{dock_focus ? "gd_adv" : games[selected].title, dock_focus ? "" : "Nintendo 3DS", "03:02", "PM", 76, true};
        Skin::DrawTopBar(c, f, bar);
        const Skin::Grid grid = Skin::ComputeGrid(c.Width(), c.Height());
        for (int i = 0; i < grid.cols * grid.visible_rows; ++i) {
            const int x = grid.start_x + (i % grid.cols) * (kTileW + kTileGap);
            const int y = grid.top + (i / grid.cols) * (kTileH + kTileGap);
            const float stagger = std::clamp(appear * 3.0f - (i % grid.cols) * 0.12f - (i / grid.cols) * 0.2f, 0.0f, 1.0f);
            if (i >= int(games.size())) {
                Skin::DrawEmptySlot(c, x, y, stagger);
                continue;
            }
            if (i == selected && !dock_focus) continue;
            Skin::DrawTile(c, f, tile_info(i), x, y, {0.0f, stagger, t, dock_focus});
        }
        if (!dock_focus) {
            const int x = grid.start_x + (selected % grid.cols) * (kTileW + kTileGap);
            const int y = grid.top + (selected / grid.cols) * (kTileH + kTileGap);
            Skin::DrawTile(c, f, tile_info(selected), x, y, {lift, 1.0f, t, false});
            const float s = kTileW + (kTileFocus - kTileW) * lift;
            Skin::DrawFocusRing(c, x + kTileW / 2.0f - s / 2, y + kTileH / 2.0f - s / 2 - 3 * lift, s, t, 1.0f);
        }
        Skin::DrawDock(c, f, dock, {0, dock_focus ? 1 : 0, dock_focus, dock_focus ? 1.0f : 0.0f, dock_focus ? 1.0f : 0.0f, t});
        hints(c, {{"A", "Play"}, {"X", "Search"}, {"L", "Picture"}}, {{"+", "Details"}, {"+ -", "Exit"}});
    };

    Canvas c;
    Frame(c, 0.0, [&](Canvas& v) { home(v, 5, false, 1.3f, 1.0f, 1.0f); });
    // A few frames so every picture's exact copy is built, then the real shot.
    for (int i = 0; i < 12; ++i) Frame(c, 0.1 * i, [&](Canvas& v) { home(v, 5, false, 1.3f, 1.0f, 1.0f); });
    Skin::SetAmbient(Skin::TileAccent(tile_info(5)));
    for (int i = 0; i < 90; ++i) Frame(c, 1.0 + i / 30.0, [&](Canvas& v) { home(v, 5, false, 1.3f, 1.0f, 1.0f); });
    SavePng(c, out + "/home.png");
    Frame(c, 5.0, [&](Canvas& v) { home(v, 7, false, 1.5f, 1.0f, 1.0f); });
    for (int i = 0; i < 8; ++i) Frame(c, 5.0, [&](Canvas& v) { home(v, 7, false, 1.5f, 1.0f, 1.0f); });
    SavePng(c, out + "/home_box.png");
    Frame(c, 5.2, [&](Canvas& v) { home(v, 5, false, 0.2f, 0.45f, 0.55f); });
    SavePng(c, out + "/home_intro.png");
    Frame(c, 6.0, [&](Canvas& v) { home(v, 5, true, 0.0f, 0.0f, 1.0f); });
    SavePng(c, out + "/home_dock.png");

    // Systems carousel.
    const std::vector<Skin::SystemCard> cards = {
        {"3ds", "3DS", red}, {"ds", "DS", MakeColor(0x3D, 0x7B, 0xFF)}, {"gba", "GBA", MakeColor(0x8B, 0x5C, 0xF6)},
        {"gb", "GB", MakeColor(0x22, 0xC5, 0x5E)}, {"nes", "NES", MakeColor(0xE1, 0x3B, 0x3B)},
    };
    auto systems = [&](Canvas& v, float anim, int sel, float t) {
        Skin::DrawBackdrop(v);
        Skin::TopBar bar{"Systems", "Pick a system to see its games", "03:02", "PM", 76, true};
        Skin::DrawTopBar(v, f, bar);
        Skin::CarouselText text{"Game Boy Advance", "12 games", "Ready", true, false};
        Skin::DrawSystemsCarousel(v, f, cards, anim, sel, text, cards[sel].color, t);
        Skin::DrawDock(v, f, dock, {1, 1, false, 1.0f, 0.0f, t});
        hints(v, {{"A", "Games"}, {"Y", "Picture"}}, {{"B", "Back"}});
    };
    Skin::SetAmbient(cards[2].color);
    Skin::SetSpot(225.0f, 330.0f + 90.0f, 230.0f, cards[2].color, 1.0f);
    for (int i = 0; i < 60; ++i) Frame(c, 7.0 + i / 30.0, [&](Canvas& v) { systems(v, 2.0f, 2, 0.4f); });
    SavePng(c, out + "/systems.png");
    Frame(c, 9.0, [&](Canvas& v) { systems(v, 2.45f, 2, 0.4f); });
    SavePng(c, out + "/systems_moving.png");

    Skin::SetSpot(225.0f, 420.0f, 230.0f, cards[2].color, 0.0f);
    // Empty library.
    for (int i = 0; i < 4; ++i) Frame(c, 10.0, [&](Canvas& v) {
        Skin::DrawBackdrop(v);
        Skin::TopBar bar2{"gd_adv", "", "03:02", "PM", 12, false};
        Skin::DrawTopBar(v, f, bar2);
        Skin::DrawEmptyLibrary(v, f, "sdmc:/switch/dekopon/roms/", 0.0f);
        Skin::DrawDock(v, f, dock, {0, 0, false, 0.0f, 0.0f, 0.0f});
        Skin::DrawToast(v, f, "Found 0 games", false, 1.0f);
        hints(v, {{"Y", "Refresh"}}, {});
    });
    SavePng(c, out + "/empty.png");

    // A modal over the library: rows, tabs and a progress bar.
    Frame(c, 11.0, [&](Canvas& v) {
        home(v, 5, false, 1.3f, 1.0f, 1.0f);
        const int w = 620, h = 330, x = (v.Width() - w) / 2, y = (v.Height() - h) / 2;
        Skin::DrawModal(v, x, y, w, h);
        bold.Draw(v, x + 24, y + 44, "Reset All Settings", 24, Skin::Palette::kColText);
        regular.Draw(v, x + 24, y + 68, "Choose the settings to reset to", 16, Skin::Palette::kColTextDim);
        const char* rows[] = {"Balanced", "Performance", "Quality"};
        for (int i = 0; i < 3; ++i) {
            const int ry = y + 86 + i * 54;
            Skin::DrawRow(v, x + 16, ry, w - 32, 50, i == 1);
            regular.Draw(v, x + 36, ry + 32, rows[i], 20, Skin::Palette::kColText);
        }
        Skin::DrawPill(v, x + 24, y + 262, 120, 30, true);
        bold.Draw(v, x + 44, y + 283, "General", 16, Skin::Palette::kColOnAccent);
        Skin::DrawPill(v, x + 152, y + 262, 120, 30, false);
        Skin::DrawProgress(v, x + 300, y + 272, 290, 10, 0.62f, 0.3f);
        Skin::DrawSpinner(v, x + w - 40.0f, y + 40.0f, 12.0f, 0.4f);
    });
    SavePng(c, out + "/modal.png");

    // Timing: the home screen, single-threaded bands versus the pool.
    auto bench = [&](bool parallel) {
        const int n = 120;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            Frame(c, 20.0 + i / 60.0, [&](Canvas& v) { home(v, 5, false, 20.0f + i / 60.0f, 1.0f, 1.0f); }, parallel);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("home frame %s: %.2f ms\n", parallel ? "parallel" : "one thread", ms);
    };
    bench(false);
    bench(true);
    auto bench_sys = [&](bool parallel) {
        const int n = 120;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            Frame(c, 30.0 + i / 60.0, [&](Canvas& v) { systems(v, 2.0f + 0.5f * std::sin(i * 0.1f), 2, i / 60.0f); }, parallel);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("systems frame %s: %.2f ms\n", parallel ? "parallel" : "one thread", ms);
    };
    bench_sys(false);
    bench_sys(true);
    Workers::Get().Shutdown();
    return 0;
}
