// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// An in-game menu to change quick settings that apply on-the-fly.
namespace SwitchFrontend {

// What the caller should do after a navigation frame.
enum class QuickMenuAction {
    None,
    Close,
    ExitGame,
};

// One frame of navigation.
struct QuickMenuNav {
    bool up{};
    bool down{};
    bool left{};  // Steps a value so the caller feeds it only the d-pad.
    bool right{}; // ^
    bool confirm{};   // A
    bool cancel{};    // B
    bool alt{};       // X
    bool alt2{};      // Y
    bool tab_prev{};  // L
    bool tab_next{};  // R
    bool page_prev{}; // ZL
    bool page_next{}; // ZR
    // Held buttons and the sticks (-1..1, up positive), for the screen layout editor, which
    // moves things for as long as they're held.
    bool hold_up{};
    bool hold_down{};
    bool hold_left{};
    bool hold_right{};
    bool hold_l{};
    bool hold_r{};
    bool hold_zl{};
    bool hold_zr{};
    float stick_x{};
    float stick_y{};
    float rstick_x{};
    float rstick_y{};
};

// True while the overlay is showing.
bool IsQuickMenuOpen();

// Whether opening the overlay freezes the game.
bool IsPauseInQuickMenu();
void SetPauseInQuickMenu(bool enabled);

// Opens/closes the overlay saving any changed settings.
void OpenQuickMenu();
void CloseQuickMenu();
void ToggleQuickMenu();

// Applies one navigation frame, updates the live settings, and repaints the overlay.
QuickMenuAction UpdateQuickMenu(const QuickMenuNav& nav);

// Opens the screen layout editor over the game (the Display page's "Custom Screen Layout").
void BeginCustomLayoutEdit();

} // namespace SwitchFrontend
