// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "citra_switch/menu_gfx.h"

namespace SwitchFrontend {

// Game pictures from SteamGridDB (steamgriddb.com), with the player's own API key. The menu
// searches for a game, shows the pictures SteamGridDB has for it and saves the one picked.
namespace SteamGrid {

enum class Stage {
    Idle,
    Searching, // finding the game and its pictures
    Ready,     // the pictures are listed; their thumbnails arrive one by one
    Saving,    // downloading the picked picture in full
    Saved,     // `saved` is the file it went to
    Failed,    // the search failed; `error` says why
};

struct Picture {
    int width = 0;
    int height = 0;
    std::string style;   // "alternate", "blurred", "no_logo", ...
    Gfx::Image thumb;    // empty until it has loaded (or if it couldn't be read)
    bool loaded = false; // the thumbnail has been tried
};

// What a search has come to so far.
struct Results {
    Stage stage = Stage::Idle;
    std::string game;  // the SteamGridDB game the pictures are for
    std::string error; // why the search, or the last download, failed
    std::string saved;
    std::vector<Picture> pictures; // square ones first, then portrait

    // Poll()'s bookkeeping.
    unsigned generation = ~0u;
    unsigned version = 0;
};

// A search term from a game's title: tags such as "(USA)" dropped, "Legend of Zelda, The -
// Minish Cap" turned into "The Legend of Zelda: Minish Cap".
std::string SearchTerm(const std::string& title);

// Starts looking up `term` in the background (stopping any earlier search).
void Search(const std::string& term, const std::string& api_key);
// Downloads picture `index` of the current search in full, to `dest_stem` plus its extension.
void Save(int index, const std::string& dest_stem);
// Brings `out` up to date; thumbnails that have arrived are moved into it. Returns true if
// anything changed.
bool Poll(Results& out);
// Stops the search and waits for it.
void Cancel();

} // namespace SteamGrid
} // namespace SwitchFrontend
