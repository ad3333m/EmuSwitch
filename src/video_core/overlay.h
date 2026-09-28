// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"

// A render-agnostic description of an on-screen overlay menu. The frontend fills it in from the
// input thread and the active renderer reads it back on the emulation thread to draw it on top of
// the game.
namespace VideoCore {

struct OverlayMenuItem {
    std::string label;
    std::string value;
    bool is_action{};
    bool is_header{};
};

struct OverlayMenuState {
    bool visible{};
    std::string title;
    std::vector<OverlayMenuItem> items;
    int selected{};
    std::string hint; // Footer help text.
    // A small panel at the bottom (or top) that leaves the game undimmed (the screen layout
    // editor).
    bool compact{};
    bool compact_top{};
    // Outlines both screens when >= 0, highlighting the top (0) or bottom (1) one.
    int outline_screen = -1;
};

// The launcher theme's colours for everything drawn over the game (RGB, 0..1): the accent, the
// glass panels and the dimming behind the quick menu.
struct OverlayTheme {
    std::array<float, 3> accent{0.37f, 0.91f, 0.87f};
    std::array<float, 3> panel{0.066f, 0.07f, 0.1f};
    std::array<float, 3> scrim{0.012f, 0.012f, 0.03f};
};
void SetOverlayTheme(const OverlayTheme& theme);
OverlayTheme GetOverlayTheme();

// Covers the whole picture in black while set, so the last frames before the game closes are
// black rather than a frozen picture.
void SetOverlayBlackout(bool enabled);
bool IsOverlayBlackout();

// Clockwise rotation the renderer applies to everything it draws over the game.
void SetOverlayRotation(u32 degrees);
u32 GetOverlayRotation();

// Publishes the latest overlay description (called from the input thread).
void SetOverlayMenuState(const OverlayMenuState& state);

// Snapshots the current overlay description (called from the renderer/emulation thread).
OverlayMenuState GetOverlayMenuState();

// Skip the copy above when nothing is shown.
bool IsOverlayMenuVisible();

// Shader-compilation activity.
void NotifyShaderCompileBegin();
void NotifyShaderCompileEnd();
u32 GetPendingShaderCompiles();

// A short-lived message shown over the game, used to report things the player asked for.
// Posting a new one replaces whatever is showing.
void PostOverlayToast(const std::string& text, u32 duration_ms = 2500);

// Returns the live toast text, or an empty string once it has expired.
std::string GetOverlayToast();

} // namespace VideoCore
