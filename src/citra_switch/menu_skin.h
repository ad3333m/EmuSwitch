// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// EmuSwitch's look: a dark, softly lit home screen with frosted-glass bars and panels,
// raised tiles, a floating dock and a systems carousel. It depends only on menu_gfx, so
// tools/menu_preview can render it to PNGs on a desktop.
//
// Frames are drawn once without pixels (a "warm-up" pass that fills the caches) and then
// in parallel bands; see Gfx::Canvas. Only BeginFrame/EndWarmup and the setters may change
// state, and they run on one thread between frames.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "citra_switch/menu_gfx.h"

namespace SwitchFrontend::Skin {

using Gfx::Canvas;
using Gfx::Font;
using Gfx::Image;
using Gfx::MakeColor;
using Gfx::u32;
using Gfx::u8;

// The menu's colours. ApplyTheme() sets them (between frames); these are Midnight's.
namespace Palette {
inline u32 kColBg = MakeColor(0x06, 0x07, 0x0B);
inline u32 kColRail = MakeColor(0x2A, 0x2A, 0x36);
inline u32 kColSurface = MakeColor(0x1C, 0x1C, 0x26);
inline u32 kColSurfaceHi = MakeColor(0x2A, 0x2A, 0x38);
inline u32 kColBadge = MakeColor(0x34, 0x33, 0x42);
inline u32 kColAccent = MakeColor(0x5E, 0xE7, 0xDF);
inline u32 kColAccent2 = MakeColor(0x8B, 0x7C, 0xFF);
inline u32 kColAccentDim = MakeColor(0x1E, 0x4E, 0x52);
inline u32 kColText = MakeColor(0xF5, 0xF5, 0xFB);
inline u32 kColTextDim = MakeColor(0xA4, 0xA3, 0xBA);
inline u32 kColOnAccent = MakeColor(0x04, 0x1A, 0x19);
inline u32 kColError = MakeColor(0xFF, 0x5A, 0x6A);
inline u32 kColHintBar = MakeColor(0x06, 0x07, 0x0B);
inline u32 kColLine = MakeColor(0xFF, 0xFF, 0xFF, 0x14);
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
constexpr int kTileR = 22;        // tile corner radius
constexpr int kTileFocus = 144;   // a focused tile's size once it has popped up
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

// ---- frames ------------------------------------------------------------------------------------

// Starts a frame at `now` seconds: moves the backdrop along and lets the caches fill again.
void BeginFrame(double now, int screen_w, int screen_h);
// Switches the menu to theme `index` (themes.h): the palette, backdrop, glass and focus ring.
// Call between frames; the cached pictures of the old colours are dropped.
void ApplyTheme(int index);
// After the warm-up pass: nothing new is built until the next BeginFrame, so every band of the
// frame sees the same pictures.
void EndWarmup();
// The colour the backdrop's glow drifts towards (the focused game's).
void SetAmbient(u32 color);
// A pool of coloured light in the backdrop centred on (x, y), fading towards `strength` (0..1).
void SetSpot(float x, float y, float radius, u32 color, float strength);

void DrawBackdrop(Canvas& c);

// ---- profile bar ---------------------------------------------------------------------------------
void SetAvatar(std::vector<u32> rgba, int w, int h);
// The profile's nickname: its first letter stands in while there's no avatar picture.
void SetProfileName(std::string_view name);
struct TopBar {
    std::string_view title;     // the nickname on Home, a page or system name elsewhere
    std::string_view subtitle;  // small line under it
    std::string_view clock;     // "03:02"
    std::string_view ampm;      // "PM" or empty
    int battery = -1;           // 0..100, -1 hides it
    bool charging = false;
    // The title fades in over the previous one while `blend` goes 0 -> 1.
    std::string_view prev_title;
    std::string_view prev_subtitle;
    float blend = 1.0f;
};
void DrawTopBar(Canvas& c, const Fonts& f, const TopBar& bar);

// ---- dock ----------------------------------------------------------------------------------------
enum class DockIcon { Home, Systems, Install, Settings, Folder, Text };
struct DockItem {
    const char* label;
    DockIcon icon;
    const char* text = nullptr; // for DockIcon::Text
};
struct DockState {
    int active = 0;       // the open page
    int cursor = 0;       // where the dock cursor sits when focused
    bool focused = false;
    float slide = 0.0f;   // the highlight's (fractional) position, eased towards active/cursor
    float label = 0.0f;   // label bubble opacity
    float t = 0.0f;       // seconds
};
void DrawDock(Canvas& c, const Fonts& f, const std::vector<DockItem>& items, const DockState& s);
// Dock item under a touch, or -1.
int DockHitTest(const Canvas& c, int item_count, int x, int y);

// ---- tiles ---------------------------------------------------------------------------------------
struct TileInfo {
    std::string_view title;
    std::string_view system_badge;   // "3DS", "GBA", ...
    u32 system_color = MakeColor(0xE2, 0x1B, 0x33);
    const std::vector<u32>* icon{};  // 3DS SMDH icon, or null
    int icon_size{};
    std::string_view art_key;        // game path: a custom picture set for it wins
    std::vector<std::string_view> tags;
};
struct TileLook {
    float lift = 0.0f;    // 0 resting .. 1 focused (may overshoot a little)
    float appear = 1.0f;  // 0 hidden .. 1 shown: tiles rise and fade in
    float t = 0.0f;       // seconds, for the sheen
    bool dim = false;     // the grid isn't focused
};
// Draws a tile whose resting top-left is (x, y).
void DrawTile(Canvas& c, const Fonts& f, const TileInfo& t, int x, int y, const TileLook& look);
void DrawEmptySlot(Canvas& c, int x, int y, float alpha);
// The cursor ring around a (w x w) square at (x, y); it glides between tiles.
void DrawFocusRing(Canvas& c, float x, float y, float w, float t, float alpha);
// One theme on the Themes page: a small picture of the menu in that theme's colours, its name
// under it, "In use" on the active one and the focus ring around the one the cursor is on.
void DrawThemeCard(Canvas& c, const Fonts& f, int x, int y, int w, int h, int theme, bool focused,
                   bool active, float t);
// Arrows around a (w x w) square being moved, on the sides it can still go.
void DrawMoveArrows(Canvas& c, float x, float y, float w, float t, bool left, bool right, bool up, bool down);
// The colour a tile's picture is mostly made of, for the glow and the backdrop.
u32 TileAccent(const TileInfo& t);
// The focused game's name in a pill under the grid.
void DrawTitlePill(Canvas& c, const Fonts& f, int y, std::string_view title, std::string_view system, u32 color);

// ---- systems carousel ------------------------------------------------------------------------------
struct SystemCard {
    std::string_view id;
    std::string_view badge;
    u32 color;
};
struct CarouselText {
    std::string_view name;
    std::string_view detail;
    std::string_view status;
    bool status_ok = true;
    bool has_picture = false;
    bool picture_button = true; // offer "Change picture"
    bool moving = false;        // the focused card is being moved: arrows above and below it
};
// Draws the carousel with `anim` the (fractional) focused index; `accent` is the eased
// colour of the focused system.
void DrawSystemsCarousel(Canvas& c, const Fonts& f, const std::vector<SystemCard>& cards, float anim,
                         int selected, const CarouselText& text, u32 accent, float t);
// Where the focused card sits, for touch: returns the card index under (x, y) or -1.
int CarouselHitTest(const Canvas& c, int card_count, float anim, int x, int y);
// The "Change picture" button beside the focused card.
bool CarouselPictureButtonHit(const Canvas& c, int x, int y);
// A shaded game controller in the system's colour, `width` pixels wide, centred on (cx, cy).
void DrawController(Canvas& c, float cx, float cy, float width, u32 accent, float t);

// ---- Home sections and a console's games ---------------------------------------------------------------
// How tall a Home section's header is; its games start this far below the header's top.
constexpr int kSectionHeaderH = 58;
// A console's logo `h` pixels high at (x, y): the logo picture put in for it (see SetSystemLogo),
// or its short name in a pill of its colour. Returns the width it took.
int DrawSystemLogo(Canvas& c, const Fonts& f, const SystemCard& card, int x, int y, int h);
// The header over a console's games on Home: its logo, its name and game count, and a hairline
// running on to x + w.
void DrawSectionHeader(Canvas& c, const Fonts& f, const SystemCard& card, std::string_view name,
                       std::string_view count, int x, int y, int w, float alpha);
// A console's card (its picture, or its default look), `s` pixels square, lit in its colour.
void DrawSystemCard(Canvas& c, const Fonts& f, const SystemCard& card, int x, int y, int s);
// One game in a list: its picture, name and a detail line. `focus` (0..1) lights the row up.
void DrawGameRow(Canvas& c, const Fonts& f, const TileInfo& t, std::string_view detail, int x, int y, int w,
                 int h, float focus);

// ---- pictures ----------------------------------------------------------------------------------------
// Pictures arrive already scaled down (see custom_art.cpp); the skin keeps them and the
// exact-size copies it draws.
void SetSystemImage(const std::string& id, Image img);
bool HasSystemImage(const std::string& id);
void SetGameImage(const std::string& path, Image img);
bool HasGameImage(const std::string& path);
// A console's logo for its Home section; an empty image goes back to the drawn badge.
void SetSystemLogo(const std::string& id, Image img);
bool HasSystemLogo(const std::string& id);
// Legacy forms taking raw RGBA.
void SetSystemImage(const std::string& id, std::vector<u32> rgba, int w, int h);
void SetGameImage(const std::string& path, std::vector<u32> rgba, int w, int h);
// Drops every exact-size copy (e.g. before a game runs, to hand the memory back).
void TrimCaches();

// ---- panels ------------------------------------------------------------------------------------------
// Dims whatever is under a modal; `alpha` 0..1 fades it in.
void DrawScrim(Canvas& c, float alpha);
// A frosted-glass panel.
void DrawPanel(Canvas& c, int x, int y, int w, int h, int r = 20);
// Scrim plus panel, the usual modal.
void DrawModal(Canvas& c, int x, int y, int w, int h);
// The highlight behind a selected list row.
void DrawRow(Canvas& c, int x, int y, int w, int h, bool focused);
// A tab or chip: filled when active.
void DrawPill(Canvas& c, int x, int y, int w, int h, bool active);
// A progress bar with a moving sheen.
void DrawProgress(Canvas& c, int x, int y, int w, int h, float frac, float t);
// A spinning arc, for waits.
void DrawSpinner(Canvas& c, float cx, float cy, float r, float t);
// A thin vertical scrollbar.
void DrawScrollbar(Canvas& c, int x, int top, int track_h, int thumb_y, int thumb_h);

// ---- small pieces --------------------------------------------------------------------------------------
int DrawHint(Canvas& c, const Fonts& f, int x, int y, const char* button, const char* label);
void DrawHintBar(Canvas& c);
// A notice above the dock; `alpha` 0..1 slides and fades it.
void DrawToast(Canvas& c, const Fonts& f, std::string_view text, bool error, float alpha = 1.0f);
void DrawEmptyLibrary(Canvas& c, const Fonts& f, std::string_view roms_dir, float t = 0.0f);
// A small filled controller glyph (dock icon style), any size.
void DrawGamepad(Canvas& c, float cx, float cy, float size, u32 color, float stroke);

} // namespace SwitchFrontend::Skin
