// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "citra_switch/menu_gfx.h"

namespace SwitchFrontend {

struct GameEntry;

// Pictures the user picks for systems and games (PNG, JPEG or WebP, any size; the format is
// read from the file itself, so a misnamed file still works).
// Picked files are remembered in sdmc:/switch/emuswitch/art.ini; files dropped in
// sdmc:/switch/emuswitch/systems/<id>.<ext> or .../covers/<rom name>.<ext> work too.
namespace Art {

// Every system in the order the Systems page shows them: "3ds" first.
struct SystemInfo {
    std::string id;
    std::string name;
};
const std::vector<SystemInfo>& Systems();

bool IsImageFile(const std::string& name);

// Where a system's / game's picture comes from, or empty for the default.
std::string SystemArtPath(const std::string& id);
std::string GameArtPath(const GameEntry& game);
// Whether the player picked a picture for the game (in the menu, rather than a file in covers/).
bool HasPickedArt(const GameEntry& game);

// The console user's nickname; their avatar goes to the skin's profile bar.
std::string LoadProfile();

// Decodes whatever is configured into the skin. Game pictures load in the background and
// arrive through Pump(); ones that haven't changed since the last call aren't read again.
void LoadSystemArt();
void LoadGameArt(const std::vector<GameEntry>& games);

// Hands finished background loads to the skin. Call between frames. Returns true if anything
// changed.
bool Pump();
// Drops queued loads and waits for the loader, e.g. before a game starts.
void StopLoading();

// Sets (or, with an empty image path, clears) a picture and applies it right away.
// Returns an error message, or empty on success.
std::string SetSystemArt(const std::string& id, const std::string& image_path);
std::string SetGameArt(const GameEntry& game, const std::string& image_path);

// The picture picker's preview: ask for a file, then collect it (decoded in the background).
void RequestPreview(const std::string& path);
// True once the preview for `path` is ready; `img` is empty and `error` set if it can't be read.
bool TakePreview(const std::string& path, Gfx::Image& img, std::string& error);

} // namespace Art
} // namespace SwitchFrontend
