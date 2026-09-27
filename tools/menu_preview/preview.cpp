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
#include "citra_switch/rail_icons.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace SwitchFrontend;
using namespace SwitchFrontend::Gfx;
using namespace SwitchFrontend::Skin::Layout;

namespace {

// A stand-in for a 3DS SMDH icon: a colourful 48x48 gradient with a simple shape.
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
            const bool blob = dx * dx + dy * dy < 150.0f;
            const float k = blob ? 1.0f : 0.72f;
            px[y * 48 + x] = MakeColor(u8(255 * r * k), u8(255 * g * k), u8(255 * b * k));
        }
    return px;
}

struct FakeGame {
    std::string title, subtitle, system;
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

    const std::vector<Skin::RailEntry> rail = {
        {"Library", RailIcons::kLibrary}, {"Systems", RailIcons::kSystems}, {"Install", RailIcons::kInstall},
        {"Settings", RailIcons::kSettings}, {"Paths", RailIcons::kPaths}, {"Artic", nullptr},
    };

    const std::vector<FakeGame> games = {
        {"Animal Crossing: New Leaf", "Nintendo", "3ds", true},
        {"Castlevania: Aria of Sorrow", "Game Boy Advance", "gba", false},
        {"Final Fantasy X", "PlayStation 2", "ps2", false},
        {"Fire Emblem Awakening", "Nintendo", "3ds", true},
        {"Kirby Super Star", "Super Nintendo", "snes", false},
        {"Mario Kart 7", "Nintendo", "3ds", true},
        {"Metroid Prime Hunters", "Nintendo DS", "ds", false},
        {"Pokemon Crystal", "Game Boy", "gb", false},
        {"Super Mario 64", "Nintendo 64", "n64", false},
        {"The Wind Waker HD", "Wii U", "wiiu", false},
        {"Tekken 3", "PlayStation", "ps1", false},
        {"Sonic the Hedgehog 2", "Mega Drive / Genesis", "md", false},
    };
    std::vector<std::vector<u32>> icons;
    for (std::size_t i = 0; i < games.size(); ++i) icons.push_back(FakeIcon(int(i) + 3));

    auto frame = [&](Canvas& c, int pill, int ghost, int selected, bool content_focus, bool empty, const char* toast) {
        Skin::DrawBackdrop(c);
        Skin::DrawHintBar(c);
        Skin::DrawRail(c, f, rail, pill, ghost);
        Skin::DrawHeader(c, f, "Library", empty ? "0 games" : std::to_string(games.size()) + " games");
        if (empty) {
            Skin::DrawEmptyLibrary(c, f, "sdmc:/switch/dekopon/roms/");
        } else {
            const Skin::Grid grid = Skin::ComputeGrid(c.Width(), c.Height());
            for (int i = 0; i < int(games.size()); ++i) {
                const int row = i / grid.cols, col = i % grid.cols;
                if (row >= grid.visible_rows) break;
                Skin::TileInfo t;
                t.title = games[i].title;
                t.subtitle = games[i].subtitle;
                t.system_id = games[i].system;
                t.art_key = games[i].title;
                if (games[i].icon) {
                    t.icon = &icons[i];
                    t.icon_size = 48;
                }
                if (i == 3) t.tags = {"SD"};
                Skin::DrawTile(c, f, t, grid.start_x + col * (kTileW + kTileGap), grid.top + row * (kTileH + kTileGap),
                               i == selected, content_focus);
            }
            const int track_h = grid.visible_rows * (kTileH + kTileGap) - kTileGap;
            Skin::DrawScrollbar(c, c.Width() - 12, grid.top, track_h, grid.top, track_h * 2 / 3);
        }
        int hx = kContentX + 32;
        const int hy = c.Height() - kHintH + (kHintH - 28) / 2;
        if (content_focus) {
            hx += Skin::DrawHint(c, f, hx, hy, "A", "Play") + 26;
            hx += Skin::DrawHint(c, f, hx, hy, "X", "Search") + 26;
            hx += Skin::DrawHint(c, f, hx, hy, "Y", "Refresh") + 26;
            hx += Skin::DrawHint(c, f, hx, hy, "+", "Details") + 26;
        } else {
            hx += Skin::DrawHint(c, f, hx, hy, "A", "Open") + 26;
            hx += Skin::DrawHint(c, f, hx, hy, "B", "Back") + 26;
        }
        Skin::DrawHint(c, f, hx, hy, "+ -", "Exit");
        if (toast) Skin::DrawToast(c, f, toast, false);
    };

    Canvas c;
    frame(c, 0, -1, 1, true, false, nullptr);
    SavePng(c, out + "/library.png");
    frame(c, 1, 0, 1, false, false, nullptr);
    SavePng(c, out + "/library_rail.png");
    frame(c, 0, -1, 0, true, true, "Found 0 games");
    SavePng(c, out + "/empty.png");

    // Every system's wordmark, to review them side by side.
    Skin::DrawBackdrop(c);
    const char* ids[] = {"3ds", "ds", "gba", "gb", "nes", "snes", "n64", "vb", "ps1", "ps2", "psp",
                         "md", "sms", "gg", "dc", "arcade", "pce", "ngp", "ws", "a2600", "lynx", "wiiu"};
    for (int i = 0; i < 22; ++i) {
        const int col = i % 6, row = i / 6;
        Skin::DrawSystemMark(c, f, ids[i], 40 + col * 205, 40 + row * 170, 180, 140, 22);
        const int w = regular.Measure(ids[i], 15);
        regular.Draw(c, 40 + col * 205 + 90 - w / 2, 40 + row * 170 + 160, ids[i], 15, Skin::Palette::kColTextDim);
    }
    SavePng(c, out + "/marks.png");
    return 0;
}
