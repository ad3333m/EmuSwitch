// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders EmuSwitch's menu skin to PNGs on a desktop, so the look can be checked
// without a Switch. Built by .github/workflows/menu-preview.yml:
//   preview <fonts dir> <out dir>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "citra_switch/menu_gfx.h"
#include "citra_switch/menu_skin.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

struct FakeGame {
    std::string title, badge;
    u32 color;
    bool icon;
};

void SavePng(Canvas& c, const std::string& path) {
    std::vector<u32> px(c.Data(), c.Data() + std::size_t(c.Width()) * c.Height());
    for (u32& p : px) p |= 0xFF000000u;
    stbi_write_png(path.c_str(), c.Width(), c.Height(), 4, px.data(), c.Width() * 4);
    std::printf("wrote %s\n", path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::string fonts = argc > 1 ? argv[1] : "fonts";
    const std::string out = argc > 2 ? argv[2] : ".";
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
        {"Animal Crossing: New Leaf", "3DS", red, true},
        {"Castlevania: Aria of Sorrow", "GBA", MakeColor(0x8B, 0x5C, 0xF6), false},
        {"Final Fantasy X", "PS2", MakeColor(0x38, 0xBD, 0xF8), false},
        {"Fire Emblem Awakening", "3DS", red, true},
        {"Kirby Super Star", "SNES", MakeColor(0x7E, 0x6C, 0xD8), false},
        {"Mario Kart 7", "3DS", red, true},
        {"Metroid Prime Hunters", "DS", MakeColor(0x3D, 0x7B, 0xFF), false},
        {"Pokemon Crystal", "GB", MakeColor(0x22, 0xC5, 0x5E), false},
        {"Super Mario 64", "N64", MakeColor(0x10, 0x9A, 0x4E), false},
        {"The Wind Waker HD", "Wii U", MakeColor(0x2D, 0xD4, 0xBF), false},
        {"Tekken 3", "PS1", MakeColor(0x9C, 0xA3, 0xB5), false},
    };
    std::vector<std::vector<u32>> icons;
    for (std::size_t i = 0; i < games.size(); ++i) icons.push_back(FakeIcon(int(i) + 3));

    auto hints = [&](Canvas& c, std::vector<std::pair<const char*, const char*>> left,
                     std::vector<std::pair<const char*, const char*>> right) {
        const int y = c.Height() - 44;
        int x = 40;
        for (auto& [b, l] : left) x += Skin::DrawHint(c, f, x, y, b, l) + 24;
        // Right-aligned: measure by drawing off-screen first.
        int total = 0;
        for (auto& [b, l] : right) total += Skin::DrawHint(c, f, -2000, -2000, b, l) + 24;
        x = c.Width() - 40 - total + 24;
        for (auto& [b, l] : right) x += Skin::DrawHint(c, f, x, y, b, l) + 24;
    };

    auto home = [&](Canvas& c, int selected, bool dock_focus, float t) {
        Skin::DrawBackdrop(c);
        Skin::TopBar bar{"gd_adv", "", "03:02", "PM", 76, true};
        Skin::DrawTopBar(c, f, bar);
        const Skin::Grid grid = Skin::ComputeGrid(c.Width(), c.Height());
        for (int i = 0; i < grid.cols * grid.visible_rows; ++i) {
            const int x = grid.start_x + (i % grid.cols) * (kTileW + kTileGap);
            const int y = grid.top + (i / grid.cols) * (kTileH + kTileGap);
            if (i >= int(games.size())) {
                Skin::DrawEmptySlot(c, x, y);
                continue;
            }
            if (i == selected && !dock_focus) continue;
            Skin::TileInfo ti;
            ti.title = games[i].title;
            ti.system_badge = games[i].badge;
            ti.system_color = games[i].color;
            if (games[i].icon) { ti.icon = &icons[i]; ti.icon_size = 48; }
            Skin::DrawTile(c, f, ti, x, y, i == selected, !dock_focus, t);
        }
        if (!dock_focus) {
            Skin::TileInfo ti;
            ti.title = games[selected].title;
            ti.system_badge = games[selected].badge;
            ti.system_color = games[selected].color;
            if (games[selected].icon) { ti.icon = &icons[selected]; ti.icon_size = 48; }
            Skin::DrawTile(c, f, ti, grid.start_x + (selected % grid.cols) * (kTileW + kTileGap),
                           grid.top + (selected / grid.cols) * (kTileH + kTileGap), true, true, t);
        }
        const int pill_y = grid.top + grid.visible_rows * (kTileH + kTileGap) - kTileGap + 12;
        Skin::DrawTitlePill(c, f, pill_y, games[selected].title, "Nintendo 3DS", games[selected].color);
        Skin::DrawDock(c, f, dock, 0, dock_focus ? 1 : 0, dock_focus);
        hints(c, {{"-", "Rescan"}, {"L", "Picture"}}, {{"+", "Details"}, {"A", "Play"}});
    };

    Canvas c;
    home(c, 5, false, 0.6f);
    SavePng(c, out + "/home.png");
    home(c, 5, true, 0.0f);
    SavePng(c, out + "/home_dock.png");

    // Systems carousel.
    Skin::DrawBackdrop(c);
    Skin::TopBar bar{"Systems", "Pick a system to see its games", "03:02", "PM", 76, true};
    Skin::DrawTopBar(c, f, bar);
    const std::vector<Skin::SystemCard> cards = {
        {"3ds", "3DS", red}, {"ds", "DS", MakeColor(0x3D, 0x7B, 0xFF)}, {"gba", "GBA", MakeColor(0x8B, 0x5C, 0xF6)},
        {"gb", "GB", MakeColor(0x22, 0xC5, 0x5E)}, {"nes", "NES", MakeColor(0xE1, 0x3B, 0x3B)},
    };
    Skin::DrawSystemsCarousel(c, f, cards, 2.0f, 2, "Game Boy Advance", "12 games", "Ready", true);
    Skin::DrawDock(c, f, dock, 1, 1, false);
    hints(c, {{"Y", "Picture"}, {"X", "Default"}}, {{"A", "Games"}});
    SavePng(c, out + "/systems.png");

    // Empty library.
    Skin::DrawBackdrop(c);
    Skin::TopBar bar2{"gd_adv", "", "03:02", "PM", 12, false};
    Skin::DrawTopBar(c, f, bar2);
    Skin::DrawEmptyLibrary(c, f, "sdmc:/switch/dekopon/roms/");
    Skin::DrawDock(c, f, dock, 0, 0, false);
    Skin::DrawToast(c, f, "Found 0 games", false);
    hints(c, {{"-", "Rescan"}}, {});
    SavePng(c, out + "/empty.png");
    return 0;
}
