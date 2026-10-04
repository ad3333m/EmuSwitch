// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "citra_switch/config.h"
#include "citra_switch/cover_fetch.h"
#include "citra_switch/custom_art.h"
#include "citra_switch/input.h"
#include "citra_switch/menu.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/menu_gfx.h"
#include "citra_switch/menu_skin.h"
#include "citra_switch/multi_system.h"
#include "citra_switch/rail_icons.h"
#include "citra_switch/save_manager.h"
#include "citra_switch/settings_menu.h"
#include "citra_switch/steamgriddb.h"
#include "citra_switch/updater.h"
#include "citra_switch/usb_storage.h"
#include "common/horizon_boost.h"
#include "common/horizon_thread.h"

namespace SwitchFrontend {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using namespace Gfx;
using namespace Skin::Palette;
using namespace Skin::Layout;

// The canvas the menu lays itself out on.
int g_screen_w = kPanelW;
int g_screen_h = kPanelH;
int g_rotation = 0; // Degrees clockwise.

bool RotatedUpright() {
    return g_rotation == 90 || g_rotation == 270;
}





Font g_font;
Font g_font_bold;
Font g_font_mark;

Skin::Fonts SkinFonts() {
    return {&g_font, &g_font_bold, &g_font_mark};
}

std::string g_nickname = "Player";

// Seconds since the menu first drew, for animations. Only changes between frames.
double g_now = 0.0;
float AnimTime() {
    return static_cast<float>(g_now);
}
double NowSeconds() {
    static const u64 start = armGetSystemTick();
    return static_cast<double>(armTicksToNs(armGetSystemTick() - start)) / 1e9;
}

// After a game the menu comes in from black, the way the game faded out, instead of cutting in.
// The fade starts with the first frame drawn; kMenuFadeArmed means it's waiting for it.
constexpr double kMenuFadeIdle = -1.0;
constexpr double kMenuFadeArmed = -2.0;
constexpr double kMenuFadeInSeconds = 0.35;
double g_menu_fade_start = kMenuFadeIdle;

// How much black still covers the menu (0..1), advancing the fade.
float MenuFadeCover() {
    if (g_menu_fade_start == kMenuFadeIdle) {
        return 0.0f;
    }
    const double now = NowSeconds();
    if (g_menu_fade_start == kMenuFadeArmed) {
        g_menu_fade_start = now;
    }
    const double t = (now - g_menu_fade_start) / kMenuFadeInSeconds;
    if (t >= 1.0) {
        g_menu_fade_start = kMenuFadeIdle;
        return 0.0f;
    }
    const float left = static_cast<float>(1.0 - t);
    return left * left * (3.0f - 2.0f * left);
}

// Puts the chosen theme into the skin whenever it changes (it can, on the Settings page).
// Between frames only.
void ApplyMenuTheme() {
    static int applied = -1;
    const int theme = GetMenuTheme();
    if (theme != applied) {
        applied = theme;
        Skin::ApplyTheme(theme);
    }
}

// Shows opaque black in `fb`, so it's black rather than the last menu picture that stays up
// while the display changes hands.
void PresentBlack(Framebuffer& fb) {
    u32 stride = 0;
    auto* px = static_cast<u32*>(framebufferBegin(&fb, &stride));
    if (!px) {
        return;
    }
    std::fill(px, px + fb.fb_size / sizeof(u32), 0xFF000000u);
    framebufferEnd(&fb);
}

// Fonts and drawing threads, set up once for the loading screen and the menu.
bool EnsureMenuGraphics() {
    static bool tried = false, ok = false;
    if (tried) {
        return ok;
    }
    tried = true;
    // Inter from the romfs, with the console's fonts behind it for anything it lacks.
    g_font_bold.Init("romfs:/fonts/Inter-Bold.ttf");
    g_font_mark.Init("romfs:/fonts/Inter-BlackItalic.ttf");
    ok = g_font.Init("romfs:/fonts/Inter-Medium.ttf");
    // The drawing workers take cores 1 and 2; the menu's own thread keeps core 0.
    Workers::SetStartHook([](int index) { Common::Horizon::PinCurrentThread(static_cast<std::uint32_t>(1 + index)); });
    return ok;
}

// Draws `scene` once without pixels (so every cache it needs is built on this thread), then
// in bands across the cores, each band going straight into the framebuffer.
void RenderToFramebuffer(Canvas& canvas, Framebuffer& fb, const std::function<void(Canvas&)>& scene) {
    {
        Canvas warm = canvas.View(0, 0);
        scene(warm);
    }
    Skin::EndWarmup();
    u32 stride = 0;
    auto* fb_base = static_cast<u8*>(framebufferBegin(&fb, &stride));
    if (fb_base == nullptr) {
        // The framebuffer was never created (see Menu::EnsureFramebuffer): skip the frame
        // rather than write through a null pointer.
        return;
    }
    const int h = canvas.Height();
    const int rotation = g_rotation;
    Workers::Get().Run(kBands, [&](int i) {
        int y0, y1;
        BandRows(h, i, y0, y1);
        if (y0 >= y1) {
            return;
        }
        Canvas band = canvas.View(y0, y1);
        scene(band);
        WriteBlockLinear(canvas, y0, y1, rotation, fb_base, stride);
    });
    framebufferEnd(&fb);
}

// The loading screen: the app's name over the backdrop, what it's doing, and a spinner.
void DrawStartupScene(Canvas& c, std::string_view status) {
    Skin::DrawBackdrop(c);
    const float cx = g_screen_w / 2.0f, cy = g_screen_h / 2.0f - 70.0f;
    // A slow breath of light behind the controller mark.
    const float pulse = 0.5f + 0.5f * std::sin(AnimTime() * 2.2f);
    c.Glow(static_cast<int>(cx) - 58, static_cast<int>(cy) - 58, 116, 116, 58, 46,
           WithAlpha(kColAccent, static_cast<u8>(40 + 50 * pulse)), false);
    Skin::DrawGamepad(c, cx, cy, 112.0f, kColText, 0.0f);
    const char* name = "EmuSwitch";
    const int nw = g_font_bold.Measure(name, 46);
    g_font_bold.Draw(c, static_cast<int>(cx) - nw / 2, static_cast<int>(cy) + 118, name, 46, kColText);
    const int sw = g_font.Measure(status, 20);
    g_font.Draw(c, static_cast<int>(cx) - sw / 2, static_cast<int>(cy) + 156, status, 20, kColTextDim);
    Skin::DrawSpinner(c, cx, cy + 212.0f, 13.0f, AnimTime());
}

// The loading screen put up before the menu exists; the menu takes its framebuffer over.
Framebuffer g_splash_fb{};
bool g_splash_fb_ready = false;

// Exponential approach: `x` covers the given fraction of the way to `target` per `half` seconds.
float Approach(float x, float target, float dt, float half) {
    return target + (x - target) * std::exp2(-dt / std::max(1e-4f, half));
}

// A damped spring, for motion that should overshoot a little and settle.
struct Spring {
    float x = 0.0f;
    float v = 0.0f;
    void Step(float target, float dt, float stiffness = 260.0f, float damping = 22.0f) {
        // Small steps keep it stable when a frame runs long.
        const int n = std::max(1, static_cast<int>(std::ceil(dt / (1.0f / 240.0f))));
        const float h = dt / n;
        for (int i = 0; i < n; ++i) {
            v += (stiffness * (target - x) - damping * v) * h;
            x += v * h;
        }
    }
    void Snap(float target) {
        x = target;
        v = 0.0f;
    }
};

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Each system's short badge and colour for tiles and the carousel.
std::string_view SystemBadge(int system) {
    return system >= 0 ? std::string_view{Multi::Systems()[system].badge} : std::string_view{"3DS"};
}
u32 SystemColor(int system) {
    if (system < 0) {
        return MakeColor(0xE2, 0x1B, 0x33);
    }
    const Multi::System& s = Multi::Systems()[system];
    return MakeColor(s.r, s.g, s.b);
}
std::string_view SystemName(int system) {
    return system >= 0 ? std::string_view{Multi::Systems()[system].name} : std::string_view{"Nintendo 3DS"};
}


struct Repeater {
    int held_frames[4]{};

    // Returns a bitmask of directions that should act this frame.
    u32 Step(bool up, bool down, bool left, bool right) {
        const bool active[4] = {up, down, left, right};
        u32 fired = 0;
        for (int d = 0; d < 4; ++d) {
            if (!active[d]) {
                held_frames[d] = 0;
                continue;
            }
            const int f = held_frames[d]++;
            if (f == 0 || (f >= 24 && (f - 24) % 5 == 0)) {
                fired |= 1u << d;
            }
        }
        return fired;
    }
};
enum { DirUp = 1, DirDown = 2, DirLeft = 4, DirRight = 8 };

u32 NavMask(const MenuDirections& d) {
    return (d.up ? DirUp : 0) | (d.down ? DirDown : 0) | (d.left ? DirLeft : 0) |
           (d.right ? DirRight : 0);
}

enum class Tab { Library, Systems, Settings, Paths };

// Which pane the cursor lives in.
enum class Focus { Rail, Content };

// Indexed by Tab, so the order has to match the enum.
constexpr std::array<std::pair<Tab, const char*>, 4> kRailItems{{{Tab::Library, "Home"},
                                                                 {Tab::Systems, "Systems"},
                                                                 {Tab::Settings, "Settings"},
                                                                 {Tab::Paths, "Paths"}}};

constexpr bool RailItemsMatchTabs() {
    for (int i = 0; i < static_cast<int>(kRailItems.size()); ++i) {
        if (static_cast<int>(kRailItems[i].first) != i) {
            return false;
        }
    }
    return true;
}
static_assert(RailItemsMatchTabs(), "kRailItems must be indexable by Tab");


int RailItemTop(int index) {
    return kRailFirstY + index * kRailItemStep;
}

std::optional<Tab> RailHitTest(int y) {
    for (int i = 0; i < static_cast<int>(kRailItems.size()); ++i) {
        const int top = RailItemTop(i);
        if (y >= top && y < top + kRailItemH) {
            return kRailItems[i].first;
        }
    }
    return std::nullopt;
}

int ContentW() {
    return g_screen_w - kRailW;
}

int ContentBottom() {
    return g_screen_h - kHintH;
}


std::string g_notice;
int g_notice_frames = 0;
bool g_notice_is_error = true;
bool g_auto_update_checked = false;
// CIA files are looked for once per session, when the menu first opens (and on a refresh).
bool g_auto_install_checked = false;
// Likewise box art for games without a picture.
bool g_covers_checked = false;

// ~4 seconds at 60fps.
constexpr int kNoticeFrames = 240;

void ShowNotice(const std::string& text, bool error) {
    g_notice = text;
    g_notice_frames = kNoticeFrames;
    g_notice_is_error = error;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// The Settings page strip, which sits between the header and the rows.
constexpr int kTabStripTop = kContentTop + 10;
constexpr int kTabStripH = 34;
constexpr int kTabPadX = 14;
constexpr int kTabPadMin = 4;
constexpr int kTabGap = 6;

struct TabRect {
    int x{};
    int w{};
};

struct Rect {
    int x{};
    int y{};
    int w{};
    int h{};
    bool Contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Lays the page chips out as one centred row, tightening the padding rather than overflowing the
// content area if the shared font measures wider than the nominal padding allows for.
std::array<TabRect, NumCategories> SettingsTabRects() {
    std::array<TabRect, NumCategories> rects{};
    int text = 0;
    for (int i = 0; i < NumCategories; ++i) {
        rects[i].w = g_font.Measure(CategoryName(static_cast<Category>(i)), 18);
        text += rects[i].w;
    }
    const int available = ContentW() - 48 - (NumCategories - 1) * kTabGap;
    const int pad = std::clamp((available - text) / (2 * NumCategories), kTabPadMin, kTabPadX);

    int total = 0;
    for (int i = 0; i < NumCategories; ++i) {
        rects[i].w += pad * 2;
        total += rects[i].w + (i > 0 ? kTabGap : 0);
    }
    int x = kContentX + std::max(24, (ContentW() - total) / 2);
    for (int i = 0; i < NumCategories; ++i) {
        rects[i].x = x;
        x += rects[i].w + kTabGap;
    }
    return rects;
}

// Where the label sits inside its chip, which follows the same tightening.
int SettingsTabTextInset(const std::array<TabRect, NumCategories>& rects, int index) {
    return (rects[index].w -
            g_font.Measure(CategoryName(static_cast<Category>(index)), 18)) /
           2;
}

bool CompactTabStrip() {
    int total = (NumCategories - 1) * kTabGap + NumCategories * 2 * kTabPadMin;
    for (int i = 0; i < NumCategories; ++i) {
        total += g_font.Measure(CategoryName(static_cast<Category>(i)), 18);
    }
    return total > ContentW() - 48;
}

std::optional<int> SettingsTabHitTest(int x, int y, int active) {
    if (y < kTabStripTop || y >= kTabStripTop + kTabStripH) {
        return std::nullopt;
    }
    if (CompactTabStrip()) {
        const int step = x < kContentX + ContentW() / 2 ? -1 : 1;
        return (active + step + NumCategories) % NumCategories;
    }
    const auto rects = SettingsTabRects();
    for (int i = 0; i < NumCategories; ++i) {
        if (x >= rects[i].x && x < rects[i].x + rects[i].w) {
            return i;
        }
    }
    return std::nullopt;
}

// swkbd prompt for the text-valued settings rows.
std::optional<std::string> PromptSettingText(const char* header, const char* guide,
                                             const std::string& initial, int max_length) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return std::nullopt;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, max_length);
    // swkbd counts the limit in characters, which UTF-8 can take four bytes each of.
    std::vector<char> out(static_cast<std::size_t>(max_length) * 4 + 1, '\0');
    const Result rc = swkbdShow(&kbd, out.data(), out.size());
    swkbdClose(&kbd);
    if (R_FAILED(rc)) {
        return std::nullopt;
    }
    return std::string{out.data()};
}

// Rows on the Paths page.
enum PathRow { PathRowUserDir, PathRowRomsDir, PathRowRomsDir2, PathRowRecursive, PathRowCount };

constexpr int kPathRowH = 76;
constexpr int kPathToggleH = 52;
constexpr int kPathRowGap = 8;

int PathRowHeight(int row) {
    return row == PathRowRecursive ? kPathToggleH : kPathRowH;
}

int PathRowTop(int row) {
    int y = kContentTop + 16;
    for (int i = 0; i < row; ++i) {
        y += PathRowHeight(i) + kPathRowGap;
    }
    return y;
}

const char* PathRowLabel(int row) {
    switch (row) {
    case PathRowUserDir:
        return "EmuSwitch Folder";
    case PathRowRomsDir:
        return "ROM Folder";
    case PathRowRomsDir2:
        return "Second ROM Folder";
    default:
        return "Scan Subfolders";
    }
}

// The country picker is the one modal list long enough to need paging.
constexpr int kCountryRows = 9;
constexpr int kCountryRowH = 40;

// Backup list inside the save manager panel.
constexpr int kSavesRows = 6;
constexpr int kSavesRowH = 34;

// The folder browser covers the whole screen, rail included.
constexpr int kBrowseTop = 108;
constexpr int kBrowseRowH = 44;
int BrowseRows() {
    return std::max(1, (ContentBottom() - kBrowseTop) / kBrowseRowH);
}

void DrawListScrollbar(Canvas& c, int track_x, int top, int visible_rows, int row_h, int count,
                       int scroll) {
    if (count <= visible_rows) {
        return;
    }
    const int track_h = visible_rows * row_h;
    const int thumb_h = std::max(24, track_h * visible_rows / count);
    const int max_scroll = count - visible_rows;
    const int thumb_y = top + (track_h - thumb_h) * scroll / std::max(1, max_scroll);
    Skin::DrawScrollbar(c, track_x, top, track_h, thumb_y, thumb_h);
}

// Where a hint row starts.
int HintX() {
    return g_screen_w >= kPanelW ? kContentX + 24 : 24;
}

// Draws a small button chip
int DrawHint(Canvas& canvas, int x, int y, const char* button, const char* label) {
    return Skin::DrawHint(canvas, SkinFonts(), x, y, button, label);
}


const std::vector<Skin::DockItem>& DockItems() {
    static const std::vector<Skin::DockItem> items = {
        {"Home", Skin::DockIcon::Home},
        {"Systems", Skin::DockIcon::Systems},
        {"Settings", Skin::DockIcon::Settings},
        {"Paths", Skin::DockIcon::Folder},
    };
    return items;
}

void DrawRail(Canvas& canvas, const Skin::DockState& state) {
    Skin::DrawDock(canvas, SkinFonts(), DockItems(), state);
}

// Toast opacity for a notice with `frames` left: it rises in and fades out.
float NoticeAlpha() {
    if (g_notice_frames <= 0 || g_notice.empty()) {
        return 0.0f;
    }
    const float in = std::min(1.0f, static_cast<float>(kNoticeFrames - g_notice_frames) / 12.0f);
    const float out = std::min(1.0f, static_cast<float>(g_notice_frames) / 18.0f);
    return std::min(in, out);
}

void DrawNotice(Canvas& canvas) {
    const float alpha = NoticeAlpha();
    if (alpha > 0.0f) {
        Skin::DrawToast(canvas, SkinFonts(), g_notice, g_notice_is_error, alpha);
    }
}

Skin::TileInfo TileFor(const GameEntry& game) {
    Skin::TileInfo t;
    t.title = game.title;
    t.system_badge = SystemBadge(game.system);
    t.system_color = SystemColor(game.system);
    t.art_key = game.path;
    if (!game.icon.empty()) {
        t.icon = &game.icon;
        t.icon_size = game.icon_size;
    }
    if (game.encrypted) {
        t.tags.push_back("LOCKED");
    }
    if (game.insertable && GetInsertedCartridge() == game.path) {
        t.tags.push_back("CART");
    }
    if (game.installed) {
        t.tags.push_back("SD");
    }
    return t;
}

void DrawTile(Canvas& canvas, const GameEntry& game, int x, int y, const Skin::TileLook& look) {
    Skin::DrawTile(canvas, SkinFonts(), TileFor(game), x, y, look);
}

void DrawEmptyLibrary(Canvas& canvas, const std::string& roms_dir) {
    Skin::DrawEmptyLibrary(canvas, SkinFonts(), roms_dir, AnimTime());
}

// Layout of the library grid
using Skin::Grid;

Grid ComputeGrid() {
    return Skin::ComputeGrid(g_screen_w, g_screen_h);
}

// swkbd search prompt
std::string PromptSearch(const std::string& initial) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return initial;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Search library");
    swkbdConfigSetGuideText(&kbd, "Game title");
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 128);
    char out[256] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? std::string{out} : initial;
}

std::string FormatTitleId(u64 program_id) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(program_id));
    return buf;
}

// Modal panel listing what is installed alongside one library entry.
void DrawTitleDetails(Canvas& c, const GameEntry& game, const TitleDetails& details, bool customised) {
    const int w = std::min(660, ContentW() - 48);
    constexpr int h = 420;
    const int x = kContentX + (ContentW() - w) / 2;
    const int y = kContentTop + (ContentBottom() - kContentTop - h) / 2;
    Skin::DrawModal(c, x, y, w, h);

    int ty = y + 22;
    g_font.Draw(c, x + 24, ty + 20, g_font.Truncate(game.title, 24, w - 48), 24, kColText);
    ty += 32;
    if (!game.publisher.empty()) {
        g_font.Draw(c, x + 24, ty + 18, g_font.Truncate(game.publisher, 18, w - 48), 18,
                    kColTextDim);
    }
    ty += 30;
    c.FillRect(x + 24, ty, w - 48, 1, kColRail);
    ty += 12;

    const auto row = [&](const char* label, const std::string& value, u32 color) {
        g_font.Draw(c, x + 24, ty + 18, label, 18, kColTextDim);
        g_font.Draw(c, x + 190, ty + 18, g_font.Truncate(value, 18, w - 214), 18, color);
        ty += 30;
    };

    row("Title ID",
        details.program_id == 0 ? std::string{"Unknown"} : FormatTitleId(details.program_id),
        kColText);
    row("Type", TitleKindName(details.kind), kColText);
    row("Source",
        game.installed ? "Installed on SD (" + game.file_type + ")"
                       : "ROM file (" + game.file_type + ")",
        kColText);
    row("Version",
        details.has_base_version ? FormatTitleVersion(details.base_version)
                                 : std::string{"Unknown (no TMD)"},
        details.has_base_version ? kColAccent : kColTextDim);
    row("Update",
        details.has_update ? FormatTitleVersion(details.update_version)
                           : std::string{"Not installed"},
        details.has_update ? kColAccent : kColTextDim);
    row("DLC",
        details.has_dlc ? std::to_string(details.dlc_contents) +
                              (details.dlc_contents == 1 ? " content" : " contents")
                        : std::string{"Not installed"},
        details.has_dlc ? kColAccent : kColTextDim);
    row("Settings", customised ? "Customised for this game" : "Global",
        customised ? kColAccent : kColTextDim);
    const bool inserted = game.insertable && GetInsertedCartridge() == game.path;
    if (game.insertable) {
        row("Cartridge", inserted ? "Inserted" : "Not inserted",
            inserted ? kColAccent : kColTextDim);
    }

    ty += 4;
    g_font.Draw(c, x + 24, ty + 16, g_font.TruncateFront(game.path, 16, w - 48), 16, kColTextDim);

    int hx = x + 24;
    const int hy = y + h - 38;
    if (game.insertable) {
        hx += DrawHint(c, hx, hy, "X", inserted ? "Eject Cartridge" : "Insert Cartridge") + 22;
    }
    if (game.program_id != 0) {
        hx += DrawHint(c, hx, hy, "Y", "Manage Saves") + 22;
        hx += DrawHint(c, hx, hy, "A", "Game Settings") + 22;
    }
    DrawHint(c, hx, hy, "B", "Close");
}

// A card of short pages flipped through with L/R. Carries the welcome tour on a first
// run and the release notes after an update.
struct InfoPage {
    std::string heading;
    std::vector<std::string> lines;
};

struct InfoCard {
    std::string title;
    std::vector<InfoPage> pages;
    int page = 0;
};

constexpr int kInfoBodySize = 18;
constexpr int kInfoLineH = 26;
constexpr int kInfoLines = 8;
constexpr int kInfoBodyTop = 112; // Baseline of the first body line.

constexpr std::string_view kKofiUrl = "ko-fi.com/palindromicbreadloaf";

int InfoCardW() {
    return std::min(720, g_screen_w - 48);
}

int InfoCardH() {
    return kInfoBodyTop + kInfoLines * kInfoLineH + 74;
}

int InfoCardTextW() {
    return InfoCardW() - 48;
}

std::vector<std::string> WrapText(const std::string& text, int max_w) {
    if (text.empty()) {
        return {std::string{}};
    }
    // Continuation lines of a bullet line up under its text rather than its dash.
    const std::string indent = text.rfind("- ", 0) == 0 ? "   " : "";
    std::vector<std::string> out;
    std::string line;
    std::size_t pos = 0;
    while (true) {
        const std::size_t space = text.find(' ', pos);
        const std::string word = text.substr(pos, space - pos);
        if (!word.empty()) {
            const std::string candidate = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && g_font.Measure(candidate, kInfoBodySize) > max_w) {
                out.push_back(std::move(line));
                line = indent + word;
            } else {
                line = candidate;
            }
        }
        if (space == std::string::npos) {
            break;
        }
        pos = space + 1;
    }
    if (!line.empty()) {
        out.push_back(std::move(line));
    }
    for (std::string& wrapped : out) {
        wrapped = g_font.Truncate(wrapped, kInfoBodySize, max_w);
    }
    return out;
}

InfoPage MakeInfoPage(std::string heading, std::initializer_list<std::string> body, int max_w) {
    InfoPage page{std::move(heading), {}};
    for (const std::string& text : body) {
        for (std::string& line : WrapText(text, max_w)) {
            page.lines.push_back(std::move(line));
        }
    }
    return page;
}

// Release bodies are written in Markdown
std::string FlattenMarkdown(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    std::size_t indent = 0;
    while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) {
        ++indent;
    }
    line.remove_prefix(indent);
    if (line.rfind("#", 0) == 0) {
        while (!line.empty() && line.front() == '#') {
            line.remove_prefix(1);
        }
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
    }

    std::string out;
    if (!line.empty() && (line.front() == '*' || line.front() == '-' || line.front() == '+') &&
        line.size() > 1 && line[1] == ' ') {
        out = "- ";
        line.remove_prefix(2);
    }
    for (std::size_t i = 0; i < line.size();) {
        if (line.compare(i, 2, "**") == 0 || line.compare(i, 2, "__") == 0 ||
            line.compare(i, 2, "~~") == 0) {
            i += 2;
            continue;
        }
        if (line[i] == '`' || line[i] == '*' || line[i] == '_') {
            ++i;
            continue;
        }
        // "[text](url)" keeps only the text.
        if (line[i] == '[') {
            const std::size_t close = line.find(']', i);
            const std::size_t open = close == std::string_view::npos ? close : close + 1;
            if (open != std::string_view::npos && open < line.size() && line[open] == '(') {
                const std::size_t end = line.find(')', open);
                if (end != std::string_view::npos) {
                    out.append(line.substr(i + 1, close - i - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push_back(line[i]);
        ++i;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out == "-" ? std::string{} : out;
}

std::vector<InfoPage> PaginateNotes(const std::string& notes, const std::string& heading,
                                    int max_w) {
    std::vector<std::string> lines;
    bool last_blank = true; // Drops the leading and repeated blank lines Markdown leaves behind.
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = notes.find('\n', pos);
        const std::string flat =
            FlattenMarkdown(std::string_view{notes}.substr(pos, newline - pos));
        if (flat.empty()) {
            if (!last_blank) {
                lines.emplace_back();
                last_blank = true;
            }
        } else {
            for (std::string& line : WrapText(flat, max_w)) {
                lines.push_back(std::move(line));
            }
            last_blank = false;
        }
        if (newline == std::string::npos) {
            break;
        }
        pos = newline + 1;
    }

    std::vector<InfoPage> pages;
    InfoPage current;
    for (std::string& line : lines) {
        if (current.lines.empty() && line.empty()) {
            continue;
        }
        current.lines.push_back(std::move(line));
        if (static_cast<int>(current.lines.size()) == kInfoLines) {
            pages.push_back(std::move(current));
            current = {};
        }
    }
    while (!current.lines.empty() && current.lines.back().empty()) {
        current.lines.pop_back();
    }
    if (!current.lines.empty()) {
        pages.push_back(std::move(current));
    }
    if (!pages.empty()) {
        pages.front().heading = heading;
    }
    return pages;
}

// Shown only to somebody who has already been running an earlier build.
InfoPage MakeSupportPage(int max_w) {
    return MakeInfoPage("Support Dekopon (3DS engine)",
                        {"Dekopon is free software written in my spare time.",
                         "If you are enjoying it, and able to, you can support its development on Ko-fi:", "",
                         std::string{kKofiUrl}, "", "Thank you."},
                        max_w);
}

InfoCard BuildWelcomeCard() {
    const int max_w = InfoCardTextW();
    const SwitchPaths& paths = GetPaths();
    InfoCard card;
    card.title = "Welcome to EmuSwitch";
    card.pages.push_back(MakeInfoPage(
        "Your games",
        {"EmuSwitch lists the games it finds in:", paths.roms_dir, "and in sdmc:/roms/<system>/ (3ds, ds, gba, gb, ps2, wiiu).", "",
         "- 3DS, CCI, CXI, 3DSX and APP files are recognised; CIA files install by themselves.",
         "- DS, GBA, Game Boy, NES, SNES, N64, PS1, PS2, PSP and Wii U games open in their own emulator.",
         "- PS2: drop your BIOS .zip or .bin into sdmc:/roms/ps2/.",
         "- The Paths tab moves that folder, adds a second one, and can scan subfolders."},
        max_w));
    card.pages.push_back(MakeInfoPage(
        "While a game runs",
        {"- Press + and - together to open the quick menu. Here you can access save states,"
         " cheats, amiibo, screen settings, and more.",
         "- Click the right stick to cycle through the screen layouts.",
         "- The touchscreen drives the bottom screen, and a stick or the gyro can drive it "
         "instead."},
        max_w));
    card.pages.push_back(MakeInfoPage(
        "Cheats, mods and textures",
        {"These live under " + paths.user_dir + ", keyed by Title ID:", "",
         "- cheats/<TITLE_ID>.txt", "- load/mods/<TITLE_ID>/", "- load/textures/<TITLE_ID>/", "",
         "Texture packs also need Custom Textures switched on in the quick/settings menu."},
        max_w));
    card.pages.push_back(MakeInfoPage(
        "Settings",
        {"- Graphics chooses the renderer, the resolution and the shader options.",
         "- Reset All Settings offers the Default, Performance and Ultra Performance presets."
         "Performance is recommended for most titles.",
         "- General holds the update channel, Check for Updates and the release notes.", "",
         "Press A to start."},
        max_w));
    return card;
}

// What the menu does with the result of an update check. Only Silent runs without a modal.
enum class UpdateCheckKind {
    Silent,
    Manual,
    Notes,
};

class Menu {
public:
    MenuResult Run(PadState& pad) {
        pad_state = &pad;
        LoadSystemOrder();
        LoadGameOrder();
        EnsureFramebuffer();
        ApplyRotation();
        // The loading screen keeps moving while the library is read.
        LoadLibrary();
        library_intro = NowSeconds();
        if (!g_auto_install_checked) {
            g_auto_install_checked = true;
            StartAutoInstall();
        }
        if (!g_covers_checked) {
            g_covers_checked = true;
            StartCoverDownload();
        }
        if (!g_auto_update_checked) {
            g_auto_update_checked = true;
            ShowStartupCard();
            BeginUpdateCheck(UpdateCheckKind::Silent);
        }
        while (appletMainLoop()) {
            padUpdate(&pad);
            ApplyRotation();
            const u64 down = padGetButtonsDown(&pad);
            held = padGetButtons(&pad);
            PumpUpdater();
            PumpInstall();
            PumpCovers();

            if (!install_active && ConsumeUsbStorageChange()) {
                HandleUsbStorageChange();
            }

            // +/- together exits the app, but not while a CIA is installing.
            constexpr u64 kExitChord = HidNpadButton_Plus | HidNpadButton_Minus;
            if (install_active && (held & kExitChord) == kExitChord && (down & kExitChord)) {
                NoticeInstallBusy("close EmuSwitch");
            }
            if (!install_active && !update_download_active && (held & kExitChord) == kExitChord) {
                if (remap_open) {
                    CloseRemap();
                }
                Flush();
                return {MenuAction::Exit, {}};
            }

            const HidAnalogStickState ls = padGetStickPos(&pad, 0);
            constexpr int dz = 12000;
            // Rotated separately so that the split below stays in menu space: under a rotated menu
            // a physical stick left is a menu up, and that still has to move the cursor.
            const MenuDirections dpad = RotateMenuDirections({
                .up = (down & HidNpadButton_Up) != 0,
                .down = (down & HidNpadButton_Down) != 0,
                .left = (down & HidNpadButton_Left) != 0,
                .right = (down & HidNpadButton_Right) != 0,
            });
            const MenuDirections stick = RotateMenuDirections({
                .up = ls.y > dz,
                .down = ls.y < -dz,
                .left = ls.x < -dz,
                .right = ls.x > dz,
            });
            // Rows that cycle a value take `dpad_nav`, so a stray nudge while scrolling with the
            // stick cannot change a setting.
            const u32 dpad_nav = NavMask(dpad);
            const u32 nav =
                dpad_nav | repeater.Step(stick.up, stick.down, stick.left, stick.right);

            MenuResult result;
            bool done = false;
            if (info_card) {
                HandleInfoCard(down, nav);
            } else if (update_installed) {
                if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus |
                            HidNpadButton_Minus)) {
                    QueueUpdatedRelaunch();
                    Flush();
                    return {MenuAction::Exit, {}};
                }
            } else if (update_download_active || UpdateModalOpen()) {
                // The worker is pumped above and its modal is drawn below.
            } else if (confirm) {
                HandleConfirm(down);
            } else if (preset_picker_open) {
                HandlePresetPicker(down, nav);
            } else if (country_picker_open) {
                HandleCountryPicker(down, nav);
            } else if (layout_picker_open) {
                HandleLayoutPicker(down, nav);
            } else if (remap_open) {
                HandleRemap(down, nav, dpad_nav);
            } else if (saves_open) {
                HandleSaves(down, nav);
            } else if (details_open) {
                const GameEntry& game = games[filtered[selected]];
                if ((down & HidNpadButton_X) && game.insertable) {
                    SetInsertedCartridge(GetInsertedCartridge() == game.path ? "" : game.path);
                }
                if ((down & HidNpadButton_Y) && game.program_id != 0) {
                    OpenSaves();
                } else if ((down & HidNpadButton_A) && game.program_id != 0) {
                    OpenPerGameSettings();
                } else if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) {
                    details_open = false;
                }
            } else if (PerGameOpen()) {
                HandleSettings(down, nav, dpad_nav);
            } else if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                // ZL / ZR step through the dock from anywhere.
                const int n = static_cast<int>(kRailItems.size());
                const int step = (down & HidNpadButton_ZR) ? 1 : n - 1;
                SetTab(kRailItems[(static_cast<int>(tab) + step) % n].first);
                rail_sel = tab;
                focus = Focus::Content;
            } else if (focus == Focus::Rail) {
                HandleRail(down, nav);
            } else if (tab == Tab::Library) {
                done = HandleLibrary(down, nav, result);
            } else if (tab == Tab::Systems) {
                done = HandleSystems(down, nav, result);
            } else if (tab == Tab::Settings) {
                done = HandleSettings(down, nav, dpad_nav);
            } else {
                done = HandlePaths(down, nav);
            }
            if (done && result.action == MenuAction::Launch && install_active) {
                // A game can't start while the emulated SD card is being written to.
                NoticeInstallBusy("play");
                done = false;
            }
            if (done) {
                return result;
            }

            if (!update_download_active && !update_installed &&
                !UpdateModalOpen() && !info_card && !details_open && !saves_open &&
                !layout_picker_open && !remap_open && !preset_picker_open && !country_picker_open &&
                !confirm && !PerGameOpen()) {
                HandleTouch();
            }
            if (pending_launch && install_active) {
                NoticeInstallBusy("play");
                pending_launch.reset();
            }
            if (pending_launch) {
                MenuResult launch{MenuAction::Launch, *pending_launch};
                pending_launch.reset();
                return launch;
            }

            Frame();
            if (g_notice_frames > 0) {
                --g_notice_frames;
            }
        }
        if (remap_open) {
            CloseRemap();
        }
        Flush();
        return {MenuAction::Exit, {}};
    }

    // Releasing the framebuffer hands the nwindow back so the emulator's renderer can claim it for the launched game.
    ~Menu() {
        // Only reachable with a worker still running if appletMainLoop() bowed out mid-install.
        if (install_thread.joinable()) {
            install_thread.join();
        }
        updater_cancel = true;
        if (updater_thread.joinable()) {
            updater_thread.join();
        }
        if (fb_ready) {
            PresentBlack(fb);
            framebufferClose(&fb);
        }
    }

private:
    Tab tab{Tab::Library};
    Focus focus{Focus::Content};
    Tab rail_sel{Tab::Library}; // Highlighted rail item while focused.
    std::vector<GameEntry> games;
    std::vector<int> filtered; // Indices into `games` after search filtering.
    int selected = 0;          // Index into `filtered`.
    // Home: one section per console, in the Systems page's order; each a header and rows of tiles.
    struct HomeSection {
        int system; // Multi::Systems() index, -1 for the 3DS
        int first;  // index into `filtered`
        int count;
        float y;    // top of its header, in pixels from the top of the list
    };
    struct HomeRow {
        int section;
        int first; // index into `filtered`
        int count;
        float y; // top of its tiles, in pixels from the top of the list
    };
    std::vector<HomeSection> home_sections;
    std::vector<HomeRow> home_rows;
    std::vector<int> home_row_of; // `filtered` index -> row
    float home_height = 0.0f;
    int home_cols = 0;
    float home_top = 0.0f; // where the list is scrolled to, in pixels
    int paths_sel = 0;
    std::string search;
    SwitchPaths paths{};

    // Settings tab.
    Category settings_page{Category::General};
    // Settings > Themes: the theme under the cursor and the first card row on screen.
    int theme_sel = 0;
    int theme_scroll = 0;
    std::vector<SettingsRow> settings_rows;
    // Kept per page so switching back lands where the cursor was left.
    std::array<int, NumCategories> settings_sel{};
    std::array<int, NumCategories> settings_scroll{};
    bool settings_search_open = false;
    std::string settings_search_query;
    int settings_search_sel = 0;
    int settings_search_scroll = 0;

    std::uint64_t per_game_id = 0;
    std::string per_game_title;
    bool per_game_dirty = false;
    Category per_game_return_page{Category::General};
    std::array<int, NumCategories> per_game_sel{};
    std::array<int, NumCategories> per_game_scroll{};

    Repeater repeater;
    Framebuffer fb{};
    PadState* pad_state = nullptr;
    u64 held = 0; // This frame's held buttons.
    bool fb_ready = false;
    bool settings_dirty = false; // Edited settings not yet written to config.ini.
    bool paths_dirty = false;
    // Whether the second ROM folder's device is attached. Sampled rather than stat'd per frame.
    bool roms_dir_2_present = false;

    // Library detail panel.
    bool details_open = false;
    TitleDetails details{};
    bool details_customised = false; // Sampled when the panel opens: it's a file check.

    // Animation and per-frame state; only PrepareFrame() and the input handlers write these.
    bool frame_started = false;
    std::string header_title, header_sub, header_prev_title, header_prev_sub;
    double header_changed = -10.0;
    double status_sampled = -1.0;
    std::string clock_text, ampm_text;
    int battery = -1;
    bool charging = false;
    Spring dock_slide;
    float dock_label = 0.0f;
    float page_anim = 1.0f; // 0 -> 1 as a page slides in
    float page_dir = 0.0f;  // -1 from the left, +1 from the right
    int modal_kind = 0;
    float modal_anim = 1.0f;
    Spring grid_scroll;     // pixels, eased towards home_top
    Spring cursor_x, cursor_y;
    bool cursor_ready = false;
    Spring lift;            // how far the focused tile has popped up
    int lift_index = -1;
    int lift_prev_index = -1;
    float lift_prev = 0.0f; // the tile the cursor just left, settling back
    double library_intro = -10.0;
    Spring systems_spring;
    float systems_accent[3] = {226, 27, 51};

    // Save manager.
    bool saves_open = false;
    SaveKind saves_kind{SaveKind::SaveData};
    std::uint64_t saves_extdata_id = 0;
    std::vector<SaveBackup> saves_backups;
    bool saves_present = false; // Whether the emulated card holds a save of the current kind.
    int saves_sel = 0;
    int saves_scroll = 0;

    // R3 screen-layout picker.
    bool layout_picker_open = false;
    int layout_picker_sel = 0;

    // Preset picker the reset row opens.
    bool preset_picker_open = false;
    int preset_sel = 0;

    bool country_picker_open = false;
    int country_sel = 0;
    int country_scroll = 0;

    // Controller remapping page.
    bool remap_open = false;
    int remap_sel = 0;
    int remap_scroll = 0;

    // Confirmation for the settings rows that destroy something.
    struct ConfirmPrompt {
        std::string title;
        std::vector<std::string> lines;
        std::string note;
        const char* accept;
        std::function<void()> on_accept;
        std::function<void()> on_cancel;
    };
    std::optional<ConfirmPrompt> confirm;

    // CIA files found in the ROM folders install themselves in the background, one at a time.
    std::thread install_thread;
    std::atomic<bool> install_done{false};
    std::atomic<std::size_t> install_written{0};
    std::atomic<std::size_t> install_total{0};
    std::atomic<int> install_index{0}; // 1-based; 0 while the CIAs are still being checked
    std::atomic<int> install_count{0};
    std::mutex install_mutex; // guards the name and the results below
    std::string install_name;
    int install_ok = 0;
    std::string install_last_ok;
    std::vector<std::pair<std::string, std::string>> install_failures; // path, message
    bool install_active = false;
    double covers_polled = 0.0;
    std::set<std::string> install_skip; // CIAs that failed this session aren't tried again
    // What the progress pill shows, sampled in PrepareFrame().
    std::string install_label;
    float install_frac = 0.0f;
    float install_pill = 0.0f; // 0..1, fades the pill in and out

    // GitHub update checker/downloader. Only one updater worker is active at a time.
    std::thread updater_thread;
    std::atomic<bool> updater_done{false};
    // The silent startup check does not block launching a game, so the menu can be torn down with
    // it still in flight. Without this the join below would wait out the GitHub request timeouts.
    std::atomic<bool> updater_cancel{false};
    bool update_check_active = false;
    UpdateCheckKind update_check_kind = UpdateCheckKind::Silent;
    bool update_download_active = false;
    bool update_installed = false;
    std::atomic<std::uint64_t> update_downloaded{0};
    std::atomic<std::uint64_t> update_total{0};
    UpdateCheckResult update_check_result{};
    UpdateInstallResult update_install_result{};
    UpdateRelease update_release{};

    std::optional<InfoCard> info_card;
    std::string update_from_version;
    // Set when the What's New card went up without cached notes, so the startup check can fill
    // them in behind it.
    bool info_card_notes_pending = false;

    void Rescan() {
        games = ScanGames();
        Art::LoadGameArt(games);
        paths = GetPaths();
        RefreshRomsDir2Presence();
        ApplyFilter();
    }

    // A drive appearing or leaving changes what the second ROM folder holds, and the library has
    // to follow it. Path edits in progress are left alone, since Rescan() would drop them.
    void HandleUsbStorageChange() {
        const bool was_present = roms_dir_2_present;
        RefreshRomsDir2Presence();

        const std::vector<UsbVolume> volumes = GetUsbVolumes();
        if (volumes.empty()) {
            ShowNotice("USB storage disconnected", false);
        } else if (volumes.size() == 1) {
            ShowNotice(volumes.front().label + " mounted as " + volumes.front().root, false);
        } else {
            ShowNotice(std::to_string(volumes.size()) + " USB volumes mounted", false);
        }

        if (roms_dir_2_present != was_present && !paths.roms_dir_2.empty() && !paths_dirty) {
            ShowBusy("Refreshing library...");
            Rescan();
        }
    }

    // Switch tabs persisting any pending edits when leaving an editing page so a disk write
    // happens once per editing session rather than once per adjustment.
    void SetTab(Tab next) {
        if (tab == Tab::Settings && next != Tab::Settings) {
            FlushSettings();
        }
        if (next != tab) {
            // Menus close and anything being carried goes back where it was.
            game_menu = GameMenu::None;
            if (game_moving) {
                CancelGameMove();
            }
            system_menu_open = false;
            if (system_moving) {
                CancelSystemMove();
            }
        }
        if (tab == Tab::Paths && next != Tab::Paths) {
            // The library on screen came from the old directory so it must be re-read.
            const bool stale = ScanInputsChanged();
            FlushPaths();
            if (stale) {
                ShowBusy("Refreshing library...");
                search.clear();
                Rescan();
                StartAutoInstall();
            }
        }
        if (next != tab) {
            page_dir = static_cast<int>(next) > static_cast<int>(tab) ? 1.0f : -1.0f;
            page_anim = 0.0f;
            if (next == Tab::Library) {
                library_intro = NowSeconds();
            }
        }
        tab = next;
        if (tab == Tab::Paths) {
            RefreshRomsDir2Presence();
        }
        if (tab == Tab::Settings) {
            RefreshShaderCacheSize();
            SetSettingsPage(settings_page);
        }
    }

    bool PerGameOpen() const {
        return per_game_id != 0;
    }

    static Category FirstOverridablePage() {
        for (int i = 0; i < NumCategories; ++i) {
            const auto page = static_cast<Category>(i);
            if (CategoryHasOverridables(page)) {
                return page;
            }
        }
        return Category::General;
    }

    void SetSettingsPage(Category page) {
        settings_search_open = false;
        RefreshUniqueDataStatus();
        if (page == Category::Themes && settings_page != Category::Themes) {
            theme_sel = GetMenuTheme();
            theme_scroll = 0;
        }
        settings_page = page;
        settings_rows = PerGameOpen() ? BuildGameCategoryRows(page) : BuildCategoryRows(page);
        SettingsSel() = SettleSettingsSelection(SettingsSel(), +1);
        ScrollSettingsIntoView();
    }

    void OpenSettingsSearch() {
        const auto text = PromptSettingText("Search settings", "Name of a setting",
                                            settings_search_query, 40);
        if (!text) {
            return;
        }
        settings_search_query = *text;
        settings_rows = PerGameOpen() ? BuildGameSearchRows(settings_search_query)
                                      : BuildSearchRows(settings_search_query);
        settings_search_open = true;
        settings_search_sel = 0;
        settings_search_scroll = 0;
        if (settings_rows.empty()) {
            ShowNotice("No setting matches \"" + settings_search_query + "\"", true);
            CloseSettingsSearch();
        }
    }

    void CloseSettingsSearch() {
        SetSettingsPage(settings_page);
    }

    // Lists the page again after Show All Settings flipped. The row under the cursor keeps its
    // place on screen if it is still listed, otherwise the nearest listed row above it does.
    void RelistSettingsPage() {
        const std::vector<SettingsRow> before = std::move(settings_rows);
        const int old_sel =
            std::clamp(SettingsSel(), 0, std::max(0, static_cast<int>(before.size()) - 1));
        const int old_offset = old_sel - SettingsScroll();
        settings_rows = BuildCategoryRows(settings_page);
        // The other pages' cursors point into lists that just changed, so they start at the top.
        for (int i = 0; i < NumCategories; ++i) {
            if (i != static_cast<int>(settings_page)) {
                settings_sel[i] = 0;
                settings_scroll[i] = 0;
            }
        }
        int sel = 0;
        for (int i = old_sel; i >= 0 && i < static_cast<int>(before.size()); --i) {
            const auto it = std::find_if(
                settings_rows.begin(), settings_rows.end(), [&](const SettingsRow& row) {
                    return !row.is_header && !before[i].is_header && row.label == before[i].label;
                });
            if (it != settings_rows.end()) {
                sel = static_cast<int>(it - settings_rows.begin());
                break;
            }
        }
        SettingsSel() = SettleSettingsSelection(sel, +1);
        const int count = static_cast<int>(settings_rows.size());
        SettingsScroll() =
            std::clamp(SettingsSel() - old_offset, 0, std::max(0, count - SettingsVisibleRows()));
        ScrollSettingsIntoView();
    }

    void ToggleShowAllSettings() {
        const bool on = !IsShowAllSettingsEnabled();
        SetShowAllSettingsEnabled(on);
        settings_dirty = true;
        RelistSettingsPage();
        ShowNotice(on ? "Showing every setting" : "Showing the main settings", false);
    }

    int& SettingsSel() {
        if (settings_search_open) {
            return settings_search_sel;
        }
        auto& cursors = PerGameOpen() ? per_game_sel : settings_sel;
        return cursors[static_cast<std::size_t>(settings_page)];
    }

    int& SettingsScroll() {
        if (settings_search_open) {
            return settings_search_scroll;
        }
        auto& scrolls = PerGameOpen() ? per_game_scroll : settings_scroll;
        return scrolls[static_cast<std::size_t>(settings_page)];
    }

    int SettleSettingsSelection(int index, int dir) const {
        const int count = static_cast<int>(settings_rows.size());
        if (count == 0) {
            return 0;
        }
        index = std::clamp(index, 0, count - 1);
        for (int i = index; i >= 0 && i < count; i += dir) {
            if (!settings_rows[i].is_header) {
                return i;
            }
        }
        for (int i = index; i >= 0 && i < count; i -= dir) {
            if (!settings_rows[i].is_header) {
                return i;
            }
        }
        return index;
    }

    // True while the edited scan inputs differ from what the last scan used.
    bool ScanInputsChanged() const {
        const SwitchPaths& live = GetPaths();
        return paths.roms_dir != live.roms_dir || paths.roms_dir_2 != live.roms_dir_2 ||
               paths.scan_recursive != live.scan_recursive;
    }

    // The dekopon directory only moves on the next launch.
    bool RestartPending() const {
        return paths.user_dir != GetActiveUserDir();
    }

    void Flush() {
        if (PerGameOpen()) {
            ClosePerGameSettings();
        }
        FlushSettings();
        FlushPaths();
    }

    void FlushSettings() {
        if (settings_dirty) {
            CommitSettings();
            settings_dirty = false;
        }
    }

    void FlushPaths() {
        if (paths_dirty) {
            SetPaths(paths);
            paths_dirty = false;
        }
    }

    void ApplyFilter() {
        filtered.clear();
        const std::string needle = ToLowerAscii(search);
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (needle.empty() || ToLowerAscii(games[i].title).find(needle) != std::string::npos) {
                filtered.push_back(i);
            }
        }
        // Grouped by console in the player's order; within each, the games in the player's
        // order (Move Placement), then the rest by title.
        std::stable_sort(filtered.begin(), filtered.end(), [this](int a, int b) {
            const int sa = SystemRank(games[static_cast<std::size_t>(a)].system);
            const int sb = SystemRank(games[static_cast<std::size_t>(b)].system);
            return sa != sb ? sa < sb : GameRank(a) < GameRank(b);
        });
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(filtered.size()) - 1));
        BuildHomeLayout();
        home_top = 0.0f;
        grid_scroll.Snap(0.0f);
        cursor_ready = false;
    }

    static constexpr int kSectionGap = 24; // between a section's last row and the next header

    static int HomeViewTop() {
        return kContentTop + 4;
    }
    static int HomeViewBottom() {
        return g_screen_h - kHintH - 40;
    }
    static float HomeViewH() {
        return static_cast<float>(HomeViewBottom() - HomeViewTop());
    }

    void BuildHomeLayout() {
        const Grid grid = ComputeGrid();
        home_cols = grid.cols;
        home_sections.clear();
        home_rows.clear();
        home_row_of.assign(filtered.size(), 0);
        const int step = kTileH + kTileGap;
        const int n = static_cast<int>(filtered.size());
        float y = 0.0f;
        for (int i = 0; i < n;) {
            const int sys = games[static_cast<std::size_t>(filtered[static_cast<std::size_t>(i)])].system;
            int j = i;
            while (j < n && games[static_cast<std::size_t>(filtered[static_cast<std::size_t>(j)])].system == sys) {
                ++j;
            }
            home_sections.push_back({sys, i, j - i, y});
            y += Skin::kSectionHeaderH;
            for (int k = i; k < j; k += grid.cols) {
                const int cnt = std::min(grid.cols, j - k);
                for (int m = k; m < k + cnt; ++m) {
                    home_row_of[static_cast<std::size_t>(m)] = static_cast<int>(home_rows.size());
                }
                home_rows.push_back({static_cast<int>(home_sections.size()) - 1, k, cnt, y});
                y += step;
            }
            y += kSectionGap - kTileGap;
            i = j;
        }
        home_height = home_rows.empty() ? 0.0f : home_rows.back().y + kTileH;
    }

    // The tile in the row above (dir -1) or below (+1) `index`, in the same column where it can.
    int RowNeighbour(int index, int dir) const {
        if (index < 0 || index >= static_cast<int>(home_row_of.size())) {
            return index;
        }
        const int r = home_row_of[static_cast<std::size_t>(index)];
        const int nr = r + dir;
        if (nr < 0 || nr >= static_cast<int>(home_rows.size())) {
            return index;
        }
        const int col = index - home_rows[static_cast<std::size_t>(r)].first;
        const HomeRow& next = home_rows[static_cast<std::size_t>(nr)];
        return next.first + std::min(col, next.count - 1);
    }

    // Where tile `index` sits on screen with the list scrolled to `scroll`.
    void HomeTilePos(const Grid& grid, int index, float scroll, float& x, float& y) const {
        const HomeRow& row = home_rows[static_cast<std::size_t>(home_row_of[static_cast<std::size_t>(index)])];
        x = static_cast<float>(grid.start_x + (index - row.first) * (kTileW + kTileGap));
        y = HomeViewTop() + row.y - scroll;
    }

    // Returns true if the menu should return `result` to the caller.
    bool HandleLibrary(u64 down, u32 nav, MenuResult& result) {
        const Grid grid = ComputeGrid();
        const int count = static_cast<int>(filtered.size());
        if (game_menu != GameMenu::None) {
            HandleGameMenu(down, nav);
            return false;
        }
        if (game_moving) {
            HandleGameMove(down, nav);
            EnsureVisible(grid);
            return false;
        }
        if (count > 0) {
            if (nav & DirLeft) {
                selected = std::max(0, selected - 1);
            }
            if (nav & DirRight) {
                selected = std::min(count - 1, selected + 1);
            }
            if (nav & DirUp) {
                selected = RowNeighbour(selected, -1);
            }
            if (nav & DirDown) {
                selected = RowNeighbour(selected, +1);
            }
            if (down & HidNpadButton_A) {
                result = {MenuAction::Launch, games[filtered[selected]].path};
                return true;
            }
            // Guarded so that reaching for the +/- exit combo doesn't flash the menu open.
            if ((down & HidNpadButton_Plus) && !(held & HidNpadButton_Minus)) {
                OpenGameMenu(GameMenu::Actions);
                return false;
            }
        }
        if (count > 0 && (down & HidNpadButton_L) && IsPictureEditingEnabled()) {
            OpenGameMenu(GameMenu::Picture);
            return false;
        }
        if (count > 0 && (down & HidNpadButton_R) && IsPictureEditingEnabled() &&
            Skin::HasGameImage(games[filtered[selected]].path)) {
            const std::string err = Art::SetGameArt(games[filtered[selected]], "");
            ShowNotice(err.empty() ? "Picture removed" : err, !err.empty());
        }
        if (down & HidNpadButton_X) {
            search = PromptSearch(search);
            ApplyFilter();
        }
        if (down & HidNpadButton_Y) {
            // Rescanning the SD card blocks
            ShowBusy("Refreshing library…");
            search.clear();
            Rescan();
            StartAutoInstall();
            StartCoverDownload();
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        EnsureVisible(grid);
        return false;
    }

    void OpenSaves() {
        ShowBusy("Reading saves...");
        saves_extdata_id = GetExtDataId(games[filtered[selected]]);
        saves_kind = SaveKind::SaveData;
        saves_sel = 0;
        saves_scroll = 0;
        RefreshSaves();
        details_open = false;
        saves_open = true;
    }

    void RefreshSaves() {
        const GameEntry& game = games[filtered[selected]];
        saves_backups = ListBackups(game, saves_kind, saves_extdata_id);
        saves_present = HasSaveData(game, saves_kind, saves_extdata_id);
        saves_sel =
            std::clamp(saves_sel, 0, std::max(0, static_cast<int>(saves_backups.size()) - 1));
    }

    void HandleSaves(u64 down, u32 nav) {
        const int count = static_cast<int>(saves_backups.size());
        if (nav & DirUp) {
            saves_sel = std::max(0, saves_sel - 1);
        }
        if (nav & DirDown) {
            saves_sel = std::min(std::max(0, count - 1), saves_sel + 1);
        }
        // A title without extdata has nothing to switch to.
        if ((nav & (DirLeft | DirRight)) && saves_extdata_id != 0) {
            saves_kind =
                saves_kind == SaveKind::SaveData ? SaveKind::ExtData : SaveKind::SaveData;
            saves_sel = 0;
            saves_scroll = 0;
            RefreshSaves();
        }
        saves_scroll = std::clamp(saves_scroll, std::max(0, saves_sel - kSavesRows + 1),
                                  std::max(0, std::min(saves_sel, count - kSavesRows)));
        if (down & HidNpadButton_X) {
            PromptBackup();
        }
        if ((down & HidNpadButton_A) && saves_sel < count) {
            OpenRestoreConfirm(saves_backups[saves_sel]);
        }
        if ((down & HidNpadButton_Y) && saves_sel < count) {
            OpenDeleteBackupConfirm(saves_backups[saves_sel]);
        }
        if (down & HidNpadButton_B) {
            saves_open = false;
            details_open = true;
        }
    }

    void PromptBackup() {
        const std::optional<std::string> entered =
            PromptSettingText("Backup name", "Name for this backup", DefaultBackupName(), 64);
        if (!entered) {
            return;
        }
        const std::string name = SanitizeBackupName(*entered);
        if (name.empty()) {
            ShowNotice("That name can't be used", true);
            return;
        }
        const bool exists = std::any_of(saves_backups.begin(), saves_backups.end(),
                                        [&name](const SaveBackup& b) { return b.name == name; });
        if (exists) {
            confirm = ConfirmPrompt{"Overwrite backup",
                                    {"\"" + name + "\" already exists.", "It will be replaced."},
                                    {},
                                    "Overwrite",
                                    [this, name] { RunExport(name); },
                                    {}};
            return;
        }
        RunExport(name);
    }

    void RunExport(const std::string& name) {
        ShowBusy("Backing up...");
        const SaveResult result =
            ExportSave(games[filtered[selected]], saves_kind, saves_extdata_id, name);
        if (result == SaveResult::Success) {
            saves_sel = 0;
            saves_scroll = 0;
            RefreshSaves();
            ShowNotice("Backed up as " + name, false);
        } else {
            ShowNotice(SaveResultText(result), true);
        }
    }

    void OpenRestoreConfirm(const SaveBackup& backup) {
        confirm = ConfirmPrompt{
            "Restore backup",
            {std::string{SaveKindName(saves_kind)} + " will be replaced with",
             "\"" + backup.name + "\"."},
            "The save currently on the card is deleted.",
            "Restore",
            [this, backup] { RunRestore(backup); },
            {}};
    }

    void RunRestore(const SaveBackup& backup) {
        ShowBusy("Restoring...");
        const SaveResult result =
            ImportSave(games[filtered[selected]], saves_kind, saves_extdata_id, backup);
        RefreshSaves();
        ShowNotice(result == SaveResult::Success ? "Restored " + backup.name
                                                 : std::string{SaveResultText(result)},
                   result != SaveResult::Success);
    }

    void OpenDeleteBackupConfirm(const SaveBackup& backup) {
        confirm = ConfirmPrompt{"Delete backup",
                                {"\"" + backup.name + "\" will be deleted."},
                                {},
                                "Delete",
                                [this, backup] { RunDeleteBackup(backup); },
                                {}};
    }

    void RunDeleteBackup(const SaveBackup& backup) {
        ShowBusy("Deleting...");
        const bool ok = DeleteBackup(backup);
        RefreshSaves();
        ShowNotice(ok ? "Deleted " + backup.name : std::string{"Could not delete that backup"},
                   !ok);
    }

    // Installs, in the background and one at a time, the CIA files the last scan found in the
    // ROM folders that aren't installed yet. Games show up in the library as they're done.
    void StartAutoInstall() {
        if (install_active) {
            return;
        }
        std::vector<std::string> cias;
        for (std::string& path : ScannedCiaFiles()) {
            if (!install_skip.count(path)) {
                cias.push_back(std::move(path));
            }
        }
        if (cias.empty()) {
            return;
        }
        install_active = true;
        install_done = false;
        install_index = 0;
        install_count = 0;
        install_written = 0;
        install_total = 0;
        {
            std::lock_guard lock{install_mutex};
            install_name.clear();
            install_ok = 0;
            install_last_ok.clear();
            install_failures.clear();
        }
        install_thread = std::thread([this, cias = std::move(cias)] {
            const std::vector<CiaEntry> todo = CiasToInstall(cias);
            install_count = static_cast<int>(todo.size());
            for (std::size_t i = 0; i < todo.size(); ++i) {
                const CiaEntry& cia = todo[i];
                std::string name = cia.name;
                if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
                    name.resize(dot);
                }
                {
                    std::lock_guard lock{install_mutex};
                    install_name = name;
                }
                install_written = 0;
                install_total = cia.size;
                install_index = static_cast<int>(i) + 1;
                InstallResult result = InstallResult::Invalid;
                if (cia.readable) {
                    const Common::Horizon::CpuBoostScope boost;
                    result = InstallCia(cia.path, [this](std::size_t written, std::size_t total) {
                        install_written = written;
                        install_total = total;
                    });
                }
                std::lock_guard lock{install_mutex};
                if (result == InstallResult::Success) {
                    ++install_ok;
                    install_last_ok = name;
                } else {
                    install_failures.emplace_back(cia.path, name + ": " + InstallResultText(result));
                }
            }
            install_done = true;
        });
    }

    void PumpInstall() {
        if (!install_active || !install_done) {
            return;
        }
        install_thread.join();
        install_active = false;
        int ok = 0;
        std::string last_ok;
        std::vector<std::pair<std::string, std::string>> failures;
        {
            std::lock_guard lock{install_mutex};
            ok = install_ok;
            last_ok = install_last_ok;
            failures.swap(install_failures);
        }
        for (const auto& failure : failures) {
            install_skip.insert(failure.first);
        }
        if (ok > 0) {
            // The new titles join the library.
            Rescan();
        }
        if (!failures.empty()) {
            std::string text = failures.front().second;
            if (failures.size() > 1) {
                text += " (and " + std::to_string(failures.size() - 1) + " more)";
            }
            ShowNotice("Couldn't install " + text, true);
        } else if (ok > 0) {
            ShowNotice(ok == 1 ? last_ok + " installed" : std::to_string(ok) + " games installed", false);
        }
    }

    // What's being installed, for the notices.
    std::string InstallingName() {
        std::lock_guard lock{install_mutex};
        return install_name.empty() ? std::string{"a game"} : install_name;
    }

    void NoticeInstallBusy(const char* what) {
        ShowNotice("Installing " + InstallingName() + " - you can " + what + " once it's done", true);
    }

    // Box art for the games without a picture, downloaded in the background; tiles pick the
    // covers up as they arrive.
    void StartCoverDownload() {
        if (!IsCoverDownloadEnabled() || Covers::IsRunning()) {
            return;
        }
        std::vector<Covers::Request> requests;
        for (const GameEntry& game : games) {
            Covers::Request request;
            if (Covers::RequestFor(game, request)) {
                requests.push_back(std::move(request));
            }
        }
        Covers::Start(std::move(requests));
    }

    void PumpCovers() {
        const double now = NowSeconds();
        if (now - covers_polled < 2.0) {
            return;
        }
        covers_polled = now;
        if (Covers::TakeUpdates()) {
            Art::LoadGameArt(games);
        }
        if (const int added = Covers::TakeFinishedCount(); added > 0) {
            ShowNotice(std::to_string(added) + (added == 1 ? " box art picture added" : " box art pictures added"),
                       false);
        }
    }

    // Move the cursor out to the Library/Settings rail
    void EnterRail() {
        focus = Focus::Rail;
        rail_sel = tab;
    }

    void HandleRail(u64 down, u32 nav) {
        int index = static_cast<int>(rail_sel);
        if (nav & DirLeft) {
            index = std::max(0, index - 1);
        }
        if (nav & DirRight) {
            index = std::min(static_cast<int>(kRailItems.size()) - 1, index + 1);
        }
        rail_sel = kRailItems[index].first;
        if (down & HidNpadButton_A) {
            SetTab(rail_sel);
            focus = Focus::Content;
        }
        // B cancels back into the section left.
        if (down & HidNpadButton_B) {
            rail_sel = tab;
            focus = Focus::Content;
        }
    }

    // Keeps the selected settings row inside the visible window.
    void ScrollSettingsIntoView() {
        int& scroll = SettingsScroll();
        const int sel = SettingsSel();
        const int top = sel > 0 && settings_rows[sel - 1].is_header ? sel - 1 : sel;
        scroll = std::clamp(scroll, std::max(0, sel - SettingsVisibleRows() + 1), top);
    }

    void OpenLayoutPicker() {
        layout_picker_sel = 0;
        layout_picker_open = true;
    }

    void StepSettingsPage(int dir) {
        int index = static_cast<int>(settings_page);
        for (int i = 0; i < NumCategories; ++i) {
            index = (index + dir + NumCategories) % NumCategories;
            const auto page = static_cast<Category>(index);
            if (!PerGameOpen() || CategoryHasOverridables(page)) {
                SetSettingsPage(page);
                return;
            }
        }
    }

    void OpenSettingsModal(SettingsModal modal) {
        switch (modal) {
        case SettingsModal::LayoutCycle:
            OpenLayoutPicker();
            break;
        case SettingsModal::ControllerMap:
            OpenRemap();
            break;
        case SettingsModal::LogFilter:
            if (const auto text = PromptSettingText("Log filter", "e.g. *:Info Render:Debug",
                                                    GetLogFilter(), 255)) {
                SetLogFilter(*text);
                settings_dirty = true;
            }
            break;
        case SettingsModal::ResetDefaults:
            OpenPresetPicker();
            break;
        case SettingsModal::ClearShaderCache:
            OpenShaderCacheConfirm();
            break;
        case SettingsModal::CheckForUpdates:
            BeginUpdateCheck(UpdateCheckKind::Manual);
            break;
        case SettingsModal::ReleaseNotes:
            OpenReleaseNotes();
            break;
        case SettingsModal::Username:
            if (const auto text = PromptSettingText("Username", "Name shown to other consoles",
                                                    GetProfileUsername(), 10)) {
                SetProfileUsername(*text);
                settings_dirty = true;
            }
            break;
        case SettingsModal::Country:
            OpenCountryPicker();
            break;
        case SettingsModal::FixedClock:
            if (const auto text = PromptSettingText("Fixed clock time", "YYYY-MM-DD HH:MM:SS",
                                                    GetFixedClockText(), 19)) {
                if (SetFixedClockText(*text)) {
                    settings_dirty = true;
                } else {
                    ShowNotice("Enter the time as YYYY-MM-DD HH:MM:SS", true);
                }
            }
            break;
        case SettingsModal::InitTicksValue:
            if (const auto text = PromptSettingText("Initial ticks", "CPU tick count",
                                                    GetInitTicksText(), 20)) {
                SetInitTicksText(*text);
                settings_dirty = true;
            }
            break;
        case SettingsModal::ConsoleId:
            OpenConsoleIdConfirm();
            break;
        case SettingsModal::MacAddress:
            OpenMacConfirm();
            break;
        case SettingsModal::UnlinkConsole:
            OpenUnlinkConfirm();
            break;
        case SettingsModal::InstallSecureInfo:
        case SettingsModal::InstallFriendCodeSeed:
        case SettingsModal::InstallOtp:
        case SettingsModal::InstallMovable:
            InstallUniqueData(static_cast<UniqueDataFile>(
                static_cast<int>(modal) - static_cast<int>(SettingsModal::InstallSecureInfo)));
            break;
        case SettingsModal::SteamGridDbKey:
            if (const auto text = PromptSettingText("SteamGridDB API key",
                                                    "From steamgriddb.com > Preferences > API",
                                                    GetSteamGridDbKey(), 64)) {
                // Spaces and line breaks sneak in when a key is pasted; a key never has them.
                std::string key;
                for (const char ch : *text) {
                    if (!std::isspace(static_cast<unsigned char>(ch))) {
                        key += ch;
                    }
                }
                SetSteamGridDbKey(key);
                SaveConfig();
                ShowNotice(key.empty() ? "SteamGridDB key removed" : "SteamGridDB key saved", false);
            }
            break;
        default:
            break;
        }
    }

    void OpenCountryPicker() {
        const std::vector<CountryOption>& options = CountryOptions();
        const int current = GetProfileCountry();
        country_sel = 0;
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (options[i].code == current) {
                country_sel = i;
                break;
            }
        }
        country_scroll = 0;
        country_picker_open = true;
        ScrollCountryIntoView();
    }

    void ScrollCountryIntoView() {
        const int count = static_cast<int>(CountryOptions().size());
        country_scroll = std::clamp(country_scroll, std::max(0, country_sel - kCountryRows + 1),
                                    std::max(0, std::min(country_sel, count - kCountryRows)));
    }

    void HandleCountryPicker(u64 down, u32 nav) {
        const int count = static_cast<int>(CountryOptions().size());
        if (nav & DirUp) {
            country_sel = (country_sel - 1 + count) % count;
        }
        if (nav & DirDown) {
            country_sel = (country_sel + 1) % count;
        }
        // A long list is worth paging through with the shoulders.
        if (down & HidNpadButton_L) {
            country_sel = std::max(0, country_sel - kCountryRows);
        }
        if (down & HidNpadButton_R) {
            country_sel = std::min(count - 1, country_sel + kCountryRows);
        }
        ScrollCountryIntoView();
        if (down & HidNpadButton_A) {
            SetProfileCountry(CountryOptions()[country_sel].code);
            settings_dirty = true;
            country_picker_open = false;
        }
        if (down & HidNpadButton_B) {
            country_picker_open = false;
        }
    }

    void OpenConsoleIdConfirm() {
        confirm = ConfirmPrompt{
            "Generate a new console ID?",
            {"The current virtual console ID is replaced and cannot be",
             "recovered. Some applications react badly to the change."},
            "This can fail on an outdated config save.",
            "Generate",
            [this] {
                RegenerateConsoleId();
                ShowNotice("Console ID regenerated", false);
            }};
    }

    void OpenMacConfirm() {
        confirm = ConfirmPrompt{
            "Generate a new MAC address?",
            {"The current MAC address is replaced with a random one."},
            "Keep the old one if you took it from your own console.",
            "Generate",
            [this] {
                RegenerateMacAddress();
                ShowNotice("MAC address regenerated", false);
            }};
    }

    void OpenUnlinkConfirm() {
        if (!IsConsoleLinked()) {
            ShowNotice("No console is linked", true);
            return;
        }
        confirm = ConfirmPrompt{
            "Unlink this console?",
            {"The OTP, SecureInfo and LocalFriendCodeSeed are removed, your",
             "friend list resets and you are logged out of your NNID/PNID.",
             "System and eShop titles stay locked until you link it again."},
            "Save data is not touched.",
            "Unlink",
            [this] {
                UnlinkConsole();
                SetSettingsPage(settings_page);
                ShowNotice("Console unlinked", false);
            }};
    }

    void InstallUniqueData(UniqueDataFile file) {
        const std::string name{UniqueDataFileName(file)};
        // movable.sed stands apart from the three files that together make a console linked.
        if (file != UniqueDataFile::Movable && IsConsoleLinked()) {
            ShowNotice("Unlink the console before replacing " + name, true);
            return;
        }
        const std::string title = "Select " + name;
        const std::optional<std::string> picked = BrowseForFile(title.c_str(), "sdmc:/");
        if (!picked) {
            return;
        }
        if (InstallUniqueDataFile(file, *picked)) {
            SetSettingsPage(settings_page);
            ShowNotice(name + " installed", false);
        } else {
            ShowNotice("Could not install " + name, true);
        }
    }

    // `nav` moves the cursor; `dpad_nav` is the d-pad-only subset that is allowed to edit a value.
    bool HandleSettings(u64 down, u32 nav, u32 dpad_nav) {
        if (down & HidNpadButton_Y) {
            OpenSettingsSearch();
            return false;
        }
        if (down & HidNpadButton_L) {
            StepSettingsPage(-1);
        }
        if (down & HidNpadButton_R) {
            StepSettingsPage(+1);
        }
        if (ThemesPageShown()) {
            HandleThemes(down, nav);
            return false;
        }
        if ((down & HidNpadButton_X) && !PerGameOpen() && !settings_search_open &&
            ExpertEntryCount(settings_page) > 0) {
            ToggleShowAllSettings();
            return false;
        }
        if (settings_rows.empty()) {
            if (down & HidNpadButton_B) {
                if (PerGameOpen()) {
                    ClosePerGameSettings();
                } else {
                    EnterRail();
                }
            }
            return false;
        }

        int& sel = SettingsSel();
        if (nav & DirUp) {
            sel = SettleSettingsSelection(std::max(0, sel - 1), -1);
        }
        if (nav & DirDown) {
            sel = SettleSettingsSelection(
                std::min(static_cast<int>(settings_rows.size()) - 1, sel + 1), +1);
        }
        ScrollSettingsIntoView();

        const auto back = [this] {
            if (settings_search_open) {
                CloseSettingsSearch();
            } else if (PerGameOpen()) {
                ClosePerGameSettings();
            } else {
                EnterRail();
            }
        };

        const SettingsRow& row = settings_rows[sel];
        if (row.modal != SettingsModal::None) {
            if ((down & HidNpadButton_A) || (dpad_nav & DirRight)) {
                OpenSettingsModal(row.modal);
            }
            if (down & HidNpadButton_B) {
                back();
            }
            return false;
        }

        if (PerGameOpen() && (down & HidNpadButton_X) && row.set_global) {
            row.set_global(!row.using_global());
            per_game_dirty = true;
            return false;
        }

        const auto edit = [this, &row](int dir) {
            if (PerGameOpen()) {
                if (row.set_global && row.using_global()) {
                    row.set_global(false);
                }
                row.step(dir);
                per_game_dirty = true;
                return;
            }
            row.step(dir);
            settings_dirty = true;
        };

        // Settings::values is edited live. FlushSettings() only batches the config.ini write.
        const bool show_all = IsShowAllSettingsEnabled();
        if (dpad_nav & DirLeft) {
            edit(-1);
        }
        if (dpad_nav & DirRight) {
            edit(+1);
        }
        if (down & HidNpadButton_A) {
            edit(row.boolean && row.boolean() ? -1 : +1);
        }
        if (IsShowAllSettingsEnabled() != show_all && !settings_search_open && !PerGameOpen()) {
            // `row` belongs to the list this replaces.
            RelistSettingsPage();
            return false;
        }
        if (down & HidNpadButton_B) {
            back();
        }
        return false;
    }

    void HandleLayoutPicker(u64 down, u32 nav) {
        const int count = GetScreenLayoutCount();
        if (nav & DirUp) {
            layout_picker_sel = (layout_picker_sel - 1 + count) % count;
        }
        if (nav & DirDown) {
            layout_picker_sel = (layout_picker_sel + 1) % count;
        }
        if (down & HidNpadButton_A) {
            SetLayoutCycleMask(GetLayoutCycleMask() ^ (1u << layout_picker_sel));
            settings_dirty = true;
        }
        if (down & HidNpadButton_B) {
            layout_picker_open = false;
        }
    }

    void OpenPresetPicker() {
        preset_sel = 0;
        preset_picker_open = true;
    }

    void HandlePresetPicker(u64 down, u32 nav) {
        if (nav & DirUp) {
            preset_sel = (preset_sel - 1 + NumSettingsPresets) % NumSettingsPresets;
        }
        if (nav & DirDown) {
            preset_sel = (preset_sel + 1) % NumSettingsPresets;
        }
        if (down & HidNpadButton_A) {
            OpenResetConfirm(static_cast<SettingsPreset>(preset_sel));
        }
        if (down & HidNpadButton_B) {
            preset_picker_open = false;
        }
    }

    void OpenResetConfirm(SettingsPreset preset) {
        const std::string name = SettingsPresetName(preset);
        const bool risky = preset != SettingsPreset::Default;
        confirm = ConfirmPrompt{
            "Reset to " + name + " settings?",
            risky ? std::vector<std::string>{"Every setting is reset, then the " + name,
                                             "selected preset is applied. Mappings are reset too."}
                  : std::vector<std::string>{"Every setting returns to its default for this",
                                             "version, with controller mappings included."},
            risky ? "This preset can break some games."
                  : "Your folders, titles, and saves are untouched.",
            "Reset",
            [this, preset] {
                ResetSettings(preset);
                // ResetSettings() has already written the new values out.
                settings_dirty = false;
                preset_picker_open = false;
                SetSettingsPage(settings_page);
                ShowNotice("Settings reset to " + std::string{SettingsPresetName(preset)}, false);
            }};
    }

    void OpenShaderCacheConfirm() {
        confirm = ConfirmPrompt{
            "Clear the shader cache?",
            {"Every game's compiled shaders and pipelines will be deleted.",
             "Expect stutter on your next run of a game."},
            "Post-processing shaders are untouched.",
            "Clear",
            [this] {
                const u64 freed = ClearShaderCache();
                RefreshShaderCacheSize();
                SetSettingsPage(settings_page);
                ShowNotice("Shader cache cleared, " + FormatSize(freed) + " freed", false);
            }};
    }

    void HandleConfirm(u64 down) {
        if (down & HidNpadButton_A) {
            // The action outlives the prompt it came from, so it is moved out before closing.
            const std::function<void()> action = std::move(confirm->on_accept);
            confirm.reset();
            action();
            return;
        }
        if (down & HidNpadButton_B) {
            const std::function<void()> cancel = std::move(confirm->on_cancel);
            confirm.reset();
            if (cancel) {
                cancel();
            }
        }
    }

    // True while a check the user asked for is blocking the menu behind its modal.
    bool UpdateModalOpen() const {
        return update_check_active && update_check_kind != UpdateCheckKind::Silent;
    }

    void BeginUpdateCheck(UpdateCheckKind kind) {
        if (update_check_active || update_download_active) {
            if (kind != UpdateCheckKind::Silent) {
                ShowNotice("An update operation is already running", false);
            }
            return;
        }
        if (updater_thread.joinable()) {
            updater_thread.join();
        }
        update_check_active = true;
        update_check_kind = kind;
        updater_done = false;
        updater_cancel = false;
        const UpdateChannel channel = GetUpdateChannel();
        updater_thread = std::thread([this, channel] {
            update_check_result = CheckForUpdate(channel, &updater_cancel);
            updater_done = true;
        });
    }

    void PumpUpdater() {
        if (!updater_done) {
            return;
        }
        updater_done = false;
        if (updater_thread.joinable()) {
            updater_thread.join();
        }

        if (update_check_active) {
            update_check_active = false;
            const UpdateCheckKind kind = update_check_kind;
            update_check_kind = UpdateCheckKind::Silent;
            const bool manual = kind == UpdateCheckKind::Manual;
            if (!update_check_result.current_notes.empty()) {
                CacheReleaseNotes(CurrentVersion(), update_check_result.current_notes);
                FillPendingNotes(update_check_result.current_notes);
            }
            if (kind == UpdateCheckKind::Notes) {
                if (!update_check_result.current_notes.empty()) {
                    OpenReleaseNotesCard(update_check_result.current_notes);
                } else if (update_check_result.status == UpdateCheckStatus::Error) {
                    ShowNotice(update_check_result.error, true);
                } else {
                    ShowNotice("GitHub lists no notes for EmuSwitch " +
                                   std::string{CurrentVersion()},
                               true);
                }
                return;
            }
            if (update_check_result.status == UpdateCheckStatus::Error) {
                if (manual) {
                    OpenUpdateError(update_check_result.error);
                }
                return;
            }
            if (update_check_result.status == UpdateCheckStatus::UpToDate) {
                if (manual) {
                    ShowNotice("EmuSwitch " + std::string{CurrentVersion()} + " is up to date",
                               false);
                }
                return;
            }
            if (!manual && update_check_result.release.tag == GetDismissedUpdateTag()) {
                return;
            }
            OpenUpdateConfirm(update_check_result.release);
            return;
        }

        if (update_download_active) {
            update_download_active = false;
            if (update_install_result.success) {
                // Cached now so the What's New card works offline later.
                CacheReleaseNotes(update_release.tag, update_release.notes);
                DismissUpdateTag("");
                update_installed = true;
            } else {
                OpenUpdateError(update_install_result.error);
            }
        }
    }

    // Puts up the welcome tour on a first run, or the notes for a build the user has just
    // moved onto.
    void ShowStartupCard() {
        const std::string current = CurrentVersion();
        const std::string previous = GetLastSeenVersion();
        if (previous == current) {
            return;
        }
        RecordSeenVersion(current);
        const bool first_run = previous.empty() && GetLaunchCount() <= 1;
        if (!first_run && !IsWhatsNewCardEnabled()) {
            return;
        }
        if (first_run) {
            info_card = BuildWelcomeCard();
            return;
        }
        update_from_version = previous;
        OpenWhatsNewCard();
    }

    std::string NotesHeading() const {
        return update_from_version.empty() ? "Release notes"
                                           : "Updated from " + update_from_version;
    }

    void OpenWhatsNewCard() {
        const int max_w = InfoCardTextW();
        InfoCard card;
        card.title = "What's New in EmuSwitch " + std::string{CurrentVersion()};
        const CachedReleaseNotes cached = LoadCachedReleaseNotes();
        if (CompareReleaseVersions(cached.tag, CurrentVersion()) == 0) {
            card.pages = PaginateNotes(cached.notes, NotesHeading(), max_w);
        }
        if (card.pages.empty()) {
            card.pages.push_back(MakeInfoPage(
                NotesHeading(),
                {"EmuSwitch is now on " + std::string{CurrentVersion()} + ".", "",
                 "The notes for this release appear here once fetched from GitHub, and "
                 "are also under Settings > General to see later."},
                max_w));
            info_card_notes_pending = true;
        }
        card.pages.push_back(MakeSupportPage(max_w));
        info_card = std::move(card);
    }

    // Swaps the placeholder page of an open What's New card for the notes the startup check
    // brought back, leaving the support page where it is.
    void FillPendingNotes(const std::string& notes) {
        if (!info_card || !info_card_notes_pending) {
            return;
        }
        info_card_notes_pending = false;
        std::vector<InfoPage> pages = PaginateNotes(notes, NotesHeading(), InfoCardTextW());
        if (pages.empty()) {
            return;
        }
        pages.push_back(info_card->pages.back());
        info_card->pages = std::move(pages);
        info_card->page = 0;
    }

    void OpenReleaseNotesCard(const std::string& notes) {
        InfoCard card;
        card.title = "EmuSwitch " + std::string{CurrentVersion()};
        card.pages = PaginateNotes(notes, "Release notes", InfoCardTextW());
        if (card.pages.empty()) {
            ShowNotice("This release has no notes", false);
            return;
        }
        info_card = std::move(card);
    }

    void OpenReleaseNotes() {
        const CachedReleaseNotes cached = LoadCachedReleaseNotes();
        if (!cached.notes.empty() && CompareReleaseVersions(cached.tag, CurrentVersion()) == 0) {
            OpenReleaseNotesCard(cached.notes);
            return;
        }
        BeginUpdateCheck(UpdateCheckKind::Notes);
    }

    void HandleInfoCard(u64 down, u32 nav) {
        const int last = static_cast<int>(info_card->pages.size()) - 1;
        int page = info_card->page;
        if ((nav & DirLeft) || (down & HidNpadButton_L)) {
            --page;
        }
        if ((nav & DirRight) || (down & HidNpadButton_R)) {
            ++page;
        }
        info_card->page = std::clamp(page, 0, last);
        if (down & (HidNpadButton_A | HidNpadButton_B)) {
            info_card.reset();
            info_card_notes_pending = false;
        }
    }

    void OpenUpdateConfirm(const UpdateRelease& release) {
        update_release = release;
        const std::string kind = release.prerelease ? "prerelease" : "stable release";
        confirm = ConfirmPrompt{"Update EmuSwitch to " + release.tag + '?',
                                {"Installed: " + std::string{CurrentVersion()},
                                 "Available: " + release.tag + " (" + kind + ")",
                                 "It's checked, then installed when EmuSwitch restarts."},
                                "The current NRO is kept as a .backup file.",
                                "Update",
                                [this, release] { StartUpdate(release); },
                                [tag = release.tag] { DismissUpdateTag(tag); }};
    }

    void StartUpdate(const UpdateRelease& release) {
        if (updater_thread.joinable()) {
            updater_thread.join();
        }
        update_release = release;
        update_downloaded = 0;
        update_total = release.size;
        updater_done = false;
        update_download_active = true;
        updater_thread = std::thread([this, release] {
            update_install_result = InstallUpdate(
                release, GetUpdaterExecutablePath(), [this](std::uint64_t downloaded,
                                                            std::uint64_t total) {
                    update_downloaded = downloaded;
                    if (total != 0) {
                        update_total = total;
                    }
                });
            updater_done = true;
        });
    }

    // Chainloads the NRO that was just installed.
    void QueueUpdatedRelaunch() {
        const std::string& path = GetUpdaterExecutablePath();
        if (path.empty() || !envHasNextLoad()) {
            return;
        }
        envSetNextLoad(path.c_str(), path.c_str());
    }

    void OpenUpdateError(const std::string& error) {
        std::vector<std::string> lines;
        constexpr std::size_t line_length = 66;
        for (std::size_t pos = 0; pos < error.size(); pos += line_length) {
            lines.push_back(error.substr(pos, line_length));
        }
        if (lines.empty()) {
            lines.emplace_back("The update operation failed for an unknown reason.");
        }
        confirm = ConfirmPrompt{"Update failed", std::move(lines),
                                "The installed NRO was not changed.", "Close", [] {}};
    }

    void OpenPerGameSettings() {
        if (filtered.empty()) {
            return;
        }
        const GameEntry& game = games[filtered[selected]];
        if (game.program_id == 0) {
            ShowNotice("This title has no title ID to attach settings to", true);
            return;
        }
        FlushSettings();
        details_open = false;
        focus = Focus::Content;
        per_game_id = game.program_id;
        per_game_title = game.title;
        per_game_dirty = false;
        per_game_sel.fill(0);
        per_game_scroll.fill(0);
        per_game_return_page = settings_page;
        page_dir = 1.0f;
        page_anim = 0.0f;
        ApplyPerGameConfig(per_game_id);
        SetSettingsPage(CategoryHasOverridables(settings_page) ? settings_page
                                                               : FirstOverridablePage());
    }

    void ClosePerGameSettings() {
        if (per_game_dirty) {
            SavePerGameConfig();
            ShowNotice(CountPerGameOverrides() == 0
                           ? "Cleared the settings for " + per_game_title
                           : "Saved settings for " + per_game_title,
                       false);
        }
        ClearPerGameConfig();
        per_game_id = 0;
        per_game_title.clear();
        per_game_dirty = false;
        settings_search_open = false;
        settings_rows.clear();
        settings_page = per_game_return_page;
        page_dir = -1.0f;
        page_anim = 0.0f;
    }

    void OpenRemap() {
        remap_sel = 0;
        remap_scroll = 0;
        remap_open = true;
    }

    void CloseRemap() {
        remap_open = false;
        // Rebuild the guest input profile and persist.
        ApplyButtonMappings();
        SaveConfig();
    }

    void ScrollRemapIntoView() {
        remap_scroll =
            std::clamp(remap_scroll, std::max(0, remap_sel - RemapVisibleRows() + 1), remap_sel);
    }

    // Cycles the physical Switch button bound to `control` by `dir`.
    void StepRemapMapping(MappableControl control, int dir) {
        const int cur = static_cast<int>(GetMapping(control));
        const int next = (cur + dir + NumBindingChoices) % NumBindingChoices;
        SetMapping(control, static_cast<InputButton>(next));
    }

    void HandleRemap(u64 down, u32 nav, u32 dpad_nav) {
        if (nav & DirUp) {
            remap_sel = (remap_sel - 1 + NumMappableControls) % NumMappableControls;
        }
        if (nav & DirDown) {
            remap_sel = (remap_sel + 1) % NumMappableControls;
        }
        ScrollRemapIntoView();

        const auto control = static_cast<MappableControl>(remap_sel);
        if (dpad_nav & DirLeft) {
            StepRemapMapping(control, -1);
        }
        if ((dpad_nav & DirRight) || (down & HidNpadButton_A)) {
            StepRemapMapping(control, +1);
        }
        if (down & HidNpadButton_X) {
            SetMapping(control, InputButton::None);
        }
        if (down & HidNpadButton_Y) {
            SetMapping(control, DefaultMapping(control));
        }
        if (down & HidNpadButton_B) {
            CloseRemap();
        }
    }

    bool HandlePaths(u64 down, u32 nav) {
        if (nav & DirUp) {
            paths_sel = std::max(0, paths_sel - 1);
        }
        if (nav & DirDown) {
            paths_sel = std::min(PathRowCount - 1, paths_sel + 1);
        }
        if (paths_sel == PathRowRecursive) {
            if (nav & DirLeft) {
                paths.scan_recursive = false;
                paths_dirty = true;
            }
            if (nav & DirRight) {
                paths.scan_recursive = true;
                paths_dirty = true;
            }
            if (down & HidNpadButton_A) {
                paths.scan_recursive = !paths.scan_recursive;
                paths_dirty = true;
            }
        } else if (down & HidNpadButton_A) {
            PickFolder(paths_sel);
        }
        if (down & HidNpadButton_Y) {
            ResetToDefault(paths_sel);
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        return false;
    }

    std::string& PathRowValue(int row) {
        switch (row) {
        case PathRowUserDir:
            return paths.user_dir;
        case PathRowRomsDir2:
            return paths.roms_dir_2;
        default:
            return paths.roms_dir;
        }
    }

    void PickFolder(int row) {
        std::string& current = PathRowValue(row);
        // An unset second folder starts the browse next to the first one.
        const std::optional<std::string> picked =
            BrowseForFolder(current.empty() ? paths.roms_dir : current);
        if (!picked) {
            return;
        }
        current = *picked;
        paths_dirty = true;
        RefreshRomsDir2Presence();
    }

    void RefreshRomsDir2Presence() {
        roms_dir_2_present = DirectoryExists(paths.roms_dir_2);
    }

    void ResetToDefault(int row) {
        switch (row) {
        case PathRowUserDir:
            paths.user_dir = GetDefaultUserDir();
            break;
        case PathRowRomsDir:
            paths.roms_dir = GetDefaultRomsDir(paths.user_dir);
            break;
        case PathRowRomsDir2:
            paths.roms_dir_2.clear();
            break;
        default:
            paths.scan_recursive = true;
            break;
        }
        paths_dirty = true;
    }

    void EnsureVisible(const Grid& grid) {
        if (grid.cols != home_cols) {
            BuildHomeLayout(); // the menu turned on its side
        }
        if (filtered.empty() || home_rows.empty()) {
            return;
        }
        const HomeRow& row = home_rows[static_cast<std::size_t>(home_row_of[static_cast<std::size_t>(selected)])];
        // The first row of a section brings its header along.
        const bool first_row = home_sections[static_cast<std::size_t>(row.section)].first == row.first;
        const float top = row.y - (first_row ? static_cast<float>(Skin::kSectionHeaderH) : 14.0f);
        const float bottom = row.y + kTileH + 14.0f;
        const float view_h = HomeViewH();
        if (top < home_top) {
            home_top = top;
        } else if (bottom > home_top + view_h) {
            home_top = bottom - view_h;
        }
        home_top = std::clamp(home_top, 0.0f, std::max(0.0f, home_height + 14.0f - view_h));
    }

    void HandleTouch() {
        HidTouchScreenState ts{};
        if (hidGetTouchScreenStates(&ts, 1) == 0 || ts.count == 0) {
            touch_was_down = false;
            return;
        }
        // The panel is never rotated, so a contact has to be turned onto the canvas.
        const int px = static_cast<int>(ts.touches[0].x);
        const int py = static_cast<int>(ts.touches[0].y);
        int tx = px;
        int ty = py;
        switch (g_rotation) {
        case 90:
            tx = py;
            ty = g_screen_h - 1 - px;
            break;
        case 180:
            tx = g_screen_w - 1 - px;
            ty = g_screen_h - 1 - py;
            break;
        case 270:
            tx = g_screen_w - 1 - py;
            ty = px;
            break;
        default:
            break;
        }
        if (touch_was_down) {
            return; // Act on the initial contact only.
        }
        touch_was_down = true;

        if (const int dock_hit = Skin::DockHitTest(canvas, static_cast<int>(kRailItems.size()), tx, ty); dock_hit >= 0) {
            SetTab(kRailItems[dock_hit].first);
            rail_sel = tab;
            focus = Focus::Content;
            return;
        }
        if (tx < kRailW) {
            if (const std::optional<Tab> hit = RailHitTest(ty)) {
                SetTab(*hit);
                rail_sel = tab;
                focus = Focus::Content;
            }
            return;
        }
        if (tab == Tab::Library) {
            if (game_menu != GameMenu::None) {
                // A row picks it; anywhere else closes the menu.
                for (int r = 0; r < GameMenuRows(); ++r) {
                    if (GameMenuRowRect(r).Contains(tx, ty)) {
                        ChooseGameMenuRow(r);
                        return;
                    }
                }
                game_menu = GameMenu::None;
                return;
            }
            if (game_moving) {
                return; // Carried with the buttons; A drops it.
            }
            const Grid grid = ComputeGrid();
            for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                int tile_x, tile_y;
                if (!TileRect(grid, i, tile_x, tile_y)) {
                    continue;
                }
                if (tx >= tile_x && tx < tile_x + kTileW && ty >= tile_y &&
                    ty < tile_y + kTileH) {
                    if (selected == i) {
                        pending_launch = games[filtered[i]].path; // Second tap launches.
                    }
                    selected = i;
                    EnsureVisible(grid);
                    break;
                }
            }
        } else if (tab == Tab::Systems) {
            if (system_menu_open) {
                // The one entry picks the console up; anywhere else closes the menu.
                system_menu_open = false;
                if (SystemMenuRowRect().Contains(tx, ty)) {
                    BeginSystemMove();
                }
                return;
            }
            if (system_moving) {
                return; // Carried with the buttons; A drops it.
            }
            if (system_games_open) {
                // Tap a game to highlight it, tap it again to play.
                const int top = GameListTop();
                if (tx < kGameListX || ty < top || ty >= GameListBottom()) {
                    return;
                }
                const int i = static_cast<int>((ty - top + system_games_scroll.x) / kGameRowStep);
                if (i >= 0 && i < static_cast<int>(system_games.size())) {
                    if (i == system_games_sel) {
                        pending_launch = games[static_cast<std::size_t>(system_games[static_cast<std::size_t>(i)])].path;
                    }
                    system_games_sel = i;
                    EnsureGameListVisible();
                }
                return;
            }
            // Tap a card to bring it forward, tap the focused one to see its games, or tap the
            // picture button.
            if (IsPictureEditingEnabled() && Skin::CarouselPictureButtonHit(canvas, tx, ty)) {
                PickSystemPicture();
                return;
            }
            const int hit = Skin::CarouselHitTest(canvas, static_cast<int>(system_order.size()), systems_anim, tx, ty);
            if (hit >= 0 && hit == systems_sel) {
                OpenSystemGames();
            } else if (hit >= 0) {
                systems_sel = hit;
            }
        } else if (tab == Tab::Settings) {
            const std::optional<int> page =
                settings_search_open
                    ? std::optional<int>{}
                    : SettingsTabHitTest(tx, ty, static_cast<int>(settings_page));
            if (page) {
                SetSettingsPage(static_cast<Category>(*page));
                return;
            }
            if (ThemesPageShown()) {
                for (int i = theme_scroll * kThemeCols; i < MenuThemeCount(); ++i) {
                    if (ThemeCardRect(i).Contains(tx, ty)) {
                        theme_sel = i;
                        UseTheme(i);
                        break;
                    }
                }
                return;
            }
            const int visible = (ty - kSettingsTop) / kSettingsRowStride;
            const int row = SettingsScroll() + visible;
            if (ty >= kSettingsTop && visible < SettingsVisibleRows() && row >= 0 &&
                row < static_cast<int>(settings_rows.size()) && !settings_rows[row].is_header) {
                SettingsSel() = row;
                if (settings_rows[row].modal != SettingsModal::None) {
                    OpenSettingsModal(settings_rows[row].modal);
                } else {
                    const SettingsRow& touched = settings_rows[row];
                    if (touched.boolean) {
                        touched.step(touched.boolean() ? -1 : +1);
                    } else {
                        touched.step(tx > kContentX + ContentW() / 2 ? +1 : -1);
                    }
                    settings_dirty = true;
                }
            }
        } else if (tab == Tab::Paths) {
            for (int i = 0; i < PathRowCount; ++i) {
                const int y = PathRowTop(i);
                if (ty < y || ty >= y + PathRowHeight(i)) {
                    continue;
                }
                paths_sel = i;
                if (i == PathRowRecursive) {
                    paths.scan_recursive = !paths.scan_recursive;
                    paths_dirty = true;
                } else {
                    PickFolder(i);
                }
                break;
            }
        }
    }

    // Screen rect of filtered tile `i` given the current scroll.
    bool TileRect(const Grid& grid, int i, int& out_x, int& out_y) {
        if (i < 0 || i >= static_cast<int>(home_row_of.size())) {
            return false;
        }
        float x, y;
        HomeTilePos(grid, i, grid_scroll.x, x, y);
        if (y < HomeViewTop() - 8 || y + kTileH > HomeViewBottom() + 8) {
            return false;
        }
        out_x = static_cast<int>(x);
        out_y = static_cast<int>(std::lround(y));
        return true;
    }

    // Modal folder picker. Blocks until a folder is chosen or the user backs out.
    // An empty `dir` lists the mounted devices, which is the only way onto storage other
    // than the SD card.
    std::optional<std::string> BrowseForFolder(const std::string& start) {
        return Browse("Select folder", start, false);
    }

    std::optional<std::string> BrowseForFile(const char* title, const std::string& start) {
        return Browse(title, start, true);
    }

    std::optional<std::string> BrowseForImage(const char* title, const std::string& start) {
        return Browse(title, start, true, true);
    }

    // The shared folder/file browser. In file mode the files in the current folder are listed
    // below its subfolders and A returns one; in folder mode + returns the folder itself.
    std::optional<std::string> Browse(const char* title, const std::string& start, bool pick_file,
                                      bool images_only = false) {
        auto list_files = [images_only](const std::string& d) {
            std::vector<FileEntry> all = ListFilesIn(d);
            if (images_only) {
                all.erase(std::remove_if(all.begin(), all.end(),
                                         [](const FileEntry& f) { return !Art::IsImageFile(f.name); }),
                          all.end());
            }
            return all;
        };
        std::string dir = EnsureDirectory(start) ? start : std::string{"sdmc:/"};
        std::vector<DirEntry> entries = ListSubdirectories(dir);
        std::vector<FileEntry> files = pick_file ? list_files(dir) : std::vector<FileEntry>{};
        int sel = 0;
        int scroll = 0;
        Repeater rep;
        PickerPreview preview;

        auto Enter = [&](const std::string& next, const std::string& highlight) {
            dir = next;
            entries = dir.empty() ? ListDevices() : ListSubdirectories(dir);
            files = pick_file && !dir.empty() ? list_files(dir) : std::vector<FileEntry>{};
            const int base = dir.empty() ? 0 : 1;
            sel = 0;
            scroll = 0;
            for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                if (entries[i].path == highlight) {
                    sel = i + base;
                    break;
                }
            }
        };

        while (appletMainLoop()) {
            padUpdate(pad_state);
            const u64 down = padGetButtonsDown(pad_state);
            const HidAnalogStickState ls = padGetStickPos(pad_state, 0);
            constexpr int dz = 12000;
            const u32 nav = rep.Step((down & HidNpadButton_Up) || ls.y > dz,
                                     (down & HidNpadButton_Down) || ls.y < -dz, false, false);

            const std::string parent = dir.empty() ? "" : ParentDirectory(dir);
            // Row 0 is ".." everywhere but the device list, which a device root steps up into.
            const int base = dir.empty() ? 0 : 1;
            const int dirs = static_cast<int>(entries.size());
            const int count = dirs + static_cast<int>(files.size()) + base;
            sel = std::clamp(sel, 0, std::max(0, count - 1));

            if (nav & DirUp) {
                sel = std::max(0, sel - 1);
            }
            if (nav & DirDown) {
                sel = std::min(std::max(0, count - 1), sel + 1);
            }
            scroll = std::clamp(scroll, std::max(0, sel - BrowseRows() + 1),
                                std::max(0, std::min(sel, count - BrowseRows())));
            if (down & HidNpadButton_A) {
                if (base == 1 && sel == 0) {
                    Enter(parent, dir);
                } else if (sel - base < dirs) {
                    Enter(entries[sel - base].path, "");
                } else {
                    return files[sel - base - dirs].path;
                }
            }
            if (down & HidNpadButton_B) {
                if (dir.empty()) {
                    return std::nullopt;
                }
                Enter(parent, dir);
            }
            if (down & HidNpadButton_Y) {
                return std::nullopt;
            }
            if (!pick_file && (down & HidNpadButton_Plus) && !dir.empty()) {
                return dir;
            }

            if (images_only) {
                // Preview the highlighted picture once the cursor rests on it for a moment.
                const bool on_file = sel - base >= dirs && sel - base - dirs < static_cast<int>(files.size());
                const std::string path = on_file ? files[static_cast<std::size_t>(sel - base - dirs)].path : std::string{};
                if (path != preview.path) {
                    preview = PickerPreview{};
                    preview.path = path;
                    preview.since = NowSeconds();
                }
                if (!path.empty() && !preview.requested && NowSeconds() - preview.since > 0.12) {
                    Art::RequestPreview(path);
                    preview.requested = true;
                }
                if (preview.requested && !preview.ready) {
                    preview.ready = Art::TakePreview(path, preview.img, preview.error);
                }
            }

            PrepareFrame();
            RenderFrame([&](Canvas& c) {
                DrawBrowser(c, title, dir, entries, files, sel, scroll, pick_file, images_only ? &preview : nullptr);
            });
        }
        return std::nullopt;
    }

    // The picture picker's preview of the highlighted file.
    struct PickerPreview {
        std::string path;
        Gfx::Image img;
        std::string error;
        bool requested = false;
        bool ready = false;
        double since = 0.0;
    };

    void DrawPickerPreview(Canvas& c, const PickerPreview& p, int x, int y, int w, int h) {
        Skin::DrawPanel(c, x, y, w, h, 22);
        const int inner = w - 40;
        const int cx = x + w / 2, cy = y + 20 + inner / 2;
        if (p.path.empty()) {
            const char* msg = "Highlight a picture to preview it";
            g_font.Draw(c, cx - g_font.Measure(msg, 17) / 2, cy, msg, 17, kColTextDim);
        } else if (!p.ready) {
            Skin::DrawSpinner(c, float(cx), float(cy), 16.0f, AnimTime());
        } else if (!p.error.empty() || p.img.Empty()) {
            const std::string msg = "Can't use this one: " + p.error;
            const std::string shown = g_font.Truncate(msg, 16, inner);
            g_font.Draw(c, cx - g_font.Measure(shown, 16) / 2, cy, shown, 16, kColError);
        } else {
            const float k = std::min(1.0f, std::min(float(inner) / p.img.w, float(inner) / p.img.h));
            const float dw = p.img.w * k, dh = p.img.h * k;
            c.SoftShadow(static_cast<int>(cx - dw / 2), static_cast<int>(cy - dh / 2), static_cast<int>(dw),
                         static_cast<int>(dh), 14, 16, 6, 0x90);
            c.DrawImageScaled(p.img, cx - dw / 2, cy - dh / 2, dw, dh, 14);
        }
        const std::size_t slash = p.path.find_last_of('/');
        const std::string name = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
        if (!name.empty()) {
            const std::string shown = g_font.Truncate(name, 17, inner);
            g_font.Draw(c, cx - g_font.Measure(shown, 17) / 2, y + h - 22, shown, 17, kColText);
        }
    }

    void DrawBrowser(Canvas& c, const char* title, const std::string& dir,
                     const std::vector<DirEntry>& entries, const std::vector<FileEntry>& files,
                     int sel, int scroll, bool pick_file, const PickerPreview* preview) {
        Skin::DrawBackdrop(c);
        // With a preview the list makes room for it on the right.
        const bool show_preview = preview && g_screen_w > 900;
        const int list_w = show_preview ? g_screen_w - 64 - 440 : g_screen_w - 64;

        const bool devices = dir.empty();
        g_font.Draw(c, 40, 44, devices ? "Select device" : title, 28, kColText);
        g_font.Draw(c, 40, 76,
                    devices ? std::string{"Mounted devices"}
                            : g_font.TruncateFront(dir, 20, g_screen_w - 80),
                    20, kColAccent);
        c.FillRect(40, 96, g_screen_w - 80, 1, kColLine);

        const bool has_parent = !devices;
        const int base = has_parent ? 1 : 0;
        const int dirs = static_cast<int>(entries.size());
        const int count = dirs + static_cast<int>(files.size()) + base;

        if (count == 0) {
            g_font.Draw(c, 52, kBrowseTop + 30, pick_file ? "Nothing here" : "No subfolders here",
                        20, kColTextDim);
        }
        for (int i = scroll; i < std::min(count, scroll + BrowseRows()); ++i) {
            const int y = kBrowseTop + (i - scroll) * kBrowseRowH;
            if (i == sel) {
                Skin::DrawRow(c, 32, y, list_w, kBrowseRowH - 4, true);
            }
            const bool up = has_parent && i == 0;
            const bool is_dir = up || i - base < dirs;
            const std::string name = up      ? ".."
                                     : is_dir ? entries[i - base].name + "/"
                                              : files[i - base - dirs].name;
            g_font.Draw(c, 52, CenterBaseline(y, kBrowseRowH - 4, 20),
                        g_font.Truncate(name, 20, list_w - 64), 20,
                        up ? kColTextDim : kColText);
        }
        DrawListScrollbar(c, 32 + list_w + 12, kBrowseTop, BrowseRows(), kBrowseRowH, count, scroll);
        if (show_preview) {
            const int pw = 400, px = g_screen_w - 40 - pw;
            DrawPickerPreview(c, *preview, px, kBrowseTop, pw, std::min(pw + 44, ContentBottom() - kBrowseTop));
        }

        int hx = 40;
        const int hy = g_screen_h - 44;
        hx += DrawHint(c, hx, hy, "A", pick_file ? "Open / Select" : "Open") + 22;
        hx += DrawHint(c, hx, hy, "B", has_parent ? "Up" : "Cancel") + 22;
        if (!devices && !pick_file) {
            hx += DrawHint(c, hx, hy, "+", "Select this folder") + 22;
        }
        DrawHint(c, hx, hy, "Y", "Cancel");
    }

    void DrawPathsPage(Canvas& c) {
        const bool content_focus = focus == Focus::Content;
        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        for (int i = 0; i < PathRowCount; ++i) {
            const int y = PathRowTop(i);
            const int h = PathRowHeight(i);
            const bool on = i == paths_sel;
            if (on) {
                Skin::DrawRow(c, x, y, w, h, content_focus);
            }
            if (i == PathRowRecursive) {
                g_font.Draw(c, x + 20, CenterBaseline(y, h, 22), PathRowLabel(i), 22, kColText);
                const char* value = paths.scan_recursive ? "On" : "Off";
                const int vw = g_font.Measure(value, 22);
                g_font.Draw(c, x + w - 24 - vw, CenterBaseline(y, h, 22), value, 22,
                            on && content_focus ? kColAccent : kColTextDim);
                continue;
            }
            g_font.Draw(c, x + 20, y + 30, PathRowLabel(i), 22, kColText);
            const std::string& dir = PathRowValue(i);
            const std::string value =
                dir.empty() ? std::string{"Not set"} : g_font.TruncateFront(dir, 18, w - 44);
            g_font.Draw(c, x + 20, y + 58, value, 18,
                        on && content_focus ? kColAccent : kColTextDim);
        }

        int y = PathRowTop(PathRowCount - 1) + PathRowHeight(PathRowCount - 1) + 30;
        if (RestartPending()) {
            g_font.Draw(c, x + 20, y, "Restart EmuSwitch to move to the new folder.", 18, kColAccent);
            y += 26;
        }
        if (!paths.roms_dir_2.empty() && !roms_dir_2_present) {
            g_font.Draw(c, x + 20, y, "The second ROM folder is not reachable right now.", 18,
                        kColTextDim);
            y += 26;
        }

        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            hx +=
                DrawHint(c, hx, hy, "A", paths_sel == PathRowRecursive ? "Toggle" : "Browse") + 22;
            hx += DrawHint(c, hx, hy, "Y", paths_sel == PathRowRomsDir2 ? "Clear" : "Default") + 22;
            hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    void DrawSavesPanel(Canvas& c) {
        const GameEntry& game = games[filtered[selected]];
        const int w = std::min(660, ContentW() - 48);
        constexpr int h = 430;
        const int x = kContentX + (ContentW() - w) / 2;
        const int y = kContentTop + (ContentBottom() - kContentTop - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        int ty = y + 22;
        g_font.Draw(c, x + 24, ty + 20, g_font.Truncate(game.title, 24, w - 48), 24, kColText);
        ty += 38;

        int cx = x + 24;
        for (const SaveKind kind : {SaveKind::SaveData, SaveKind::ExtData}) {
            const bool active = kind == saves_kind;
            const bool usable = kind == SaveKind::SaveData || saves_extdata_id != 0;
            const char* label = SaveKindName(kind);
            const int chip_w = g_font.Measure(label, 18) + 28;
            Skin::DrawPill(c, cx, ty, chip_w, 30, active);
            g_font.Draw(c, cx + 14, CenterBaseline(ty, 30, 18), label, 18,
                        active ? kColOnAccent : (usable ? kColText : kColTextDim));
            cx += chip_w + 10;
        }
        if (saves_extdata_id == 0) {
            g_font.Draw(c, cx + 6, CenterBaseline(ty, 30, 16), "none", 16, kColTextDim);
        }
        ty += 38;

        g_font.Draw(c, x + 24, ty + 16,
                    saves_present ? "Save present on the emulated card"
                                  : "Nothing on the emulated card to back up",
                    16, saves_present ? kColTextDim : kColError);
        ty += 26;
        c.FillRect(x + 24, ty, w - 48, 1, kColRail);
        ty += 10;

        const int count = static_cast<int>(saves_backups.size());
        if (count == 0) {
            g_font.Draw(c, x + 24, ty + 24, "No backups yet.", 18, kColTextDim);
            g_font.Draw(c, x + 24, ty + 50, "Press X to create one.", 18, kColTextDim);
        }
        for (int i = 0; i < std::min(count - saves_scroll, kSavesRows); ++i) {
            const SaveBackup& backup = saves_backups[saves_scroll + i];
            const int row_y = ty + i * kSavesRowH;
            const bool selected_row = saves_scroll + i == saves_sel;
            if (selected_row) {
                Skin::DrawRow(c, x + 20, row_y, w - 52, kSavesRowH - 4, true);
            }
            const std::string detail = FormatSize(backup.size) + "  " +
                                       std::to_string(backup.files) +
                                       (backup.files == 1 ? " file" : " files");
            const int detail_w = g_font.Measure(detail, 16);
            g_font.Draw(c, x + 30, CenterBaseline(row_y, kSavesRowH - 4, 18),
                        g_font.Truncate(backup.name, 18, w - 96 - detail_w), 18,
                        selected_row ? kColText : kColTextDim);
            g_font.Draw(c, x + w - 40 - detail_w, CenterBaseline(row_y, kSavesRowH - 4, 16), detail,
                        16, kColTextDim);
        }
        DrawListScrollbar(c, x + w - 20, ty, kSavesRows, kSavesRowH, count, saves_scroll);

        int hx = x + 24;
        const int hy = y + h - 38;
        if (count > 0) {
            hx += DrawHint(c, hx, hy, "A", "Restore") + 18;
        }
        hx += DrawHint(c, hx, hy, "X", "Back up") + 18;
        if (count > 0) {
            hx += DrawHint(c, hx, hy, "Y", "Delete") + 18;
        }
        if (saves_extdata_id != 0) {
            hx += DrawHint(c, hx, hy, "<>", "Kind") + 18;
        }
        DrawHint(c, hx, hy, "B", "Close");
    }

    // ---- frames ------------------------------------------------------------------------------
    //
    // PrepareFrame() moves every animation along and works out what the frame shows; it is the
    // only place per-frame state changes. DrawScene() then only reads, which lets RenderFrame()
    // draw it on three cores at once (see Gfx::Canvas).

    // Which modal is up, if any, so it can fade in when it opens.
    int ModalKind() const {
        if (info_card) return 1;
        if (update_installed) return 2;
        if (update_download_active) return 3;
        if (UpdateModalOpen()) return 4;
        if (confirm) return 6;
        if (remap_open) return 7;
        if (country_picker_open) return 8;
        if (preset_picker_open) return 9;
        if (layout_picker_open) return 10;
        if (saves_open) return 11;
        if (details_open) return 12;
        return 0;
    }

    // Title and subtitle of the profile bar for the page on screen.
    void HeaderFor(std::string& title, std::string& sub) const {
        sub.clear();
        if (PerGameOpen()) {
            title = "Game settings";
            sub = per_game_title;
            return;
        }
        title = kRailItems[static_cast<std::size_t>(tab)].second;
        switch (tab) {
        case Tab::Library:
            if (focus == Focus::Content && !filtered.empty()) {
                const GameEntry& game = games[filtered[static_cast<std::size_t>(selected)]];
                title = game.title;
                sub = game_moving ? std::string{"Moving: use the arrows, then A"}
                                  : std::string{SystemName(game.system)};
            } else {
                title = g_nickname;
                sub = std::to_string(filtered.size()) + (filtered.size() == 1 ? " game" : " games");
            }
            if (!search.empty()) {
                sub = "Search: " + search + "  (" + std::to_string(filtered.size()) + ")";
            }
            break;
        case Tab::Systems:
            if (system_games_open && focus == Focus::Content && !system_games.empty()) {
                const GameEntry& game =
                    games[static_cast<std::size_t>(system_games[static_cast<std::size_t>(system_games_sel)])];
                title = game.title;
                sub = std::string{SystemName(game.system)};
            } else if (system_moving) {
                sub = "Moving " + Art::Systems()[static_cast<std::size_t>(SystemRowAt(systems_sel))].name;
            } else {
                sub = "Pick a console to see its games";
            }
            break;
        default:
            break;
        }
    }

    void PrepareFrame() {
        ApplyMenuTheme();
        // Pictures finished loading in the background join between frames.
        Art::Pump();
        const double now = NowSeconds();
        const float dt = frame_started ? static_cast<float>(std::clamp(now - g_now, 0.0, 0.1)) : 1.0f / 60.0f;
        frame_started = true;
        g_now = now;

        // Profile bar: the title fades across when it changes.
        std::string title, sub;
        HeaderFor(title, sub);
        if (title != header_title || sub != header_sub) {
            header_prev_title = std::move(header_title);
            header_prev_sub = std::move(header_sub);
            header_title = std::move(title);
            header_sub = std::move(sub);
            header_changed = now;
        }
        // Clock and battery: once a second is plenty.
        if (now - status_sampled > 1.0 || status_sampled < 0) {
            status_sampled = now;
            const std::time_t t = std::time(nullptr);
            const std::tm* lt = std::localtime(&t);
            clock_text.clear();
            ampm_text.clear();
            if (lt) {
                char hm[8] = "", ap[4] = "";
                std::strftime(hm, sizeof(hm), "%I:%M", lt);
                std::strftime(ap, sizeof(ap), "%p", lt);
                clock_text = hm;
                ampm_text = ap;
            }
            u32 pct = 0;
            battery = R_SUCCEEDED(psmGetBatteryChargePercentage(&pct)) ? static_cast<int>(pct) : -1;
            PsmChargerType charger{};
            charging = R_SUCCEEDED(psmGetChargerType(&charger)) && charger != PsmChargerType_Unconnected;
        }

        // Dock highlight and its label.
        const Tab dock_target = PerGameOpen() ? Tab::Settings : (focus == Focus::Rail ? rail_sel : tab);
        dock_slide.Step(static_cast<float>(dock_target), dt, 300.0f, 30.0f);
        dock_label = Approach(dock_label, focus == Focus::Rail && !PerGameOpen() ? 1.0f : 0.0f, dt, 0.06f);

        // Page changes slide the new page in.
        page_anim = std::min(1.0f, page_anim + dt / 0.28f);

        // The install pill: what's installing and how far along it is.
        {
            const int index = install_index.load(), count = install_count.load();
            const bool showing = install_active && index > 0;
            if (showing) {
                std::lock_guard lock{install_mutex};
                install_label = "Installing " + install_name;
                if (count > 1) {
                    install_label += "  (" + std::to_string(index) + " of " + std::to_string(count) + ")";
                }
                const std::size_t total = install_total.load();
                install_frac = total == 0 ? 0.0f
                                          : static_cast<float>(static_cast<double>(install_written.load()) / total);
            }
            install_pill = Approach(install_pill, showing ? 1.0f : 0.0f, dt, 0.08f);
        }

        // Modals fade and rise in.
        const int kind = ModalKind();
        if (kind != modal_kind) {
            modal_kind = kind;
            modal_anim = kind != 0 && modal_anim > 0.5f ? 0.6f : 0.0f;
        }
        modal_anim = std::min(1.0f, modal_anim + dt / 0.18f);

        // The grid: scroll, the gliding cursor and the focused tile popping up.
        if (tab == Tab::Library && !filtered.empty()) {
            const Grid grid = ComputeGrid();
            if (grid.cols != home_cols) {
                BuildHomeLayout();
            }
            grid_scroll.Step(home_top, dt, 320.0f, 34.0f);
            float tx, ty;
            HomeTilePos(grid, selected, grid_scroll.x, tx, ty);
            if (!cursor_ready) {
                cursor_x.Snap(tx);
                cursor_y.Snap(ty);
                cursor_ready = true;
            }
            cursor_x.Step(tx, dt, 520.0f, 44.0f);
            cursor_y.Step(ty, dt, 520.0f, 44.0f);
            if (selected != lift_index) {
                lift_prev_index = lift_index;
                lift_prev = lift.x;
                lift_index = selected;
                lift.Snap(0.0f);
            }
            lift.Step(focus == Focus::Content ? 1.0f : 0.0f, dt, 380.0f, 20.0f);
            lift_prev = Approach(lift_prev, 0.0f, dt, 0.05f);
            SetAmbientForGame(games[filtered[static_cast<std::size_t>(selected)]]);
        } else if (tab == Tab::Systems) {
            Skin::SetAmbient(SystemColor(SystemAt(systems_sel)));
        } else {
            Skin::SetAmbient(MakeColor(60, 90, 160));
        }
        if (tab != Tab::Library) {
            cursor_ready = false;
        }

        // The carousel glides; the controller's colour follows the focused system.
        systems_spring.Step(static_cast<float>(systems_sel), dt, 240.0f, 26.0f);
        systems_anim = systems_spring.x;
        {
            const u32 target = SystemColor(SystemAt(systems_sel));
            for (int i = 0; i < 3; ++i) {
                systems_accent[i] = Approach(systems_accent[i], float((target >> (i * 8)) & 0xFF), dt, 0.07f);
            }
        }
        const bool systems_page = tab == Tab::Systems && !PerGameOpen();
        const float spot_y = kContentTop + (g_screen_h - kHintH - 44 - kContentTop) / 2.0f + 96.0f;
        // The pool of light under the controller; the game list has no controller.
        Skin::SetSpot(225.0f, spot_y, 240.0f, SystemColor(SystemAt(systems_sel)),
                      systems_page && !system_games_open ? 1.0f : 0.0f);

        // The + menu and the console game list fade in; the list's highlight and scroll glide.
        system_menu_anim = system_menu_open ? std::min(1.0f, system_menu_anim + dt / 0.16f) : 0.0f;
        game_menu_anim = game_menu != GameMenu::None ? std::min(1.0f, game_menu_anim + dt / 0.16f) : 0.0f;
        if (system_games_open) {
            system_games_anim = std::min(1.0f, system_games_anim + dt / 0.24f);
            system_games_scroll.Step(system_games_top, dt, 320.0f, 34.0f);
            system_games_cursor.Step(static_cast<float>(system_games_sel), dt, 520.0f, 44.0f);
        }

        Skin::BeginFrame(now, g_screen_w, g_screen_h);
    }

    void SetAmbientForGame(const GameEntry& game) {
        Skin::SetAmbient(Skin::TileAccent(TileFor(game)));
    }

    u32 SystemsAccent() const {
        return MakeColor(static_cast<u8>(systems_accent[0] + 0.5f), static_cast<u8>(systems_accent[1] + 0.5f),
                         static_cast<u8>(systems_accent[2] + 0.5f));
    }

    // Draws `scene` once without pixels (so every cache it needs is built on this thread), then
    // in bands across the cores, each band going straight into the framebuffer.
    void RenderFrame(const std::function<void(Canvas&)>& scene) {
        EnsureFramebuffer();
        if (!fb_ready) {
            return;
        }
        const float cover = MenuFadeCover();
        if (cover <= 0.0f) {
            RenderToFramebuffer(canvas, fb, scene);
            return;
        }
        const auto alpha = static_cast<u8>(std::lround(255.0f * cover));
        RenderToFramebuffer(canvas, fb, [&](Canvas& c) {
            scene(c);
            c.FillRect(0, 0, c.Width(), c.Height(), MakeColor(0, 0, 0, alpha));
        });
    }

    // One frame of the menu, with `overlay` drawn over it (a blocking prompt, say).
    void Frame(const std::function<void(Canvas&)>& overlay = {}) {
        PrepareFrame();
        RenderFrame([&](Canvas& c) {
            DrawScene(c);
            if (overlay) {
                overlay(c);
            }
        });
    }

    Skin::DockState DockStateNow() const {
        Skin::DockState d;
        d.active = static_cast<int>(PerGameOpen() ? Tab::Settings : tab);
        d.cursor = static_cast<int>(rail_sel);
        d.focused = focus == Focus::Rail && !PerGameOpen();
        d.slide = dock_slide.x;
        d.label = dock_label;
        d.t = AnimTime();
        return d;
    }

    void DrawTopBar(Canvas& c) const {
        Skin::TopBar bar;
        bar.title = header_title;
        bar.subtitle = header_sub;
        bar.prev_title = header_prev_title;
        bar.prev_subtitle = header_prev_sub;
        bar.blend = static_cast<float>(std::clamp((g_now - header_changed) / 0.28, 0.0, 1.0));
        bar.clock = clock_text;
        bar.ampm = ampm_text;
        bar.battery = battery;
        bar.charging = charging;
        Skin::DrawTopBar(c, SkinFonts(), bar);
    }

    void DrawScene(Canvas& c) {
        Skin::DrawBackdrop(c);
        DrawTopBar(c);
        DrawRail(c, DockStateNow());
        {
            // The page slides in from the side it was reached from.
            const float e = EaseOut(page_anim);
            Canvas::OffsetScope slide{c, static_cast<int>(std::lround(page_dir * 46.0f * (1.0f - e))), 0};
            Canvas::FadeScope fade{c, 0.25f + 0.75f * e};
            if (PerGameOpen()) {
                DrawSettingsPage(c);
            } else if (tab == Tab::Library) {
                DrawLibrary(c);
            } else if (tab == Tab::Systems) {
                DrawSystemsPage(c);
            } else if (tab == Tab::Settings) {
                DrawSettingsPage(c);
            } else {
                DrawPathsPage(c);
            }
        }
        DrawNotice(c);
        DrawInstallPill(c);
        if (modal_kind == 0) {
            return;
        }
        const float e = EaseOut(modal_anim);
        Canvas::FadeScope fade{c, e};
        Canvas::OffsetScope rise{c, 0, static_cast<int>(std::lround(14.0f * (1.0f - e)))};
        if (details_open && !filtered.empty()) {
            DrawTitleDetails(c, games[filtered[selected]], details, details_customised);
        }
        if (saves_open && !filtered.empty()) {
            DrawSavesPanel(c);
        }
        if (layout_picker_open) {
            DrawLayoutPicker(c);
        }
        if (preset_picker_open) {
            DrawPresetPicker(c);
        }
        if (country_picker_open) {
            DrawCountryPicker(c);
        }
        if (remap_open) {
            DrawRemapPage(c);
        }
        if (confirm) {
            DrawConfirm(c);
        }
        if (UpdateModalOpen()) {
            DrawUpdateCheckProgress(c);
        }
        if (update_download_active) {
            DrawUpdateProgress(c);
        }
        if (update_installed) {
            DrawUpdateInstalled(c);
        }
        if (info_card) {
            DrawInfoCard(c);
        }
    }

    // A small glass pill under the profile bar while CIAs install in the background.
    void DrawInstallPill(Canvas& c) {
        if (install_pill <= 0.01f || install_label.empty()) {
            return;
        }
        const float e = EaseOut(install_pill);
        Canvas::FadeScope fade{c, e};
        Canvas::OffsetScope drop{c, 0, static_cast<int>(std::lround(-10.0f * (1.0f - e)))};
        const std::string text = g_font_bold.Truncate(install_label, 17, 520);
        const int tw = g_font_bold.Measure(text, 17);
        const int w = tw + 118, h = 44;
        // In the middle of the profile bar, between the name and the clock.
        const int x = (g_screen_w - w) / 2, y = (kHeaderH - h) / 2 + 2;
        Skin::DrawPanel(c, x, y, w, h, h / 2);
        Skin::DrawSpinner(c, x + 26.0f, y + h / 2.0f, 8.0f, AnimTime());
        g_font_bold.Draw(c, x + 46, CenterBaseline(y, h - 6, 17), text, 17, kColText);
        const std::string pct = std::to_string(static_cast<int>(install_frac * 100.0f + 0.5f)) + "%";
        g_font.Draw(c, x + w - 20 - g_font.Measure(pct, 15), CenterBaseline(y, h - 6, 15), pct, 15, kColTextDim);
        Skin::DrawProgress(c, x + 46, y + h - 12, w - 66, 4, install_frac, AnimTime());
    }

    void DrawUpdateCheckProgress(Canvas& c) {
        const int w = std::min(560, g_screen_w - 48);
        constexpr int h = 112;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);
        g_font.Draw(c, x + 24, y + 46,
                    update_check_kind == UpdateCheckKind::Notes
                        ? "Fetching the release notes from GitHub..."
                        : "Checking GitHub for updates...",
                    22, kColText);
        g_font.Draw(c, x + 24, y + 80, "This normally takes only a few seconds.", 18,
                    kColTextDim);
    }

    void DrawUpdateProgress(Canvas& c) {
        const std::uint64_t downloaded = update_downloaded.load();
        const std::uint64_t total = update_total.load();
        const int w = std::min(600, g_screen_w - 48);
        constexpr int h = 150;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);
        g_font.Draw(c, x + 24, y + 42,
                    g_font.Truncate("Downloading EmuSwitch " + update_release.tag, 20, w - 48), 20,
                    kColText);

        const int bar_x = x + 24;
        const int bar_y = y + 66;
        const int bar_w = w - 48;
        Skin::DrawProgress(c, bar_x, bar_y, bar_w, 10,
                           total == 0 ? 0.0f : static_cast<float>(static_cast<double>(downloaded) / total),
                           AnimTime());
        g_font.Draw(c, bar_x, bar_y + 38,
                    FormatSize(static_cast<std::size_t>(downloaded)) + " / " +
                        FormatSize(static_cast<std::size_t>(total)),
                    18, kColTextDim);
        g_font.Draw(c, bar_x, bar_y + 66, "Verifying before updating", 16,
                    kColTextDim);
    }

    void DrawUpdateInstalled(Canvas& c) {
        const int w = std::min(640, g_screen_w - 48);
        constexpr int h = 218;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);
        g_font.Draw(c, x + 24, y + 46, "Update downloaded", 24, kColText);
        g_font.Draw(c, x + 24, y + 84, "EmuSwitch " + update_release.tag + " is ready.", 19,
                    kColAccent);
        g_font.Draw(c, x + 24, y + 116,
                    "EmuSwitch will close, install it and reopen on the new version.", 17,
                    kColTextDim);
        g_font.Draw(c, x + 24, y + 144, "The previous NRO stays beside it as a .backup file.", 17,
                    kColTextDim);
        DrawHint(c, x + 24, y + h - 42, "A", "Restart");
    }

    void DrawInfoCard(Canvas& c) {
        const int w = InfoCardW();
        const int h = InfoCardH();
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        const InfoPage& page = info_card->pages[info_card->page];
        g_font.Draw(c, x + 24, y + 46, g_font.Truncate(info_card->title, 24, w - 48), 24, kColText);
        if (!page.heading.empty()) {
            g_font.Draw(c, x + 24, y + 78, g_font.Truncate(page.heading, 19, w - 48), 19,
                        kColAccent);
        }
        int line_y = y + kInfoBodyTop;
        for (const std::string& line : page.lines) {
            g_font.Draw(c, x + 24, line_y, line, kInfoBodySize,
                        line == kKofiUrl ? kColAccent : kColTextDim);
            line_y += kInfoLineH;
        }

        const int count = static_cast<int>(info_card->pages.size());
        if (count > 1) {
            constexpr int dot = 8;
            constexpr int gap = 8;
            int dx = x + (w - (count * dot + (count - 1) * gap)) / 2;
            const int dy = y + h - 58;
            for (int i = 0; i < count; ++i) {
                c.FillRoundRect(dx, dy, dot, dot, dot / 2,
                                i == info_card->page ? kColAccent : kColBadge);
                dx += dot + gap;
            }
        }

        int hx = x + 24;
        const int hy = y + h - 38;
        if (count > 1) {
            hx += DrawHint(c, hx, hy, "L R", "Page") + 22;
        }
        DrawHint(c, hx, hy, "A", "Close");
    }

    void DrawLibrary(Canvas& c) {
        const bool content_focus = focus == Focus::Content;
        if (filtered.empty()) {
            DrawEmptyLibrary(c, paths.roms_dir);
        } else {
            const Grid grid = ComputeGrid();
            const int step = kTileH + kTileGap;
            const float scroll = grid_scroll.x;
            const int count = static_cast<int>(filtered.size());
            const int used_w = grid.cols * kTileW + (grid.cols - 1) * kTileGap;
            const float view_top = static_cast<float>(HomeViewTop());
            const float view_bottom = static_cast<float>(HomeViewBottom());
            const float intro = static_cast<float>(g_now - library_intro);
            // Tiles slide under the profile bar and above the dock, fading as they leave.
            Canvas::ClipScope clip{c, 0, kContentTop - 20, g_screen_w, ContentBottom() - 16 - (kContentTop - 20)};
            const auto edge_fade = [&](float fy, float h) {
                float out = 0.0f;
                if (fy < view_top - 4) {
                    out = (view_top - fy) / (step * 0.75f);
                } else if (fy + h > view_bottom + 4) {
                    out = (fy + h - view_bottom) / (step * 0.75f);
                }
                return std::clamp(1.0f - out, 0.0f, 1.0f);
            };
            // On arrival everything rises in, a column and a row at a time.
            const auto wave_at = [&](float fy, int col) {
                return EaseOut(intro * 3.4f - (col * 0.055f + ((fy - view_top) / step) * 0.13f));
            };
            const auto look_for = [&](int i, int& x, int& y) {
                float fx, fy;
                HomeTilePos(grid, i, scroll, fx, fy);
                x = static_cast<int>(fx);
                y = static_cast<int>(std::lround(fy));
                const int col = (x - grid.start_x) / (kTileW + kTileGap);
                Skin::TileLook look;
                look.appear = edge_fade(fy, kTileH) * wave_at(fy, col);
                look.t = AnimTime();
                look.dim = !content_focus;
                return look;
            };
            // Each console's header: its logo, name and how many games it has.
            for (const HomeSection& section : home_sections) {
                const float fy = view_top + section.y - scroll;
                if (fy > view_bottom + 40.0f || fy + Skin::kSectionHeaderH < view_top - 60.0f) {
                    continue;
                }
                const std::string games_text =
                    std::to_string(section.count) + (section.count == 1 ? " game" : " games");
                Skin::DrawSectionHeader(c, SkinFonts(), CardFor(section.system + 1), SystemName(section.system),
                                        games_text, grid.start_x, static_cast<int>(std::lround(fy)), used_w,
                                        edge_fade(fy, 40.0f) * wave_at(fy, 0));
            }
            for (const HomeRow& row : home_rows) {
                const float fy = view_top + row.y - scroll;
                if (fy > view_bottom + step || fy + kTileH < view_top - step) {
                    continue;
                }
                for (int k = 0; k < row.count; ++k) {
                    const int i = row.first + k;
                    if (i == selected || i == lift_prev_index) {
                        continue;
                    }
                    int x, y;
                    const Skin::TileLook look = look_for(i, x, y);
                    DrawTile(c, games[filtered[static_cast<std::size_t>(i)]], x, y, look);
                }
            }
            // The tile the cursor left settles back; the focused one pops up over everything.
            if (lift_prev_index >= 0 && lift_prev_index < count && lift_prev_index != selected) {
                int x, y;
                Skin::TileLook look = look_for(lift_prev_index, x, y);
                look.lift = lift_prev;
                DrawTile(c, games[filtered[static_cast<std::size_t>(lift_prev_index)]], x, y, look);
            }
            if (selected >= 0 && selected < count) {
                int x, y;
                Skin::TileLook look = look_for(selected, x, y);
                look.lift = lift.x;
                look.dim = false;
                if (game_moving) {
                    // The carried game glides with the cursor to its new place.
                    x = static_cast<int>(std::lround(cursor_x.x));
                    y = static_cast<int>(std::lround(cursor_y.x));
                }
                DrawTile(c, games[filtered[static_cast<std::size_t>(selected)]], x, y, look);
                if (content_focus) {
                    const float size = kTileW + (kTileFocus - kTileW) * lift.x;
                    Skin::DrawFocusRing(c, cursor_x.x + kTileW / 2.0f - size / 2,
                                        cursor_y.x + kTileH / 2.0f - 3.0f * lift.x - size / 2, size, AnimTime(),
                                        std::clamp(lift.x * 1.6f, 0.0f, 1.0f) * look.appear);
                }
                if (game_moving) {
                    bool left, right, up, down;
                    GameMoveRoom(left, right, up, down);
                    Skin::DrawMoveArrows(c, cursor_x.x, cursor_y.x - 3.0f * lift.x, static_cast<float>(kTileW),
                                         AnimTime(), left, right, up, down);
                }
            }
            DrawScrollbar(c, grid);
        }
        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else if (game_moving) {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            hx += DrawHint(c, hx, hy, "D-Pad", "Move") + 22;
            hx += DrawHint(c, hx, hy, "A", "Done") + 22;
            DrawHint(c, hx, hy, "B", "Cancel");
        } else {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            hx += DrawHint(c, hx, hy, "A", "Play") + 22;
            hx += DrawHint(c, hx, hy, "X", "Search") + 22;
            hx += DrawHint(c, hx, hy, "Y", "Refresh") + 22;
            if (IsPictureEditingEnabled()) {
                hx += DrawHint(c, hx, hy, "L", "Picture") + 22;
            }
            hx += DrawHint(c, hx, hy, "+", "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
        if (game_menu != GameMenu::None) {
            DrawGameMenu(c);
        }
    }

    // ---- Systems: the consoles in a carousel; A lists a console's games, + moves it -------------

    int systems_sel = 0; // position in system_order
    float systems_anim = 0.0f;
    std::string picture_dir = "sdmc:/";
    // The consoles in the player's order: carousel positions -> rows of Art::Systems() (which
    // lists "3ds" first, then Multi::Systems() in order). Home's sections follow it too.
    std::vector<int> system_order;
    // The + menu (it only offers Move Placement) and carrying a console to a new place.
    bool system_menu_open = false;
    float system_menu_anim = 0.0f;
    bool system_moving = false;
    std::vector<int> system_order_before;
    int systems_sel_before = 0;
    // A console's games, listed when A is pressed on its card.
    bool system_games_open = false;
    std::vector<int> system_games; // indices into `games`
    int system_games_sel = 0;
    float system_games_top = 0.0f; // where the list is scrolled to, in pixels
    Spring system_games_scroll;
    Spring system_games_cursor; // the highlight glides between rows
    float system_games_anim = 0.0f;

    static constexpr int kGameRowH = 92;
    static constexpr int kGameRowStep = kGameRowH + 10;
    static constexpr int kGameListX = 440;

    // Art::Systems() row -> Multi::Systems() index (-1 for the 3DS).
    static int SystemIndexFor(int row) {
        return row == 0 ? -1 : row - 1;
    }
    // The Art::Systems() row at a carousel position.
    int SystemRowAt(int pos) const {
        return pos >= 0 && pos < static_cast<int>(system_order.size()) ? system_order[pos] : 0;
    }
    int SystemAt(int pos) const {
        return SystemIndexFor(SystemRowAt(pos));
    }
    // Where a console (a Multi::Systems() index, -1 for the 3DS) sits in the player's order.
    int SystemRank(int system) const {
        const int row = system + 1;
        for (std::size_t i = 0; i < system_order.size(); ++i) {
            if (system_order[i] == row) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(system_order.size());
    }
    Skin::SystemCard CardFor(int row) const {
        const auto& systems = Art::Systems();
        return {systems[static_cast<std::size_t>(row)].id, SystemBadge(SystemIndexFor(row)),
                SystemColor(SystemIndexFor(row))};
    }

    void LoadSystemOrder() {
        const auto& systems = Art::Systems();
        system_order.clear();
        std::vector<bool> used(systems.size(), false);
        const std::string ids = GetSystemsOrder();
        std::size_t start = 0;
        while (start < ids.size()) {
            std::size_t comma = ids.find(',', start);
            if (comma == std::string::npos) {
                comma = ids.size();
            }
            const std::string id = ids.substr(start, comma - start);
            for (std::size_t i = 0; i < systems.size(); ++i) {
                if (!used[i] && systems[i].id == id) {
                    used[i] = true;
                    system_order.push_back(static_cast<int>(i));
                    break;
                }
            }
            start = comma + 1;
        }
        // Consoles the saved order doesn't mention go at the end, in the usual order.
        for (std::size_t i = 0; i < systems.size(); ++i) {
            if (!used[i]) {
                system_order.push_back(static_cast<int>(i));
            }
        }
        systems_sel = std::clamp(systems_sel, 0, static_cast<int>(system_order.size()) - 1);
    }

    void SaveSystemOrder() {
        const auto& systems = Art::Systems();
        std::string ids;
        for (const int row : system_order) {
            if (!ids.empty()) {
                ids += ',';
            }
            ids += systems[static_cast<std::size_t>(row)].id;
        }
        SetSystemsOrder(ids);
        SaveConfig();
    }

    // ---- Home: the + menu (Move Placement, Info, Delete) and the picture menu (SD Card,
    // SteamGridDB) ----

    enum class GameMenu { None, Actions, Picture };
    GameMenu game_menu = GameMenu::None;
    int game_menu_sel = 0;
    float game_menu_anim = 0.0f;
    // Carrying a game to a new place in its console's section.
    bool game_moving = false;
    std::vector<int> game_move_before; // `filtered` before the move
    int game_move_selected_before = 0;
    // The player's order of the games: path -> place. Games it doesn't list go after, by title.
    std::unordered_map<std::string, int> game_rank;

    static constexpr int kGameMenuRowStep = 58;
    static constexpr const char* kGameOrderPath = "sdmc:/switch/emuswitch/game_order.txt";

    int GameRank(int game) const {
        const auto it = game_rank.find(games[static_cast<std::size_t>(game)].path);
        return it == game_rank.end() ? std::numeric_limits<int>::max() : it->second;
    }

    void LoadGameOrder() {
        game_rank.clear();
        FILE* f = std::fopen(kGameOrderPath, "rb");
        if (!f) {
            return;
        }
        char buf[1024];
        int rank = 0;
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) {
                game_rank.emplace(std::move(line), rank++);
            }
        }
        std::fclose(f);
    }

    // Saves Home's order (the whole list: moving clears the search). Games that aren't here right
    // now, say on a drive that's unplugged, keep their places after these.
    void SaveGameOrder() {
        std::vector<std::pair<int, std::string>> absent;
        std::set<std::string> present;
        for (const int i : filtered) {
            present.insert(games[static_cast<std::size_t>(i)].path);
        }
        for (const auto& [path, rank] : game_rank) {
            if (!present.count(path)) {
                absent.emplace_back(rank, path);
            }
        }
        std::sort(absent.begin(), absent.end());
        game_rank.clear();
        std::string text;
        int rank = 0;
        const auto add = [&](const std::string& path) {
            game_rank[path] = rank++;
            text += path;
            text += '\n';
        };
        for (const int i : filtered) {
            add(games[static_cast<std::size_t>(i)].path);
        }
        for (const auto& entry : absent) {
            add(entry.second);
        }
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/emuswitch", 0777);
        const std::string tmp = std::string{kGameOrderPath} + ".part";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            return;
        }
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (ok) {
            std::remove(kGameOrderPath);
            std::rename(tmp.c_str(), kGameOrderPath);
        } else {
            std::remove(tmp.c_str());
        }
    }

    int GameMenuRows() const {
        return game_menu == GameMenu::Picture ? 2 : 3;
    }

    void OpenGameMenu(GameMenu menu) {
        game_menu = menu;
        game_menu_sel = 0;
        game_menu_anim = 0.0f;
    }

    void HandleGameMenu(u64 down, u32 nav) {
        if (nav & DirUp) {
            game_menu_sel = std::max(0, game_menu_sel - 1);
        }
        if (nav & DirDown) {
            game_menu_sel = std::min(GameMenuRows() - 1, game_menu_sel + 1);
        }
        // The button that opened a menu closes it again.
        const u64 closers =
            HidNpadButton_B | (game_menu == GameMenu::Actions ? HidNpadButton_Plus : HidNpadButton_L);
        if (down & HidNpadButton_A) {
            ChooseGameMenuRow(game_menu_sel);
        } else if (down & closers) {
            game_menu = GameMenu::None;
        }
    }

    void ChooseGameMenuRow(int row) {
        const GameMenu menu = game_menu;
        game_menu = GameMenu::None;
        if (filtered.empty()) {
            return;
        }
        const GameEntry game = games[filtered[static_cast<std::size_t>(selected)]];
        if (menu == GameMenu::Actions) {
            if (row == 0) {
                BeginGameMove();
            } else if (row == 1) {
                OpenDetails();
            } else {
                OpenDeleteGameConfirm(game);
            }
        } else if (row == 0) {
            PickGamePicture(game);
        } else {
            PickSteamGridPicture(game);
        }
    }

    void OpenDeleteGameConfirm(const GameEntry& game) {
        std::vector<std::string> lines{"\"" + g_font.Truncate(game.title, 18, 460) + "\""};
        lines.push_back(game.installed ? "is uninstalled and deleted from the SD card,"
                                       : "is deleted from the SD card.");
        if (game.installed) {
            lines.push_back("with its update and DLC.");
        }
        confirm = ConfirmPrompt{"Delete game",
                                std::move(lines),
                                "This can't be undone. Its save data is kept.",
                                "Delete",
                                [this, game] { RunDeleteGame(game); },
                                {}};
    }

    void RunDeleteGame(const GameEntry& game) {
        ShowBusy("Deleting...");
        std::string error;
        if (!DeleteGameFiles(game, error)) {
            ShowNotice(error, true);
            return;
        }
        // What the menu kept for it: its pictures, its settings, its place in the order.
        const std::string stem = Art::PictureStem(game);
        for (const char* dir : {"sdmc:/switch/emuswitch/covers/", "sdmc:/switch/emuswitch/steamgriddb/"}) {
            for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
                std::remove((std::string{dir} + stem + ext).c_str());
            }
        }
        Art::SetGameArt(game, "");
        if (game.system < 0 && game.program_id != 0) {
            DeletePerGameConfig(game.program_id);
        }
        if (GetInsertedCartridge() == game.path) {
            SetInsertedCartridge("");
        }
        const bool had_rank = game_rank.erase(game.path) > 0;
        const int keep = selected;
        Rescan();
        selected = filtered.empty() ? 0 : std::min(keep, static_cast<int>(filtered.size()) - 1);
        if (had_rank) {
            SaveGameOrder();
        }
        ShowNotice("Deleted " + game.title, false);
    }

    void OpenDetails() {
        const GameEntry& game = games[filtered[static_cast<std::size_t>(selected)]];
        details = GetTitleDetails(game);
        details_customised = HasPerGameConfig(game.program_id);
        details_open = true;
    }

    // The section the game at `filtered` index `i` is in.
    const HomeSection* SectionAt(int i) const {
        for (const HomeSection& section : home_sections) {
            if (i >= section.first && i < section.first + section.count) {
                return &section;
            }
        }
        return nullptr;
    }

    // Picks the focused game up so the arrows move it around its console's section.
    void BeginGameMove() {
        if (filtered.empty()) {
            return;
        }
        if (!search.empty()) {
            // Games move within the whole list.
            const std::string path = games[filtered[static_cast<std::size_t>(selected)]].path;
            search.clear();
            ApplyFilter();
            for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                if (games[filtered[static_cast<std::size_t>(i)]].path == path) {
                    selected = i;
                    break;
                }
            }
        }
        game_move_before = filtered;
        game_move_selected_before = selected;
        game_moving = true;
    }

    // Whether the carried game can still go left, right, up or down in its section.
    void GameMoveRoom(bool& left, bool& right, bool& up, bool& down) const {
        left = right = up = down = false;
        const HomeSection* section = SectionAt(selected);
        if (!section || home_cols <= 0) {
            return;
        }
        const int at = selected - section->first;
        left = at > 0;
        right = at < section->count - 1;
        up = at / home_cols > 0;
        down = at / home_cols < (section->count - 1) / home_cols;
    }

    void MoveGameTo(int target) {
        const HomeSection* section = SectionAt(selected);
        if (!section || target == selected || target < section->first ||
            target >= section->first + section->count) {
            return;
        }
        const int carried = filtered[static_cast<std::size_t>(selected)];
        filtered.erase(filtered.begin() + selected);
        filtered.insert(filtered.begin() + target, carried);
        selected = target;
        // The carried tile stays up; the ones it passes don't pop.
        lift_index = selected;
        lift_prev_index = -1;
    }

    void HandleGameMove(u64 down, u32 nav) {
        bool left, right, up, dn;
        GameMoveRoom(left, right, up, dn);
        const HomeSection* section = SectionAt(selected);
        if ((nav & DirLeft) && left) {
            MoveGameTo(selected - 1);
        } else if ((nav & DirRight) && right) {
            MoveGameTo(selected + 1);
        } else if ((nav & DirUp) && up) {
            MoveGameTo(selected - home_cols);
        } else if ((nav & DirDown) && dn && section) {
            // Into a shorter last row, the carried game goes to its end.
            MoveGameTo(std::min(selected + home_cols, section->first + section->count - 1));
        }
        if (down & (HidNpadButton_A | HidNpadButton_Plus)) {
            FinishGameMove();
        } else if (down & HidNpadButton_B) {
            CancelGameMove();
        }
    }

    void FinishGameMove() {
        game_moving = false;
        if (filtered == game_move_before) {
            return;
        }
        SaveGameOrder();
        ShowNotice(games[filtered[static_cast<std::size_t>(selected)]].title + " moved", false);
    }

    void CancelGameMove() {
        filtered = game_move_before;
        selected = game_move_selected_before;
        lift_index = selected;
        lift_prev_index = -1;
        game_moving = false;
    }

    Rect GameMenuRect() const {
        const int w = 420, h = 44 + GameMenuRows() * kGameMenuRowStep + 52;
        return {(g_screen_w - w) / 2, (g_screen_h - h) / 2 - 20, w, h};
    }
    Rect GameMenuRowRect(int row) const {
        const Rect p = GameMenuRect();
        return {p.x + 16, p.y + 44 + row * kGameMenuRowStep, p.w - 32, 52};
    }

    void DrawGameMenu(Canvas& c) {
        if (filtered.empty()) {
            return;
        }
        const float e = EaseOut(game_menu_anim);
        Skin::DrawScrim(c, 0.7f * e);
        Canvas::FadeScope fade{c, e};
        Canvas::OffsetScope rise{c, 0, static_cast<int>(std::lround(12.0f * (1.0f - e)))};
        const Rect p = GameMenuRect();
        Skin::DrawPanel(c, p.x, p.y, p.w, p.h, 22);
        const GameEntry& game = games[filtered[static_cast<std::size_t>(selected)]];
        const bool picture = game_menu == GameMenu::Picture;
        const std::string heading = picture ? "Picture for " + game.title : game.title;
        g_font.Draw(c, p.x + 24, p.y + 30, g_font.Truncate(heading, 16, p.w - 48), 16, kColTextDim);
        static constexpr const char* kActionRows[] = {"Move Placement", "Info", "Delete"};
        static constexpr const char* kPictureRows[] = {"SD Card", "SteamGridDB"};
        const bool no_key = picture && GetSteamGridDbKey().empty();
        for (int r = 0; r < GameMenuRows(); ++r) {
            const Rect row = GameMenuRowRect(r);
            const bool on = r == game_menu_sel;
            if (on) {
                Skin::DrawRow(c, row.x, row.y, row.w, row.h, true);
            }
            // Delete is the one that can't be taken back, so it reads in red.
            const bool danger = !picture && r == 2;
            const u32 color = danger ? (on ? kColError : WithAlpha(kColError, 0xC8))
                                     : (on ? kColText : kColTextDim);
            g_font_bold.Draw(c, row.x + 24, CenterBaseline(row.y, row.h, 21),
                             (picture ? kPictureRows : kActionRows)[r], 21, color);
            if (no_key && r == 1) {
                const char* note = "Needs an API key";
                g_font.Draw(c, row.x + row.w - 22 - g_font.Measure(note, 15), CenterBaseline(row.y, row.h, 15), note, 15,
                            kColTextDim);
            }
        }
        int hx = p.x + 24;
        const int hy = p.y + p.h - 30;
        hx += DrawHint(c, hx, hy, "A", "Select") + 22;
        DrawHint(c, hx, hy, "B", "Close");
    }

    // ---- SteamGridDB: pictures for a game, searched by its name ------------------------------------

    static constexpr int kSgCols = 4;
    static constexpr int kSgCell = 176;
    static constexpr int kSgGap = 16;
    static constexpr int kSgTop = 118;
    static constexpr const char* kSteamGridDir = "sdmc:/switch/emuswitch/steamgriddb";

    int SteamGridRows() const {
        return std::max(1, (ContentBottom() - 12 - kSgTop + kSgGap) / (kSgCell + kSgGap));
    }

    // Blocks while the player looks through what SteamGridDB has for `game`; the picture picked
    // is saved and set.
    void PickSteamGridPicture(const GameEntry& game) {
        const std::string key = GetSteamGridDbKey();
        if (key.empty()) {
            ShowNotice("Add your SteamGridDB API key in Settings > Advanced first", true);
            return;
        }
        using SteamGrid::Stage;
        std::string term = SteamGrid::SearchTerm(game.title);
        SteamGrid::Search(term, key);
        SteamGrid::Results res;
        int sel = 0;
        int first_row = 0;
        Repeater rep;
        std::string saved;
        while (appletMainLoop()) {
            padUpdate(pad_state);
            const u64 down = padGetButtonsDown(pad_state);
            const HidAnalogStickState ls = padGetStickPos(pad_state, 0);
            constexpr int dz = 12000;
            const u32 nav = rep.Step((down & HidNpadButton_Up) || ls.y > dz, (down & HidNpadButton_Down) || ls.y < -dz,
                                     (down & HidNpadButton_Left) || ls.x < -dz,
                                     (down & HidNpadButton_Right) || ls.x > dz);
            SteamGrid::Poll(res);
            if (res.stage == Stage::Saved) {
                saved = res.saved;
                break;
            }
            const int count = static_cast<int>(res.pictures.size());
            if (res.stage == Stage::Ready && count > 0) {
                if ((nav & DirLeft) && sel > 0) {
                    --sel;
                }
                if ((nav & DirRight) && sel < count - 1) {
                    ++sel;
                }
                if ((nav & DirUp) && sel >= kSgCols) {
                    sel -= kSgCols;
                }
                if ((nav & DirDown) && sel / kSgCols < (count - 1) / kSgCols) {
                    sel = std::min(count - 1, sel + kSgCols);
                }
                const SteamGrid::Picture& pic = res.pictures[static_cast<std::size_t>(sel)];
                if ((down & HidNpadButton_A) && pic.loaded && !pic.thumb.Empty()) {
                    SteamGrid::Save(sel, std::string{kSteamGridDir} + "/" + Art::PictureStem(game));
                }
            }
            sel = std::clamp(sel, 0, std::max(0, count - 1));
            const int row = sel / kSgCols;
            first_row = std::clamp(first_row, std::max(0, row - SteamGridRows() + 1), row);
            if ((down & HidNpadButton_X) && res.stage != Stage::Saving) {
                const std::optional<std::string> text = PromptSettingText("Search SteamGridDB", "Game name", term, 100);
                if (text && !text->empty()) {
                    term = *text;
                    SteamGrid::Search(term, key);
                    sel = 0;
                    first_row = 0;
                }
            }
            if (down & HidNpadButton_B) {
                break;
            }
            PrepareFrame();
            RenderFrame([&](Canvas& c) { DrawSteamGridPicker(c, term, res, sel, first_row); });
        }
        SteamGrid::Cancel();
        if (saved.empty()) {
            return;
        }
        ShowBusy("Loading picture...");
        const std::string err = Art::SetGameArt(game, saved);
        ShowNotice(err.empty() ? "Picture set for " + game.title : "Couldn't use that picture: " + err, !err.empty());
    }

    static std::string SteamGridShape(const SteamGrid::Picture& p) {
        if (p.width > 0 && p.width == p.height) return "Square";
        return p.height > p.width ? "Portrait" : "Wide";
    }

    // "no_logo" -> "No logo".
    static std::string SteamGridStyle(std::string style) {
        for (char& ch : style) {
            if (ch == '_') ch = ' ';
        }
        if (!style.empty()) {
            style[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(style[0])));
        }
        return style;
    }

    void DrawSteamGridPicker(Canvas& c, const std::string& term, const SteamGrid::Results& res, int sel,
                             int first_row) {
        using SteamGrid::Stage;
        Skin::DrawBackdrop(c);
        g_font.Draw(c, 40, 44, "SteamGridDB", 28, kColText);
        const std::string sub = res.game.empty() ? "Searching for \"" + term + "\"" : "Pictures for " + res.game;
        g_font.Draw(c, 40, 76, g_font.Truncate(sub, 20, g_screen_w - 80), 20, kColAccent);
        c.FillRect(40, 96, g_screen_w - 80, 1, kColLine);

        const int count = static_cast<int>(res.pictures.size());
        const int pw = 400, px = g_screen_w - 40 - pw;
        const int mid_y = (kSgTop + ContentBottom()) / 2;
        const auto centred = [&](const std::string& text, int y, int size, u32 color) {
            const std::string shown = g_font.Truncate(text, size, g_screen_w - 120);
            g_font.Draw(c, (g_screen_w - g_font.Measure(shown, size)) / 2, y, shown, size, color);
        };
        if (res.stage == Stage::Searching || res.stage == Stage::Idle) {
            Skin::DrawSpinner(c, g_screen_w / 2.0f, mid_y - 24.0f, 16.0f, AnimTime());
            centred("Looking on SteamGridDB...", mid_y + 26, 20, kColTextDim);
        } else if (res.stage == Stage::Failed) {
            centred(res.error, mid_y, 20, kColError);
        } else if (count == 0) {
            centred("SteamGridDB has no pictures for " + res.game + " yet.", mid_y, 20, kColTextDim);
            centred("Press X to search for another name.", mid_y + 32, 18, kColTextDim);
        } else {
            const int rows = SteamGridRows();
            for (int i = first_row * kSgCols; i < std::min(count, (first_row + rows) * kSgCols); ++i) {
                const int x = 40 + (i % kSgCols) * (kSgCell + kSgGap);
                const int y = kSgTop + (i / kSgCols - first_row) * (kSgCell + kSgGap);
                Skin::DrawPanel(c, x, y, kSgCell, kSgCell, 16);
                const SteamGrid::Picture& p = res.pictures[static_cast<std::size_t>(i)];
                const float cx = x + kSgCell / 2.0f, cy = y + kSgCell / 2.0f;
                if (!p.loaded) {
                    Skin::DrawSpinner(c, cx, cy, 12.0f, AnimTime());
                } else if (p.thumb.Empty()) {
                    const char* msg = "No preview";
                    g_font.Draw(c, static_cast<int>(cx) - g_font.Measure(msg, 15) / 2, static_cast<int>(cy) + 5, msg, 15,
                                kColTextDim);
                } else {
                    const float inner = kSgCell - 20.0f;
                    const float k = std::min(inner / p.thumb.w, inner / p.thumb.h);
                    const float dw = p.thumb.w * k, dh = p.thumb.h * k;
                    c.DrawImageScaled(p.thumb, cx - dw / 2, cy - dh / 2, dw, dh, 10);
                }
                if (i == sel) {
                    Skin::DrawFocusRing(c, x - 6.0f, y - 6.0f, kSgCell + 12.0f, AnimTime(), 1.0f);
                }
            }
            const int total_rows = (count + kSgCols - 1) / kSgCols;
            DrawListScrollbar(c, 40 + kSgCols * (kSgCell + kSgGap) - 4, kSgTop, rows, kSgCell + kSgGap, total_rows,
                              first_row);

            // The highlighted picture, bigger.
            const SteamGrid::Picture& p = res.pictures[static_cast<std::size_t>(sel)];
            const int ph = std::min(pw + 44, ContentBottom() - 8 - kSgTop);
            Skin::DrawPanel(c, px, kSgTop, pw, ph, 22);
            const int inner_w = pw - 40, inner_h = ph - 82;
            const float cx = px + pw / 2.0f, cy = kSgTop + 20.0f + inner_h / 2.0f;
            if (!p.loaded) {
                Skin::DrawSpinner(c, cx, cy, 16.0f, AnimTime());
            } else if (!p.thumb.Empty()) {
                const float k = std::min(float(inner_w) / p.thumb.w, float(inner_h) / p.thumb.h);
                const float dw = p.thumb.w * k, dh = p.thumb.h * k;
                c.SoftShadow(static_cast<int>(cx - dw / 2), static_cast<int>(cy - dh / 2), static_cast<int>(dw),
                             static_cast<int>(dh), 14, 16, 6, 0x90);
                c.DrawImageScaled(p.thumb, cx - dw / 2, cy - dh / 2, dw, dh, 14);
            }
            std::string label = SteamGridShape(p);
            if (p.width > 0) {
                label += "  " + std::to_string(p.width) + " x " + std::to_string(p.height);
            }
            const std::string style = SteamGridStyle(p.style);
            g_font_bold.Draw(c, static_cast<int>(cx) - g_font_bold.Measure(label, 18) / 2, kSgTop + ph - 44, label, 18,
                             kColText);
            if (!style.empty()) {
                g_font.Draw(c, static_cast<int>(cx) - g_font.Measure(style, 16) / 2, kSgTop + ph - 20, style, 16,
                            kColTextDim);
            }
            if (!res.error.empty()) {
                centred(res.error, ContentBottom() - 2, 17, kColError);
            }
        }
        if (res.stage == Stage::Saving) {
            Skin::DrawScrim(c, 0.6f);
            Skin::DrawSpinner(c, g_screen_w / 2.0f, mid_y - 20.0f, 16.0f, AnimTime());
            centred("Saving picture...", mid_y + 30, 20, kColText);
        }

        int hx = 40;
        const int hy = g_screen_h - 44;
        if (res.stage == Stage::Ready && count > 0) {
            hx += DrawHint(c, hx, hy, "A", "Use") + 22;
        }
        hx += DrawHint(c, hx, hy, "X", "Search") + 22;
        DrawHint(c, hx, hy, "B", "Back");
        const char* credit = "Pictures from SteamGridDB";
        g_font.Draw(c, g_screen_w - 40 - g_font.Measure(credit, 15), hy + 5, credit, 15, kColTextDim);
    }

    void PickGamePicture(const GameEntry& game) {
        const std::string title = "Picture for " + game.title;
        const std::optional<std::string> picked = BrowseForImage(title.c_str(), picture_dir);
        if (!picked) {
            return;
        }
        picture_dir = ParentDirectory(*picked);
        ShowBusy("Loading picture...");
        const std::string err = Art::SetGameArt(game, *picked);
        ShowNotice(err.empty() ? "Picture set for " + game.title : "Couldn't use that picture: " + err, !err.empty());
    }

    // Lists the focused console's games on the Systems page.
    void OpenSystemGames() {
        const int sys = SystemAt(systems_sel);
        system_games.clear();
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (games[static_cast<std::size_t>(i)].system == sys) {
                system_games.push_back(i);
            }
        }
        // In the order Home shows them.
        std::stable_sort(system_games.begin(), system_games.end(),
                         [this](int a, int b) { return GameRank(a) < GameRank(b); });
        system_games_sel = 0;
        system_games_top = 0.0f;
        system_games_scroll.Snap(0.0f);
        system_games_cursor.Snap(0.0f);
        system_games_anim = 0.0f;
        system_games_open = true;
    }

    void PickSystemPicture() {
        const auto& systems = Art::Systems();
        const auto& sys = systems[static_cast<std::size_t>(SystemRowAt(systems_sel))];
        const std::string title = "Picture for " + sys.name;
        const std::optional<std::string> picked = BrowseForImage(title.c_str(), picture_dir);
        if (!picked) {
            return;
        }
        picture_dir = ParentDirectory(*picked);
        ShowBusy("Loading picture...");
        const std::string err = Art::SetSystemArt(sys.id, *picked);
        ShowNotice(err.empty() ? sys.name + " picture set" : "Couldn't use that picture: " + err, !err.empty());
    }

    // Picks the focused console up so the arrows move it along the carousel.
    void BeginSystemMove() {
        system_order_before = system_order;
        systems_sel_before = systems_sel;
        system_moving = true;
    }

    void MoveSystem(int dir) {
        const int next = systems_sel + dir;
        if (next < 0 || next >= static_cast<int>(system_order.size())) {
            return;
        }
        std::swap(system_order[static_cast<std::size_t>(systems_sel)], system_order[static_cast<std::size_t>(next)]);
        systems_sel = next;
        // The carried console stays under the cursor; its neighbours trade places around it.
        systems_spring.Snap(static_cast<float>(systems_sel));
    }

    void FinishSystemMove() {
        system_moving = false;
        if (system_order == system_order_before) {
            return;
        }
        SaveSystemOrder();
        ApplyFilter(); // Home's sections follow the new order
        ShowNotice(Art::Systems()[static_cast<std::size_t>(SystemRowAt(systems_sel))].name + " moved", false);
    }

    void CancelSystemMove() {
        system_order = system_order_before;
        systems_sel = systems_sel_before;
        systems_spring.Snap(static_cast<float>(systems_sel));
        system_moving = false;
    }

    int GameListTop() const {
        return kContentTop + 14;
    }
    int GameListBottom() const {
        return ContentBottom() - 36;
    }
    int GameListVisibleRows() const {
        return std::max(1, (GameListBottom() - GameListTop() + 10) / kGameRowStep);
    }

    void EnsureGameListVisible() {
        const float view_h = static_cast<float>(GameListBottom() - GameListTop());
        const float top = static_cast<float>(system_games_sel * kGameRowStep);
        const float bottom = top + kGameRowH;
        if (top < system_games_top) {
            system_games_top = top;
        } else if (bottom > system_games_top + view_h) {
            system_games_top = bottom - view_h;
        }
        const float max_top = std::max(0.0f, static_cast<float>(system_games.size()) * kGameRowStep - 10 - view_h);
        system_games_top = std::clamp(system_games_top, 0.0f, max_top);
    }

    bool HandleSystemGames(u64 down, u32 nav, MenuResult& result) {
        const int count = static_cast<int>(system_games.size());
        if (count > 0) {
            const int page = GameListVisibleRows();
            if (nav & DirUp) {
                system_games_sel = std::max(0, system_games_sel - 1);
            }
            if (nav & DirDown) {
                system_games_sel = std::min(count - 1, system_games_sel + 1);
            }
            if (nav & DirLeft) {
                system_games_sel = std::max(0, system_games_sel - page);
            }
            if (nav & DirRight) {
                system_games_sel = std::min(count - 1, system_games_sel + page);
            }
            if (down & HidNpadButton_A) {
                result = {MenuAction::Launch, games[static_cast<std::size_t>(system_games[system_games_sel])].path};
                return true;
            }
        }
        if (down & HidNpadButton_B) {
            system_games_open = false;
        }
        EnsureGameListVisible();
        return false;
    }

    bool HandleSystems(u64 down, u32 nav, MenuResult& result) {
        if (system_games_open) {
            return HandleSystemGames(down, nav, result);
        }
        if (system_menu_open) {
            if (down & HidNpadButton_A) {
                system_menu_open = false;
                BeginSystemMove();
            } else if (down & (HidNpadButton_B | HidNpadButton_Plus)) {
                system_menu_open = false;
            }
            return false;
        }
        const int count = static_cast<int>(system_order.size());
        if (system_moving) {
            if (nav & (DirUp | DirLeft)) {
                MoveSystem(-1);
            }
            if (nav & (DirDown | DirRight)) {
                MoveSystem(+1);
            }
            if (down & (HidNpadButton_A | HidNpadButton_Plus)) {
                FinishSystemMove();
            } else if (down & HidNpadButton_B) {
                CancelSystemMove();
            }
            return false;
        }
        if (nav & (DirUp | DirLeft)) {
            systems_sel = std::max(0, systems_sel - 1);
        }
        if (nav & (DirDown | DirRight)) {
            systems_sel = std::min(count - 1, systems_sel + 1);
        }
        const auto& sys = Art::Systems()[static_cast<std::size_t>(SystemRowAt(systems_sel))];
        if (down & HidNpadButton_A) {
            OpenSystemGames();
            return false;
        }
        // Guarded so that reaching for the +/- exit combo doesn't flash the menu open.
        if ((down & HidNpadButton_Plus) && !(held & HidNpadButton_Minus)) {
            system_menu_open = true;
            system_menu_anim = 0.0f;
            return false;
        }
        if ((down & HidNpadButton_Y) && IsPictureEditingEnabled()) {
            PickSystemPicture();
        }
        if ((down & HidNpadButton_X) && IsPictureEditingEnabled() && Skin::HasSystemImage(sys.id)) {
            const std::string err = Art::SetSystemArt(sys.id, "");
            ShowNotice(err.empty() ? sys.name + " back to its default look" : err, !err.empty());
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        return false;
    }

    void DrawSystemsPage(Canvas& c) {
        if (system_games_open) {
            // The list rises in as it opens.
            const float e = EaseOut(system_games_anim);
            Canvas::FadeScope fade{c, e};
            Canvas::OffsetScope rise{c, 0, static_cast<int>(std::lround(24.0f * (1.0f - e)))};
            DrawSystemGames(c);
            return;
        }
        const auto& systems = Art::Systems();
        std::vector<Skin::SystemCard> cards;
        for (const int row : system_order) {
            cards.push_back(CardFor(row));
        }
        const int row = SystemRowAt(systems_sel);
        const int sys = SystemIndexFor(row);
        int count = 0;
        for (const GameEntry& g : games) {
            count += g.system == sys ? 1 : 0;
        }
        const std::string detail = count == 0 ? "No games yet" : std::to_string(count) + (count == 1 ? " game" : " games");
        const bool custom = Skin::HasSystemImage(systems[static_cast<std::size_t>(row)].id);
        Skin::CarouselText text;
        text.name = systems[static_cast<std::size_t>(row)].name;
        text.detail = detail;
        text.status = system_moving ? "Moving: use the arrows, then A" : custom ? "Showing your picture" : "Default look";
        text.status_ok = !system_moving;
        text.has_picture = custom;
        text.picture_button = IsPictureEditingEnabled();
        text.moving = system_moving;
        Skin::DrawSystemsCarousel(c, SkinFonts(), cards, systems_anim, systems_sel, text, SystemsAccent(), AnimTime());
        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else if (system_moving) {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            hx += DrawHint(c, hx, hy, "Up Down", "Move") + 22;
            hx += DrawHint(c, hx, hy, "A", "Done") + 22;
            DrawHint(c, hx, hy, "B", "Cancel");
        } else {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            hx += DrawHint(c, hx, hy, "A", "Games") + 22;
            hx += DrawHint(c, hx, hy, "+", "Menu") + 22;
            if (IsPictureEditingEnabled()) {
                hx += DrawHint(c, hx, hy, "Y", "Picture") + 22;
                if (custom) {
                    hx += DrawHint(c, hx, hy, "X", "Default") + 22;
                }
            }
            DrawHint(c, hx, hy, "B", "Back");
        }
        if (system_menu_open) {
            DrawSystemMenu(c);
        }
    }

    // The + menu over the carousel: one entry, Move Placement.
    Rect SystemMenuRect() const {
        const int w = 380, h = 150;
        return {(g_screen_w - w) / 2, (g_screen_h - h) / 2 - 20, w, h};
    }
    Rect SystemMenuRowRect() const {
        const Rect p = SystemMenuRect();
        return {p.x + 16, p.y + 44, p.w - 32, 52};
    }

    void DrawSystemMenu(Canvas& c) {
        const float e = EaseOut(system_menu_anim);
        Skin::DrawScrim(c, 0.7f * e);
        Canvas::FadeScope fade{c, e};
        Canvas::OffsetScope rise{c, 0, static_cast<int>(std::lround(12.0f * (1.0f - e)))};
        const Rect p = SystemMenuRect();
        Skin::DrawPanel(c, p.x, p.y, p.w, p.h, 22);
        const std::string& name = Art::Systems()[static_cast<std::size_t>(SystemRowAt(systems_sel))].name;
        g_font.Draw(c, p.x + 24, p.y + 30, g_font.Truncate(name, 16, p.w - 48), 16, kColTextDim);
        const Rect r = SystemMenuRowRect();
        Skin::DrawRow(c, r.x, r.y, r.w, r.h, true);
        g_font_bold.Draw(c, r.x + 24, CenterBaseline(r.y, r.h, 21), "Move Placement", 21, kColText);
        int hx = p.x + 24;
        const int hy = p.y + p.h - 34;
        hx += DrawHint(c, hx, hy, "A", "Select") + 22;
        DrawHint(c, hx, hy, "B", "Close");
    }

    // A console's games: its card and name on the left, the games in a column on the right.
    void DrawSystemGames(Canvas& c) {
        const int row = SystemRowAt(systems_sel);
        const Skin::SystemCard card = CardFor(row);
        const std::string& name = Art::Systems()[static_cast<std::size_t>(row)].name;
        const int count = static_cast<int>(system_games.size());

        constexpr int kCardS = 220;
        const int card_x = (kGameListX - kCardS) / 2 - 10;
        const int card_y = kContentTop + 40;
        Skin::DrawSystemCard(c, SkinFonts(), card, card_x, card_y, kCardS);
        const std::string title = g_font_bold.Truncate(name, 28, kGameListX - 60);
        g_font_bold.Draw(c, card_x + (kCardS - g_font_bold.Measure(title, 28)) / 2, card_y + kCardS + 50, title, 28,
                         kColText);
        const std::string games_text = count == 0 ? "No games yet" : std::to_string(count) + (count == 1 ? " game" : " games");
        g_font.Draw(c, card_x + (kCardS - g_font.Measure(games_text, 18)) / 2, card_y + kCardS + 80, games_text, 18,
                    kColTextDim);

        const int list_w = g_screen_w - kGameListX - 56;
        if (count == 0) {
            const std::string where = "Put them in sdmc:/roms/" + std::string{card.id} + "/";
            const int mid = GameListTop() + (GameListBottom() - GameListTop()) / 2;
            const char* none = "No games here yet";
            g_font_bold.Draw(c, kGameListX + (list_w - g_font_bold.Measure(none, 24)) / 2, mid - 8, none, 24, kColText);
            g_font.Draw(c, kGameListX + (list_w - g_font.Measure(where, 18)) / 2, mid + 24, where, 18, kColTextDim);
        } else {
            const float scroll = system_games_scroll.x;
            const int top = GameListTop();
            Canvas::ClipScope clip{c, kGameListX - 30, top - 26, list_w + 60, GameListBottom() - top + 52};
            const int first = std::max(0, static_cast<int>(std::floor(scroll / kGameRowStep)) - 1);
            const int last = std::min(count - 1, first + GameListVisibleRows() + 2);
            for (int i = first; i <= last; ++i) {
                const GameEntry& game = games[static_cast<std::size_t>(system_games[static_cast<std::size_t>(i)])];
                const int y = top + static_cast<int>(std::lround(i * kGameRowStep - scroll));
                const float focus_k = std::clamp(1.0f - std::fabs(system_games_cursor.x - i), 0.0f, 1.0f);
                std::string detail = game.publisher.empty() ? std::string{SystemName(game.system)} : game.publisher;
                if (game.installed) {
                    detail += "  -  Installed";
                }
                if (game.encrypted) {
                    detail += "  -  Needs keys";
                }
                // Rows fade as they slide under the edges of the list.
                float out = 0.0f;
                if (y < top) {
                    out = static_cast<float>(top - y) / kGameRowStep;
                } else if (y + kGameRowH > GameListBottom()) {
                    out = static_cast<float>(y + kGameRowH - GameListBottom()) / kGameRowStep;
                }
                Canvas::FadeScope fade{c, std::clamp(1.0f - out, 0.0f, 1.0f)};
                Skin::DrawGameRow(c, SkinFonts(), TileFor(game), detail, kGameListX, y, list_w, kGameRowH,
                                  content_focus_for_list() ? focus_k : 0.0f);
            }
            if (count > GameListVisibleRows()) {
                const int track_h = GameListBottom() - top;
                const float total = static_cast<float>(count * kGameRowStep - 10);
                const int thumb_h = std::max(28, static_cast<int>(track_h * track_h / total));
                const float max_scroll = std::max(1.0f, total - track_h);
                const int thumb_y = top + static_cast<int>((track_h - thumb_h) * std::clamp(scroll / max_scroll, 0.0f, 1.0f));
                Skin::DrawScrollbar(c, g_screen_w - 26, top, track_h, thumb_y, thumb_h);
            }
        }
        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            if (count > 0) {
                hx += DrawHint(c, hx, hy, "A", "Play") + 22;
            }
            DrawHint(c, hx, hy, "B", "Consoles");
        }
    }

    bool content_focus_for_list() const {
        return focus == Focus::Content;
    }

    void DrawScrollbar(Canvas& c, const Grid&) {
        const float view_h = HomeViewH();
        if (home_height + 14.0f <= view_h) {
            return;
        }
        const int top = HomeViewTop();
        const int track_h = static_cast<int>(view_h);
        const int thumb_h = std::max(24, static_cast<int>(track_h * view_h / (home_height + 14.0f)));
        const float max_scroll = home_height + 14.0f - view_h;
        const int thumb_y = top + static_cast<int>((track_h - thumb_h) * std::clamp(grid_scroll.x / max_scroll, 0.0f, 1.0f));
        Skin::DrawScrollbar(c, g_screen_w - 12, top, track_h, thumb_y, thumb_h);
    }

    static constexpr int kRowH = 41;

    // Scroll the settings window now that it has overflowed the size of the screen.
    static constexpr int kSettingsTop = kTabStripTop + kTabStripH + 12;
    static constexpr int kSettingsRowStride = kRowH + 8;
    static constexpr int kSettingsFooterH = 52;

    static int SettingsVisibleRows() {
        return std::max(1,
                        (ContentBottom() - kSettingsTop - kSettingsFooterH) / kSettingsRowStride);
    }

    // The controller-mapping modal covers most of the screen and scrolls its own list.
    static constexpr int kRemapRowH = 42;
    static constexpr int kRemapTopPad = 92;    // Room for the title.
    static constexpr int kRemapBottomPad = 56; // Room for the button hints.
    static constexpr int kRemapPanelY = 44;

    static int RemapW() {
        return std::min(860, g_screen_w - 48);
    }

    static int RemapPanelH() {
        return g_screen_h - 2 * kRemapPanelY;
    }

    static int RemapVisibleRows() {
        return std::max(1, (RemapPanelH() - kRemapTopPad - kRemapBottomPad) / kRemapRowH);
    }

    void DrawSettingsTabs(Canvas& c) {
        if (settings_search_open) {
            const std::string name = "Search: " + settings_search_query;
            const int baseline = CenterBaseline(kTabStripTop, kTabStripH, 18);
            const int w = std::min(ContentW() - 48, g_font.Measure(name, 18) + kTabPadX * 2);
            const int x = kContentX + (ContentW() - w) / 2;
            Skin::DrawPill(c, x, kTabStripTop, w, kTabStripH, true);
            g_font.Draw(c, x + kTabPadX, baseline,
                        g_font.Truncate(name, 18, w - kTabPadX * 2), 18, kColOnAccent);
            return;
        }
        if (CompactTabStrip() || PerGameOpen()) {
            const char* name = CategoryName(settings_page);
            const int baseline = CenterBaseline(kTabStripTop, kTabStripH, 18);
            const int w = g_font.Measure(name, 18) + kTabPadX * 2;
            const int x = kContentX + (ContentW() - w) / 2;
            Skin::DrawPill(c, x, kTabStripTop, w, kTabStripH, true);
            g_font.Draw(c, x + kTabPadX, baseline, name, 18, kColOnAccent);
            g_font.Draw(c, x - 24, baseline, "<", 18, kColTextDim);
            g_font.Draw(c, x + w + 14, baseline, ">", 18, kColTextDim);
            return;
        }
        const auto rects = SettingsTabRects();
        for (int i = 0; i < NumCategories; ++i) {
            const bool on = i == static_cast<int>(settings_page);
            Skin::DrawPill(c, rects[i].x, kTabStripTop, rects[i].w, kTabStripH, on);
            const char* name = CategoryName(static_cast<Category>(i));
            g_font.Draw(c, rects[i].x + SettingsTabTextInset(rects, i),
                        CenterBaseline(kTabStripTop, kTabStripH, 18), name, 18,
                        on ? kColOnAccent : kColTextDim);
        }
    }

    void DrawSettingsPage(Canvas& c) {
        DrawSettingsTabs(c);
        if (ThemesPageShown()) {
            DrawThemes(c);
            return;
        }

        const bool content_focus = focus == Focus::Content;
        const int count = static_cast<int>(settings_rows.size());
        const int sel = SettingsSel();
        const int scroll = SettingsScroll();
        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        const int last = std::min(count, scroll + SettingsVisibleRows());
        for (int i = scroll; i < last; ++i) {
            const SettingsRow& row = settings_rows[i];
            const int y = kSettingsTop + (i - scroll) * kSettingsRowStride;
            if (row.is_header) {
                const int baseline = CenterBaseline(y, kRowH, 17);
                const int label_w = g_font.Measure(row.label, 17);
                g_font.Draw(c, x + 20, baseline, row.label, 17, kColAccent);
                const int rule_x = x + 20 + label_w + 12;
                c.FillRect(rule_x, y + kRowH / 2 - 1, std::max(0, x + w - 24 - rule_x), 1,
                           kColRail);
                continue;
            }
            const bool on = i == sel;
            if (on) {
                Skin::DrawRow(c, x, y, w, kRowH, content_focus);
            }
            g_font.Draw(c, x + 20, CenterBaseline(y, kRowH, 22), row.label, 22, kColText);
            const bool overridden = row.using_global && !row.using_global();
            const int value_inset = PerGameOpen() ? 40 : 24;
            const std::string value = row.value();
            const int max_value_w = w - 20 - value_inset - g_font.Measure(row.label, 22);
            const std::string shown = g_font.Truncate(value, 22, std::max(60, max_value_w));
            const int vw = g_font.Measure(shown, 22);
            const u32 value_color = on && content_focus ? kColAccent
                                    : overridden        ? kColText
                                                        : kColTextDim;
            g_font.Draw(c, x + w - value_inset - vw, CenterBaseline(y, kRowH, 22), shown, 22,
                        value_color);
            if (overridden) {
                c.FillRoundRect(x + w - 26, y + kRowH / 2 - 5, 10, 10, 5, kColAccent);
            }
        }
        DrawListScrollbar(c, g_screen_w - 20, kSettingsTop, SettingsVisibleRows(),
                          kSettingsRowStride, count, scroll);

        const int footer_y = ContentBottom() - kSettingsFooterH;
        const bool has_sel = sel >= 0 && sel < count && !settings_rows[sel].is_header;
        const std::string description = has_sel ? settings_rows[sel].description : std::string{};
        g_font.Draw(c, x + 20, footer_y + 16, g_font.Truncate(description, 18, w - 40), 18,
                    kColText);
        const bool page_view = !PerGameOpen() && !settings_search_open;
        const int hidden = page_view ? HiddenEntryCount(settings_page) : 0;
        std::string note;
        if (has_sel && settings_rows[sel].needs_restart) {
            note = "Takes effect the next time you launch a game.";
        } else if (hidden > 0) {
            note = std::to_string(hidden) +
                   (hidden == 1 ? " technical setting is" : " technical settings are") +
                   " hidden here. X shows them.";
        } else {
            note = std::string{"Graphics backend: "} + ActiveGraphicsBackendName();
        }
        if (PerGameOpen()) {
            const bool overridden =
                has_sel && settings_rows[sel].using_global && !settings_rows[sel].using_global();
            note = overridden ? "Set for this game only." : "Following the global setting.";
        }
        g_font.Draw(c, x + 20, footer_y + 42, note, 18, kColTextDim);

        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = g_screen_h - 44;
            const bool modal = has_sel && settings_rows[sel].modal != SettingsModal::None;
            if (modal) {
                hx += DrawHint(c, hx, hy, "A", "Configure") + 22;
            } else if (has_sel && settings_rows[sel].boolean) {
                hx += DrawHint(c, hx, hy, "A", "Toggle") + 22;
            } else {
                hx += DrawHint(c, hx, hy, "<>", "Change") + 22;
            }
            if (PerGameOpen() && has_sel && settings_rows[sel].set_global) {
                hx += DrawHint(c, hx, hy, "X",
                               settings_rows[sel].using_global() ? "Override" : "Use Global") + 22;
            }
            if (page_view && ExpertEntryCount(settings_page) > 0) {
                hx += DrawHint(c, hx, hy, "X",
                               IsShowAllSettingsEnabled() ? "Show Less" : "Show All") +
                      22;
            }
            hx += DrawHint(c, hx, hy, "Y", "Search") + 22;
            if (!settings_search_open) {
                hx += DrawHint(c, hx, hy, "L R", "Page") + 22;
            }
            hx += DrawHint(c, hx, hy, "B",
                           settings_search_open ? "Back" : PerGameOpen() ? "Done" : "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    // ---- Settings > Themes: every theme as a small picture of the menu in its colours ----------

    static constexpr int kThemeCols = 5;
    static constexpr int kThemeGapX = 18;
    static constexpr int kThemeGapY = 18;

    bool ThemesPageShown() const {
        return tab == Tab::Settings && settings_page == Category::Themes && !settings_search_open &&
               !PerGameOpen();
    }

    int ThemeCardW() const {
        return (ContentW() - 48 - (kThemeCols - 1) * kThemeGapX) / kThemeCols;
    }

    int ThemeCardH() const {
        return ThemeCardW() * 9 / 16 + 34;
    }

    int ThemeRowsVisible() const {
        const int space = ContentBottom() - kSettingsFooterH - kSettingsTop - 8;
        return std::max(1, (space + kThemeGapY) / (ThemeCardH() + kThemeGapY));
    }

    Rect ThemeCardRect(int index) const {
        const int row = index / kThemeCols - theme_scroll;
        const int col = index % kThemeCols;
        return {kContentX + 24 + col * (ThemeCardW() + kThemeGapX),
                kSettingsTop + 8 + row * (ThemeCardH() + kThemeGapY), ThemeCardW(), ThemeCardH()};
    }

    void ScrollThemeIntoView() {
        const int rows = (MenuThemeCount() + kThemeCols - 1) / kThemeCols;
        const int visible = ThemeRowsVisible();
        const int row = theme_sel / kThemeCols;
        if (row < theme_scroll) {
            theme_scroll = row;
        } else if (row >= theme_scroll + visible) {
            theme_scroll = row - visible + 1;
        }
        theme_scroll = std::clamp(theme_scroll, 0, std::max(0, rows - visible));
    }

    // Puts the theme on right away; the settings file is written when the page is left.
    void UseTheme(int index) {
        if (index == GetMenuTheme()) {
            return;
        }
        SetMenuTheme(index);
        settings_dirty = true;
    }

    void HandleThemes(u64 down, u32 nav) {
        const int count = MenuThemeCount();
        if (nav & DirLeft) {
            theme_sel = std::max(0, theme_sel - 1);
        }
        if (nav & DirRight) {
            theme_sel = std::min(count - 1, theme_sel + 1);
        }
        if (nav & DirUp) {
            theme_sel = std::max(theme_sel % kThemeCols, theme_sel - kThemeCols);
        }
        if (nav & DirDown) {
            theme_sel = std::min(count - 1, theme_sel + kThemeCols);
        }
        ScrollThemeIntoView();
        if (down & HidNpadButton_A) {
            UseTheme(theme_sel);
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
    }

    void DrawThemes(Canvas& c) {
        ScrollThemeIntoView();
        const int count = MenuThemeCount();
        const bool content_focus = focus == Focus::Content;
        const float t = static_cast<float>(NowSeconds());
        const int first = theme_scroll * kThemeCols;
        const int last = std::min(count, first + ThemeRowsVisible() * kThemeCols);
        for (int i = first; i < last; ++i) {
            const Rect r = ThemeCardRect(i);
            Skin::DrawThemeCard(c, SkinFonts(), r.x, r.y, r.w, r.h, i,
                                content_focus && i == theme_sel, i == GetMenuTheme(), t);
        }
        const int rows = (count + kThemeCols - 1) / kThemeCols;
        DrawListScrollbar(c, g_screen_w - 20, kSettingsTop, ThemeRowsVisible(),
                          ThemeCardH() + kThemeGapY, rows, theme_scroll);

        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        const int footer_y = ContentBottom() - kSettingsFooterH;
        const std::string name = MenuThemeName(theme_sel);
        const std::string line = theme_sel == GetMenuTheme()
                                     ? name + " is the theme in use."
                                     : name + ". Press A to use it.";
        g_font.Draw(c, x + 20, footer_y + 16, g_font.Truncate(line, 18, w - 40), 18, kColText);
        g_font.Draw(c, x + 20, footer_y + 42,
                    std::to_string(count) +
                        " themes. They colour the menus and the in-game quick menu.",
                    18, kColTextDim);

        if (focus == Focus::Rail) {
            DrawRailHints(c);
            return;
        }
        int hx = HintX();
        const int hy = g_screen_h - 44;
        hx += DrawHint(c, hx, hy, "A", "Use") + 22;
        hx += DrawHint(c, hx, hy, "Y", "Search") + 22;
        hx += DrawHint(c, hx, hy, "L R", "Page") + 22;
        hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
        DrawHint(c, hx, hy, "+ -", "Exit");
    }

    void DrawConfirm(Canvas& c) {
        const int body = 86 + 26 * static_cast<int>(confirm->lines.size());
        const int note_y = confirm->note.empty() ? body : body + 8;
        const int hint_y = note_y + (confirm->note.empty() ? 4 : 30);
        const int h = hint_y + 38;
        const int w = std::min(620, g_screen_w - 48);
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        g_font.Draw(c, x + 24, y + 46, confirm->title, 24, kColText);
        int line_y = y + 86;
        for (const std::string& line : confirm->lines) {
            g_font.Draw(c, x + 24, line_y, line, 18, kColTextDim);
            line_y += 26;
        }
        if (!confirm->note.empty()) {
            g_font.Draw(c, x + 24, y + note_y, confirm->note, 18, kColAccent);
        }

        int hx = x + 24;
        const int hy = y + hint_y;
        hx += DrawHint(c, hx, hy, "A", confirm->accept) + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    // A centred modal to choose which bundle of settings the reset row applies.
    void DrawPresetPicker(Canvas& c) {
        const int w = std::min(620, g_screen_w - 48);
        constexpr int row_h = 58;
        constexpr int top_pad = 78;    // Room for the title and subtitle.
        constexpr int bottom_pad = 82; // Room for the disclaimer and the button hints.
        const int h = top_pad + NumSettingsPresets * row_h + bottom_pad;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        g_font.Draw(c, x + 24, y + 40, "Reset All Settings", 24, kColText);
        g_font.Draw(c, x + 24, y + 64, "Choose the settings to reset to", 16, kColTextDim);

        for (int i = 0; i < NumSettingsPresets; ++i) {
            const int ry = y + top_pad + i * row_h;
            const int rx = x + 16;
            const int rw = w - 32;
            if (i == preset_sel) {
                Skin::DrawRow(c, rx, ry, rw, row_h - 4, true);
            }
            const auto preset = static_cast<SettingsPreset>(i);
            g_font.Draw(c, rx + 20, ry + 24, SettingsPresetName(preset), 20, kColText);
            g_font.Draw(c, rx + 20, ry + 46, SettingsPresetSummary(preset), 16, kColTextDim);
        }

        g_font.Draw(c, x + 24, y + h - 62,
                    "Performance and Ultra Performance can break some games.", 16, kColAccent);

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Choose") + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    // A centred modal to choose the profile's country from the console's full list.
    void DrawCountryPicker(Canvas& c) {
        const std::vector<CountryOption>& options = CountryOptions();
        const int count = static_cast<int>(options.size());
        const int w = std::min(620, g_screen_w - 48);
        constexpr int top_pad = 78;    // Room for the title and subtitle.
        constexpr int bottom_pad = 56; // Room for the button hints.
        const int h = top_pad + kCountryRows * kCountryRowH + bottom_pad;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        g_font.Draw(c, x + 24, y + 40, "Country", 24, kColText);
        g_font.Draw(c, x + 24, y + 64, "Where the console reports it is being used", 16,
                    kColTextDim);

        const int selected_code = GetProfileCountry();
        for (int i = country_scroll; i < std::min(count, country_scroll + kCountryRows); ++i) {
            const int ry = y + top_pad + (i - country_scroll) * kCountryRowH;
            const int rx = x + 16;
            const int rw = w - 32;
            if (i == country_sel) {
                Skin::DrawRow(c, rx, ry, rw, kCountryRowH - 4, true);
            }
            const bool current = options[i].code == selected_code;
            g_font.Draw(c, rx + 20, CenterBaseline(ry, kCountryRowH - 4, 20),
                        g_font.Truncate(options[i].name, 20, rw - 140), 20,
                        current ? kColAccent : kColText);
            // Countries outside the configured region are still selectable, just flagged.
            if (!IsCountryValidForRegion(options[i].code)) {
                const char* note = "wrong region";
                g_font.Draw(c, rx + rw - 24 - g_font.Measure(note, 16),
                            CenterBaseline(ry, kCountryRowH - 4, 16), note, 16, kColTextDim);
            }
        }
        DrawListScrollbar(c, x + w - 8, y + top_pad, kCountryRows, kCountryRowH, count,
                          country_scroll);

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Choose") + 22;
        hx += DrawHint(c, hx, hy, "L R", "Page") + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    // A centred modal to choose which layouts R3 cycles through in-game.
    void DrawLayoutPicker(Canvas& c) {
        const int count = GetScreenLayoutCount();
        const int w = std::min(620, g_screen_w - 48);
        constexpr int row_h = 44;
        constexpr int top_pad = 78;    // Room for the title and subtitle.
        constexpr int bottom_pad = 56; // Room for the button hints.
        const int h = top_pad + count * row_h + bottom_pad;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        Skin::DrawModal(c, x, y, w, h);

        g_font.Draw(c, x + 24, y + 40, "R3 Screen Layouts", 24, kColText);
        g_font.Draw(c, x + 24, y + 64, "Choose which layouts R3 cycles through in-game", 16,
                    kColTextDim);

        for (int i = 0; i < count; ++i) {
            const int ry = y + top_pad + i * row_h;
            const int rx = x + 16;
            const int rw = w - 32;
            if (i == layout_picker_sel) {
                Skin::DrawRow(c, rx, ry, rw, row_h - 4, true);
            }
            const bool enabled = (GetLayoutCycleMask() & (1u << i)) != 0;
            g_font.Draw(c, rx + 20, CenterBaseline(ry, row_h - 4, 20), GetScreenLayoutName(i), 20,
                        kColText);
            const char* state = enabled ? "On" : "Off";
            const int sw = g_font.Measure(state, 20);
            g_font.Draw(c, rx + rw - 24 - sw, CenterBaseline(ry, row_h - 4, 20), state, 20,
                        enabled ? kColAccent : kColTextDim);
        }

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Toggle") + 22;
        DrawHint(c, hx, hy, "B", "Done");
    }

    // A near-fullscreen modal that allows rebinding inputs.
    void DrawRemapPage(Canvas& c) {
        const int x = (g_screen_w - RemapW()) / 2;
        const int y = kRemapPanelY;
        const int w = RemapW();
        const int h = RemapPanelH();
        Skin::DrawModal(c, x, y, w, h);

        g_font.Draw(c, x + 24, y + 40, "Controller Mapping", 24, kColText);
        g_font.Draw(c, x + 24, y + 66,
                    "Controller changes apply the next time you launch a game.",
                    16, kColTextDim);

        const int list_top = y + kRemapTopPad;
        const int rx = x + 16;
        const int rw = w - 32;
        const int last = std::min(NumMappableControls, remap_scroll + RemapVisibleRows());
        for (int i = remap_scroll; i < last; ++i) {
            const int ry = list_top + (i - remap_scroll) * kRemapRowH;
            const bool on = i == remap_sel;
            if (on) {
                Skin::DrawRow(c, rx, ry, rw, kRemapRowH - 4, true);
            }
            const auto control = static_cast<MappableControl>(i);
            g_font.Draw(c, rx + 20, CenterBaseline(ry, kRemapRowH - 4, 20), ControlName(control), 20,
                        kColText);
            const char* value = PhysicalButtonName(GetMapping(control));
            const int vw = g_font.Measure(value, 20);
            g_font.Draw(c, rx + rw - 24 - vw, CenterBaseline(ry, kRemapRowH - 4, 20), value, 20,
                        on ? kColAccent : kColTextDim);
        }
        DrawListScrollbar(c, x + w - 12, list_top, RemapVisibleRows(), kRemapRowH,
                          NumMappableControls, remap_scroll);

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "<>", "Change") + 22;
        hx += DrawHint(c, hx, hy, "A", "Next") + 22;
        hx += DrawHint(c, hx, hy, "X", "Unbind") + 22;
        hx += DrawHint(c, hx, hy, "Y", "Default") + 22;
        DrawHint(c, hx, hy, "B", "Back");
    }

    void DrawHintBar(Canvas& c) {
        Skin::DrawHintBar(c);
    }

    // Legend shown while the cursor sits on the Library/Settings rail.
    void DrawRailHints(Canvas& c) {
        int hx = HintX();
        const int hy = g_screen_h - 44;
        hx += DrawHint(c, hx, hy, "<>", "Move") + 22;
        hx += DrawHint(c, hx, hy, "A", "Open") + 22;
        hx += DrawHint(c, hx, hy, "B", "Back") + 22;
        DrawHint(c, hx, hy, "+ -", "Exit");
    }

    // Full-frame busy indicator for the brief blocking scans.
    void ShowBusy(std::string_view msg) {
        const std::string text{msg};
        Frame([&](Canvas& c) { DrawBusy(c, text); });
    }

    void DrawBusy(Canvas& c, std::string_view msg) {
        Skin::DrawScrim(c, 0.8f);
        const int tw = g_font.Measure(msg, 22);
        const int w = tw + 104, h = 72;
        const int x = (g_screen_w - w) / 2, y = (g_screen_h - h) / 2;
        Skin::DrawPanel(c, x, y, w, h, h / 2);
        Skin::DrawSpinner(c, x + 38.0f, y + h / 2.0f, 12.0f, AnimTime());
        g_font.Draw(c, x + 68, CenterBaseline(y, h, 22), msg, 22, kColText);
    }

    void EnsureFramebuffer() {
        if (fb_ready) {
            return;
        }
        if (g_splash_fb_ready) {
            // The loading screen's buffers carry straight on, so nothing flashes in between.
            fb = g_splash_fb;
            g_splash_fb_ready = false;
            fb_ready = true;
            return;
        }
        // A game's swapchain that wasn't torn down completely leaves its buffers on the window,
        // and then the window takes no new ones, so whatever is there is released first. If the
        // framebuffer still can't be made, the frame is skipped and this runs again next frame.
        NWindow* window = nwindowGetDefault();
        nwindowReleaseBuffers(window);
        // Three buffers so a slow frame never stalls on the display; drawn straight into
        // their block-linear layout by RenderFrame().
        const Result rc =
            framebufferCreate(&fb, window, kPanelW, kPanelH, PIXEL_FORMAT_RGBA_8888, 3);
        if (R_FAILED(rc)) {
            std::printf("Menu framebuffer creation failed: 0x%x\n", static_cast<unsigned>(rc));
            fb = Framebuffer{};
            return;
        }
        fb_ready = true;
    }

    // Picks up a rotation changed from the Settings tab and resizes the canvas to match.
    void ApplyRotation() {
        const int rotation = GetMenuRotation();
        if (rotation == g_rotation && canvas.Width() == g_screen_w) {
            return;
        }
        g_rotation = rotation;
        g_screen_w = RotatedUpright() ? kPanelH : kPanelW;
        g_screen_h = RotatedUpright() ? kPanelW : kPanelH;
        canvas.Resize(g_screen_w, g_screen_h);
        ScrollSelectionsIntoView();
    }

    void ScrollSelectionsIntoView() {
        EnsureVisible(ComputeGrid());
        ScrollSettingsIntoView();
        ScrollRemapIntoView();
    }

    void DrawLoading() {
        PrepareFrame();
        RenderFrame([&](Canvas& c) { DrawStartupScene(c, "Finding your games..."); });
    }

    // Reads the library on a worker while the loading screen keeps moving.
    void LoadLibrary() {
        std::vector<GameEntry> scanned;
        std::atomic<bool> finished{false};
        std::thread worker([&scanned, &finished] {
            scanned = ScanGames();
            finished = true;
        });
        while (!finished.load() && appletMainLoop()) {
            DrawLoading();
            // Leave the scan most of the CPU; the spinner doesn't need every frame.
            svcSleepThread(10'000'000);
        }
        worker.join();
        games = std::move(scanned);
        Art::LoadGameArt(games);
        paths = GetPaths();
        RefreshRomsDir2Presence();
        ApplyFilter();
    }

    Canvas canvas;
    bool touch_was_down = false;
    // Set by a second touch on the focused tile so Run() can escape the loop.
    std::optional<std::string> pending_launch;
};

} // namespace

MenuResult RunMenu(PadState& pad) {
    if (!EnsureMenuGraphics()) {
        // Exit on no font found.
        return {MenuAction::Exit, {}};
    }
    // The last game may have written to the CFG savegame the System page reads.
    RefreshSystemSettings();
    // The menu draws on the CPU: raise its clock for as long as the menu is up.
    const Common::Horizon::CpuBoostScope boost;
    static bool profile_loaded = false;
    if (!profile_loaded) {
        psmInitialize();
        g_nickname = Art::LoadProfile();
        profile_loaded = true;
    }
    // Decoded in the background; only pictures that changed are read again.
    Art::LoadSystemArt();
    MenuResult result;
    {
        Menu menu;
        result = menu.Run(pad);
    }
    // Whatever is still loading can wait: the game gets the cores.
    Art::StopLoading();
    Covers::Stop();
    return result;
}

void ShowStartupScreen(std::string_view status) {
    if (!EnsureMenuGraphics()) {
        return;
    }
    ApplyMenuTheme();
    if (!g_splash_fb_ready) {
        if (R_FAILED(framebufferCreate(&g_splash_fb, nwindowGetDefault(), kPanelW, kPanelH,
                                       PIXEL_FORMAT_RGBA_8888, 3))) {
            g_splash_fb = Framebuffer{};
            return;
        }
        g_splash_fb_ready = true;
    }
    g_rotation = GetMenuRotation();
    g_screen_w = RotatedUpright() ? kPanelH : kPanelW;
    g_screen_h = RotatedUpright() ? kPanelW : kPanelH;
    static Canvas canvas;
    if (canvas.Width() != g_screen_w || canvas.Height() != g_screen_h) {
        canvas.Resize(g_screen_w, g_screen_h);
    }
    g_now = NowSeconds();
    Skin::BeginFrame(g_now, g_screen_w, g_screen_h);
    const std::string text{status};
    RenderToFramebuffer(canvas, g_splash_fb, [&](Canvas& c) { DrawStartupScene(c, text); });
}

void EndStartupScreen() {
    if (g_splash_fb_ready) {
        framebufferClose(&g_splash_fb);
        g_splash_fb_ready = false;
    }
}

void SetMenuNotice(const std::string& text, bool error) {
    ShowNotice(text, error);
}

void RequestMenuFadeIn() {
    g_menu_fade_start = kMenuFadeArmed;
}

void ShutdownMenu() {
    Covers::Stop();
    Workers::Get().Shutdown();
    Skin::TrimCaches();
    g_font.Shutdown();
    g_font_bold.Shutdown();
    g_font_mark.Shutdown();
}

} // namespace SwitchFrontend
