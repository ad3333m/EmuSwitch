// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders EmuSwitch's menu skin to PNGs on a desktop, so the look can be checked
// without a Switch, and times a frame the way the app draws it (a warm-up pass, then
// the bands in parallel). Built by .github/workflows/menu-preview.yml:
//   preview <fonts dir> <out dir> [box art dir]

#include <algorithm>
#include <cctype>
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

// Stand-in for a SteamGridDB picture: a poster in two colours with a title on it.
Image FakeGrid(Font& bold, Font& regular, int w, int h, u32 top, u32 bottom) {
    Canvas c;
    c.Resize(w, h);
    for (int y = 0; y < h; ++y) {
        c.FillRect(0, y, w, 1, Canvas::Mix(top, bottom, float(y) / float(h - 1)));
    }
    const int big = w / 3;
    bold.Draw(c, (w - bold.Measure("MK7", big)) / 2, h / 2 + big / 4, "MK7", big, MakeColor(0xFF, 0xFF, 0xFF));
    const int small = std::max(12, w / 14);
    regular.Draw(c, (w - regular.Measure("MARIO KART 7", small)) / 2, h / 2 + big / 4 + small * 2, "MARIO KART 7",
                 small, MakeColor(0xFF, 0xFF, 0xFF, 0xD0));
    Image img;
    img.w = w;
    img.h = h;
    img.px.assign(c.Data(), c.Data() + std::size_t(w) * h);
    for (u32& p : img.px) p |= 0xFF000000u;
    img.opaque = true;
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
        {"Home", Skin::DockIcon::Home},
        {"Systems", Skin::DockIcon::Systems},
        {"Settings", Skin::DockIcon::Settings},
        {"Paths", Skin::DockIcon::Folder},
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

    // Home, the way the app lays it out: a section per console (in the consoles' order), each a
    // header and rows of tiles. `lift` and `appear` let a frame sit mid-animation.
    struct Section {
        std::string id, badge, name;
        u32 color;
        std::vector<int> games;
    };
    std::vector<Section> sections;
    for (std::size_t i = 0; i < games.size(); ++i) {
        const std::string& b = games[i].badge;
        auto it = std::find_if(sections.begin(), sections.end(), [&](const Section& s) { return s.badge == b; });
        if (it == sections.end()) {
            std::string id = b, name = b;
            for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (b == "3DS") name = "Nintendo 3DS";
            if (b == "GBA") name = "Game Boy Advance";
            if (b == "PS1") name = "PlayStation";
            if (b == "PS2") name = "PlayStation 2";
            if (b == "SNES") name = "Super Nintendo";
            if (b == "N64") name = "Nintendo 64";
            if (b == "DS") name = "Nintendo DS";
            if (b == "Wii U") id = "wiiu";
            sections.push_back({id, b, name, games[i].color, {}});
            it = sections.end() - 1;
        }
        it->games.push_back(static_cast<int>(i));
    }
    auto home = [&](Canvas& c, int selected, bool dock_focus, float t, float lift, float appear, float scroll = 0.0f,
                    bool moving = false) {
        Skin::DrawBackdrop(c);
        // Kept in a named string: the bar only views its text.
        const std::string title = dock_focus ? std::string{"gd_adv"} : games[selected].title;
        Skin::TopBar bar{title, dock_focus ? "" : moving ? "Moving: use the arrows, then A" : "Nintendo 3DS", "03:02", "PM",
                         76, true};
        Skin::DrawTopBar(c, f, bar);
        const Skin::Grid grid = Skin::ComputeGrid(c.Width(), c.Height());
        const int used_w = grid.cols * kTileW + (grid.cols - 1) * kTileGap;
        const int view_top = kContentTop + 4;
        Skin::DrawDock(c, f, dock, {0, dock_focus ? 1 : 0, dock_focus, dock_focus ? 1.0f : 0.0f, dock_focus ? 1.0f : 0.0f, t});
        if (moving) {
            hints(c, {{"D-Pad", "Move"}, {"A", "Done"}, {"B", "Cancel"}}, {});
        } else {
            hints(c, {{"A", "Play"}, {"X", "Search"}, {"L", "Picture"}}, {{"+", "Menu"}, {"+ -", "Exit"}});
        }
        Canvas::ClipScope clip{c, 0, kContentTop - 20, c.Width(), c.Height() - kHintH - 16 - (kContentTop - 20)};
        float y = 0.0f;
        int sel_x = 0, sel_y = 0;
        bool room_left = false, room_right = false;
        for (const Section& sec : sections) {
            const int hy = static_cast<int>(view_top + y - scroll);
            Skin::DrawSectionHeader(c, f, {sec.id, sec.badge, sec.color}, sec.name,
                                    std::to_string(sec.games.size()) + (sec.games.size() == 1 ? " game" : " games"),
                                    grid.start_x, hy, used_w, std::clamp(appear * 2.0f, 0.0f, 1.0f));
            y += Skin::kSectionHeaderH;
            for (std::size_t k = 0; k < sec.games.size(); ++k) {
                const int col = static_cast<int>(k) % grid.cols;
                if (k > 0 && col == 0) y += kTileH + kTileGap;
                const int gx = grid.start_x + col * (kTileW + kTileGap);
                const int gy = static_cast<int>(view_top + y - scroll);
                const float stagger = std::clamp(appear * 3.0f - col * 0.12f - (gy - view_top) / 150.0f * 0.2f, 0.0f, 1.0f);
                const int i = sec.games[k];
                if (i == selected && !dock_focus) {
                    sel_x = gx;
                    sel_y = gy;
                    room_left = k > 0;
                    room_right = k + 1 < sec.games.size();
                    continue;
                }
                Skin::DrawTile(c, f, tile_info(i), gx, gy, {0.0f, stagger, t, dock_focus});
            }
            y += kTileH + 24;
        }
        if (!dock_focus) {
            Skin::DrawTile(c, f, tile_info(selected), sel_x, sel_y, {lift, 1.0f, t, false});
            const float s = kTileW + (kTileFocus - kTileW) * lift;
            Skin::DrawFocusRing(c, sel_x + kTileW / 2.0f - s / 2, sel_y + kTileH / 2.0f - s / 2 - 3 * lift, s, t, 1.0f);
            if (moving) {
                Skin::DrawMoveArrows(c, float(sel_x), sel_y - 3 * lift, float(kTileW), t, room_left, room_right, false, false);
            }
        }
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

    // Home scrolled down a little, and with a logo put in for the 3DS section.
    for (int i = 0; i < 6; ++i) Frame(c, 12.0, [&](Canvas& v) { home(v, 5, false, 1.3f, 1.0f, 1.0f, 150.0f); });
    SavePng(c, out + "/home_scrolled.png");
    {
        // The logo EmuSwitch ships for the 3DS section (the app loads it from the romfs).
        Image img = LoadPng(std::string{argc > 4 ? argv[4] : "src/citra_switch/assets/logos/3ds.png"});
        img.opaque = false;
        Skin::SetSystemLogo("3ds", std::move(img));
        for (int i = 0; i < 6; ++i) Frame(c, 12.5, [&](Canvas& v) { home(v, 5, false, 1.3f, 1.0f, 1.0f); });
        SavePng(c, out + "/home_logo.png");
        Skin::SetSystemLogo("3ds", Image{});
    }

    // The + menu over the carousel, and a console being moved.
    Skin::SetSpot(225.0f, 330.0f + 90.0f, 230.0f, cards[2].color, 1.0f);
    for (int i = 0; i < 4; ++i) Frame(c, 13.0, [&](Canvas& v) {
        systems(v, 2.0f, 2, 0.4f);
        Skin::DrawScrim(v, 0.7f);
        const int w = 380, h = 150, x = (v.Width() - w) / 2, y = (v.Height() - h) / 2 - 20;
        Skin::DrawPanel(v, x, y, w, h, 22);
        regular.Draw(v, x + 24, y + 30, "Game Boy Advance", 16, Skin::Palette::kColTextDim);
        Skin::DrawRow(v, x + 16, y + 44, w - 32, 52, true);
        bold.Draw(v, x + 40, CenterBaseline(y + 44, 52, 21), "Move Placement", 21, Skin::Palette::kColText);
        int hx = x + 24;
        hx += Skin::DrawHint(v, f, hx, y + h - 34, "A", "Select") + 22;
        Skin::DrawHint(v, f, hx, y + h - 34, "B", "Close");
    });
    SavePng(c, out + "/systems_menu.png");
    for (int i = 0; i < 4; ++i) Frame(c, 13.5, [&](Canvas& v) {
        Skin::DrawBackdrop(v);
        Skin::TopBar bar{"Systems", "Moving Game Boy Advance", "03:02", "PM", 76, true};
        Skin::DrawTopBar(v, f, bar);
        Skin::CarouselText text{"Game Boy Advance", "12 games", "Moving: use the arrows, then A", false, false};
        text.moving = true;
        Skin::DrawSystemsCarousel(v, f, cards, 2.0f, 2, text, cards[2].color, 0.4f);
        Skin::DrawDock(v, f, dock, {1, 1, false, 1.0f, 0.0f, 0.4f});
        hints(v, {{"Up Down", "Move"}, {"A", "Done"}}, {{"B", "Cancel"}});
    });
    SavePng(c, out + "/systems_move.png");

    // A console's games in a column.
    Skin::SetSpot(225.0f, 420.0f, 230.0f, cards[0].color, 0.0f);
    Skin::SetAmbient(red);
    for (int i = 0; i < 30; ++i) Frame(c, 14.0 + i / 30.0, [&](Canvas& v) {
        Skin::DrawBackdrop(v);
        Skin::TopBar bar{"Mario Kart 7", "Nintendo 3DS", "03:02", "PM", 76, true};
        Skin::DrawTopBar(v, f, bar);
        constexpr int kListX = 440, kCardS = 220;
        const int card_x = (kListX - kCardS) / 2 - 10, card_y = kContentTop + 40;
        Skin::DrawSystemCard(v, f, cards[0], card_x, card_y, kCardS);
        bold.Draw(v, card_x + (kCardS - bold.Measure("Nintendo 3DS", 28)) / 2, card_y + kCardS + 50, "Nintendo 3DS", 28,
                  Skin::Palette::kColText);
        regular.Draw(v, card_x + (kCardS - regular.Measure("3 games", 18)) / 2, card_y + kCardS + 80, "3 games", 18,
                     Skin::Palette::kColTextDim);
        const int list_w = v.Width() - kListX - 56;
        const int ids[] = {0, 3, 5};
        const char* detail[] = {"Nintendo", "Nintendo  -  Installed", "Nintendo"};
        for (int k = 0; k < 3; ++k) {
            Skin::DrawGameRow(v, f, tile_info(ids[k]), detail[k], kListX, kContentTop + 14 + k * 102, list_w, 92,
                              k == 2 ? 1.0f : 0.0f);
        }
        Skin::DrawDock(v, f, dock, {1, 1, false, 1.0f, 0.0f, 0.4f});
        hints(v, {{"A", "Play"}}, {{"B", "Consoles"}});
    });
    SavePng(c, out + "/systems_games.png");

    // The loading screen.
    for (int i = 0; i < 4; ++i) Frame(c, 15.0, [&](Canvas& v) {
        Skin::DrawBackdrop(v);
        const float cx = v.Width() / 2.0f, cy = v.Height() / 2.0f - 70.0f;
        v.Glow(static_cast<int>(cx) - 58, static_cast<int>(cy) - 58, 116, 116, 58, 46,
               WithAlpha(Skin::Palette::kColAccent, 70), false);
        Skin::DrawGamepad(v, cx, cy, 112.0f, Skin::Palette::kColText, 0.0f);
        bold.Draw(v, static_cast<int>(cx) - bold.Measure("EmuSwitch", 46) / 2, static_cast<int>(cy) + 118, "EmuSwitch", 46,
                  Skin::Palette::kColText);
        const char* status = "Finding your games...";
        regular.Draw(v, static_cast<int>(cx) - regular.Measure(status, 20) / 2, static_cast<int>(cy) + 156, status, 20,
                     Skin::Palette::kColTextDim);
        Skin::DrawSpinner(v, cx, cy + 212.0f, 13.0f, 0.6f);
    });
    SavePng(c, out + "/loading.png");

    // The + menu on a game (Move Placement, Info) and the picture menu (SD Card, SteamGridDB).
    auto game_menu = [&](Canvas& v, const std::string& heading, const char* row0, const char* row1, int sel,
                         const char* note) {
        home(v, 5, false, 1.3f, 1.0f, 1.0f);
        Skin::DrawScrim(v, 0.7f);
        const int w = 420, h = 44 + 2 * 58 + 52, x = (v.Width() - w) / 2, y = (v.Height() - h) / 2 - 20;
        Skin::DrawPanel(v, x, y, w, h, 22);
        regular.Draw(v, x + 24, y + 30, heading, 16, Skin::Palette::kColTextDim);
        const char* rows[] = {row0, row1};
        for (int r = 0; r < 2; ++r) {
            const int ry = y + 44 + r * 58;
            if (r == sel) Skin::DrawRow(v, x + 16, ry, w - 32, 52, true);
            bold.Draw(v, x + 40, CenterBaseline(ry, 52, 21), rows[r], 21,
                      r == sel ? Skin::Palette::kColText : Skin::Palette::kColTextDim);
            if (note && r == 1) {
                regular.Draw(v, x + w - 38 - regular.Measure(note, 15), CenterBaseline(ry, 52, 15), note, 15,
                             Skin::Palette::kColTextDim);
            }
        }
        int hx = x + 24;
        hx += Skin::DrawHint(v, f, hx, y + h - 30, "A", "Select") + 22;
        Skin::DrawHint(v, f, hx, y + h - 30, "B", "Close");
    };
    Skin::SetAmbient(red);
    for (int i = 0; i < 6; ++i) Frame(c, 16.0, [&](Canvas& v) { game_menu(v, "Mario Kart 7", "Move Placement", "Info", 0, nullptr); });
    SavePng(c, out + "/home_game_menu.png");
    for (int i = 0; i < 6; ++i) Frame(c, 16.5, [&](Canvas& v) {
        game_menu(v, "Picture for Mario Kart 7", "SD Card", "SteamGridDB", 1, "Needs an API key");
    });
    SavePng(c, out + "/home_picture_menu.png");
    // A game being carried to a new place in its section.
    for (int i = 0; i < 6; ++i) Frame(c, 17.0, [&](Canvas& v) { home(v, 3, false, 1.3f, 1.0f, 1.0f, 0.0f, true); });
    SavePng(c, out + "/home_move.png");

    // The SteamGridDB picker. The pictures are stand-ins; the real ones come from steamgriddb.com.
    {
        struct Look {
            int w, h;
            u32 a, b;
            const char* size;
            const char* style;
        };
        const Look looks[] = {
            {360, 360, MakeColor(0xE8, 0x2B, 0x2B), MakeColor(0x6B, 0x0F, 0x1A), "Square  1024 x 1024", "Alternate"},
            {360, 360, MakeColor(0x2B, 0x8C, 0xE8), MakeColor(0x0E, 0x2A, 0x5C), "Square  512 x 512", "No logo"},
            {360, 360, MakeColor(0xF5, 0xB7, 0x2B), MakeColor(0x8A, 0x3C, 0x0B), "Square  1024 x 1024", "Alternate"},
            {240, 360, MakeColor(0x1F, 0xB5, 0x6A), MakeColor(0x0A, 0x3D, 0x26), "Portrait  600 x 900", "Alternate"},
            {240, 360, MakeColor(0x9B, 0x5C, 0xF6), MakeColor(0x2E, 0x14, 0x5C), "Portrait  600 x 900", "Blurred"},
            {240, 360, MakeColor(0x3A, 0x3A, 0x44), MakeColor(0x0C, 0x0C, 0x10), "Portrait  600 x 900", "White logo"},
            {240, 360, MakeColor(0xE8, 0x5B, 0xA0), MakeColor(0x5C, 0x12, 0x38), "Portrait  600 x 900", "Material"},
            {240, 360, MakeColor(0x2D, 0xD4, 0xBF), MakeColor(0x0B, 0x4A, 0x42), "Portrait  660 x 930", "Alternate"},
        };
        std::vector<Image> pics;
        for (const Look& l : looks) pics.push_back(FakeGrid(bold, regular, l.w, l.h, l.a, l.b));
        const int sel = 1;
        for (int i = 0; i < 4; ++i) Frame(c, 18.0, [&](Canvas& v) {
            using namespace Skin::Palette;
            Skin::DrawBackdrop(v);
            regular.Draw(v, 40, 44, "SteamGridDB", 28, kColText);
            regular.Draw(v, 40, 76, "Pictures for Mario Kart 7", 20, kColAccent);
            v.FillRect(40, 96, v.Width() - 80, 1, kColLine);
            constexpr int cols = 4, cell = 176, gap = 16, top = 118;
            const int content_bottom = v.Height() - kHintH;
            for (int k = 0; k < static_cast<int>(pics.size()); ++k) {
                const int x = 40 + (k % cols) * (cell + gap), y = top + (k / cols) * (cell + gap);
                Skin::DrawPanel(v, x, y, cell, cell, 16);
                const Image& p = pics[static_cast<std::size_t>(k)];
                const float inner = cell - 20.0f, s = std::min(inner / p.w, inner / p.h);
                const float dw = p.w * s, dh = p.h * s, cx = x + cell / 2.0f, cy = y + cell / 2.0f;
                v.DrawImageScaled(p, cx - dw / 2, cy - dh / 2, dw, dh, 10);
                if (k == sel) Skin::DrawFocusRing(v, x - 6.0f, y - 6.0f, cell + 12.0f, 0.4f, 1.0f);
            }
            const int pw = 400, px = v.Width() - 40 - pw, ph = std::min(pw + 44, content_bottom - 8 - top);
            Skin::DrawPanel(v, px, top, pw, ph, 22);
            const Image& p = pics[sel];
            const int inner_w = pw - 40, inner_h = ph - 82;
            const float cx = px + pw / 2.0f, cy = top + 20.0f + inner_h / 2.0f;
            const float s = std::min(float(inner_w) / p.w, float(inner_h) / p.h);
            const float dw = p.w * s, dh = p.h * s;
            v.SoftShadow(int(cx - dw / 2), int(cy - dh / 2), int(dw), int(dh), 14, 16, 6, 0x90);
            v.DrawImageScaled(p, cx - dw / 2, cy - dh / 2, dw, dh, 14);
            bold.Draw(v, int(cx) - bold.Measure(looks[sel].size, 18) / 2, top + ph - 44, looks[sel].size, 18, kColText);
            regular.Draw(v, int(cx) - regular.Measure(looks[sel].style, 16) / 2, top + ph - 20, looks[sel].style, 16,
                         kColTextDim);
            int hx = 40;
            const int hy = v.Height() - 44;
            hx += Skin::DrawHint(v, f, hx, hy, "A", "Use") + 22;
            hx += Skin::DrawHint(v, f, hx, hy, "X", "Search") + 22;
            Skin::DrawHint(v, f, hx, hy, "B", "Back");
            const char* credit = "Pictures from SteamGridDB";
            regular.Draw(v, v.Width() - 40 - regular.Measure(credit, 15), hy + 5, credit, 15, kColTextDim);
        });
        SavePng(c, out + "/steamgriddb.png");
    }

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
