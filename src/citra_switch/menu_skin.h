// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// EmuSwitch's look: palette, layout and the drawing of the frame (backdrop, rail,
// header, hints), game cards and system wordmarks. It depends only on menu_gfx.h,
// so tools/menu_preview can render it to PNGs on a desktop.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "citra_switch/menu_gfx.h"

namespace SwitchFrontend::Skin {

using Gfx::Canvas;
using Gfx::Font;
using Gfx::MakeColor;
using Gfx::u32;
using Gfx::u8;

namespace Palette {
// Near-black, with raised surfaces a few steps lighter.
constexpr u32 kColBg = MakeColor(0x08, 0x09, 0x0C);
constexpr u32 kColRail = MakeColor(0x0D, 0x0F, 0x13);
constexpr u32 kColSurface = MakeColor(0x15, 0x17, 0x1C);
constexpr u32 kColSurfaceHi = MakeColor(0x1D, 0x20, 0x27);
constexpr u32 kColBadge = MakeColor(0x26, 0x29, 0x32);
constexpr u32 kColAccent = MakeColor(0x2F, 0xD6, 0xC9);
constexpr u32 kColAccentDim = MakeColor(0x16, 0x4E, 0x4A);
constexpr u32 kColText = MakeColor(0xF3, 0xF4, 0xF7);
constexpr u32 kColTextDim = MakeColor(0x8A, 0x91, 0x9E);
constexpr u32 kColOnAccent = MakeColor(0x04, 0x1C, 0x1A);
constexpr u32 kColError = MakeColor(0xE5, 0x48, 0x4D);
constexpr u32 kColHintBar = MakeColor(0x08, 0x09, 0x0C);
constexpr u32 kColLine = MakeColor(0xFF, 0xFF, 0xFF, 0x10);
} // namespace Palette

namespace Layout {
constexpr int kRailW = 136;
constexpr int kHeaderH = 80;
constexpr int kHintH = 56;
constexpr int kContentX = kRailW;
constexpr int kContentTop = kHeaderH;
constexpr int kRailFirstY = 104;
constexpr int kRailItemH = 80;
constexpr int kRailItemStep = 92;
constexpr int kTileW = 200;
constexpr int kTileH = 232;
constexpr int kTileGap = 20;
constexpr int kIconSize = 128;
} // namespace Layout

// The fonts the skin draws with.
struct Fonts {
    Font* regular;
    Font* bold;
    Font* mark; // heavy italic, for system wordmarks
};

// Library grid geometry for a screen size.
struct Grid {
    int cols{};
    int start_x{};
    int top{};
    int visible_rows{};
};
Grid ComputeGrid(int screen_w, int screen_h);

// Backdrop: smooth near-black gradient with a faint glow, rendered once per size.
void DrawBackdrop(Canvas& c);

struct RailEntry {
    const char* label;
    const std::uint8_t* mask; // RailIcons coverage mask, or null for a text glyph
};
void DrawRail(Canvas& c, const Fonts& f, const std::vector<RailEntry>& items, int pill, int ghost);
void DrawHeader(Canvas& c, const Fonts& f, std::string_view title, std::string_view subtitle);

// One library card.
struct TileInfo {
    std::string_view title;
    std::string_view subtitle;           // publisher or system name
    const std::vector<u32>* icon{};      // 3DS SMDH icon, or null
    int icon_size{};
    std::string_view system_id{"3ds"};   // "3ds" or a Multi system id
    std::string_view art_key;            // game path: a custom picture set for it wins
    std::vector<std::string_view> tags;  // small chips (LOCKED, SD, CART)
};

// Custom pictures, decoded to straight-alpha RGBA. An empty buffer removes one.
// System pictures replace a system's wordmark plate; game pictures replace a card's art.
void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h);
bool HasSystemImage(const std::string& id);
void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h);
bool HasGameImage(const std::string& path);
void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, bool selected, bool focused);

// A system's wordmark on a coloured plate filling (x, y, w, h). `id` is "3ds" or a Multi id.
void DrawSystemMark(Canvas& c, const Fonts& f, std::string_view id, int x, int y, int w, int h, int radius);

int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label);
void DrawHintBar(Canvas& c);
void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h);
void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error);
void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir);

} // namespace SwitchFrontend::Skin
