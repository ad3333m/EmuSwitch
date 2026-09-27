// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// EmuSwitch's look: a black home screen with a profile bar, raised tiles, a floating
// dock and a systems carousel. It depends only on menu_gfx.h, so tools/menu_preview
// can render it to PNGs on a desktop.

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
constexpr u32 kColBg = MakeColor(0x07, 0x07, 0x0A);
constexpr u32 kColRail = MakeColor(0x14, 0x13, 0x1B);
constexpr u32 kColSurface = MakeColor(0x1A, 0x19, 0x22);
constexpr u32 kColSurfaceHi = MakeColor(0x24, 0x23, 0x2E);
constexpr u32 kColBadge = MakeColor(0x2C, 0x2B, 0x37);
constexpr u32 kColAccent = MakeColor(0x48, 0xE6, 0xD6);
constexpr u32 kColAccentDim = MakeColor(0x1E, 0x4E, 0x52);
constexpr u32 kColText = MakeColor(0xF4, 0xF3, 0xFA);
constexpr u32 kColTextDim = MakeColor(0xA0, 0x9C, 0xB6);
constexpr u32 kColOnAccent = MakeColor(0x05, 0x1C, 0x1A);
constexpr u32 kColError = MakeColor(0xE5, 0x48, 0x4D);
constexpr u32 kColHintBar = MakeColor(0x07, 0x07, 0x0A);
constexpr u32 kColLine = MakeColor(0xFF, 0xFF, 0xFF, 0x12);
} // namespace Palette

namespace Layout {
constexpr int kRailW = 0;         // no side rail: the dock sits at the bottom
constexpr int kHeaderH = 108;     // profile bar
constexpr int kHintH = 132;       // dock plus the hint row under it
constexpr int kContentX = kRailW;
constexpr int kContentTop = kHeaderH;
constexpr int kRailFirstY = 0;    // unused by the dock, kept for the menu's hit tests
constexpr int kRailItemH = 0;
constexpr int kRailItemStep = 0;
constexpr int kTileW = 128;
constexpr int kTileH = 128;
constexpr int kTileGap = 22;
constexpr int kIconSize = 128;
constexpr int kDockH = 68;
} // namespace Layout

struct Fonts {
    Font* regular;
    Font* bold;
    Font* mark; // heavy italic: tile initials and system card labels
};

struct Grid {
    int cols{};
    int start_x{};
    int top{};
    int visible_rows{};
};
Grid ComputeGrid(int screen_w, int screen_h);

void DrawBackdrop(Canvas& c);

// ---- profile bar ----------------------------------------------------------------
void SetAvatar(std::vector<u32> rgba, int w, int h);
struct TopBar {
    std::string_view title;     // the nickname on Home, a page or system name elsewhere
    std::string_view subtitle;  // small line under it
    std::string_view clock;     // "03:02"
    std::string_view ampm;      // "PM" or empty
    int battery = -1;           // 0..100, -1 hides it
    bool charging = false;
};
void DrawTopBar(Canvas& c, const Fonts& f, const TopBar& bar);

// ---- dock -------------------------------------------------------------------------
enum class DockIcon { Home, Systems, Install, Settings, Folder, Text };
struct DockItem {
    const char* label;
    DockIcon icon;
    const char* text = nullptr; // for DockIcon::Text
};
// `active` is the open page; `cursor` is where the dock cursor sits when focused.
void DrawDock(Canvas& c, const Fonts& f, const std::vector<DockItem>& items, int active, int cursor, bool focused);
// Dock item under a touch, or -1.
int DockHitTest(const Canvas& c, int item_count, int x, int y);

// ---- tiles ------------------------------------------------------------------------
struct TileInfo {
    std::string_view title;
    std::string_view system_badge;   // "3DS", "GBA", ...
    u32 system_color = MakeColor(0xE2, 0x1B, 0x33);
    const std::vector<u32>* icon{};  // 3DS SMDH icon, or null
    int icon_size{};
    std::string_view art_key;        // game path: a custom picture set for it wins
    std::vector<std::string_view> tags;
};
void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, bool selected, bool focused, float t_anim);
void DrawEmptySlot(Canvas& c, int x, int y);
// The focused game's name in a pill under the grid.
void DrawTitlePill(Canvas& c, const Fonts& f, int y, std::string_view title, std::string_view system, u32 color);

// ---- systems carousel ----------------------------------------------------------------
struct SystemCard {
    std::string_view id;
    std::string_view badge;
    u32 color;
};
// Draws the vertical carousel with `anim` the (fractional) focused index.
void DrawSystemsCarousel(Canvas& c, const Fonts& f, const std::vector<SystemCard>& cards, float anim, int selected,
                         std::string_view name, std::string_view detail, std::string_view status, bool status_ok);

// ---- pictures ------------------------------------------------------------------------
void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h);
bool HasSystemImage(const std::string& id);
void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h);
bool HasGameImage(const std::string& path);

// ---- small pieces ----------------------------------------------------------------------
int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label);
void DrawHintBar(Canvas& c);
void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h);
void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error);
void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir);
// A crisp vector gamepad outline (the Systems icon), any size.
void DrawGamepad(Canvas& c, float cx, float cy, float size, u32 color, float stroke);

} // namespace SwitchFrontend::Skin
