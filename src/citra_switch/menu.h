// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <switch.h>

// The console UI.
namespace SwitchFrontend {

enum class MenuAction {
    Launch, // MenuResult::path holds the ROM to boot.
    Exit,   // Quit the application.
};

struct MenuResult {
    MenuAction action{MenuAction::Exit};
    std::string path;
};

// Draws the library/settings menu on the default nwindow and blocks until the user
// launches a game or exits.
MenuResult RunMenu(PadState& pad);

// Puts the loading screen up (or updates it) with `status` under the app's name, before the
// menu exists; the menu carries on from it without a blank frame.
void ShowStartupScreen(std::string_view status);
// Takes the loading screen down if the menu didn't take it over (a game started straight away).
void EndStartupScreen();

// Queues a one-shot notice for the next RunMenu entry.
void SetMenuNotice(const std::string& text, bool error = true);

// Frees the font and shared-font resources cached across RunMenu calls.
void ShutdownMenu();

} // namespace SwitchFrontend
