// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace SwitchFrontend {

struct GameEntry;

// Pictures the user picks for systems and games (PNG, JPEG or WebP, any size).
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

// Decodes whatever is configured into the skin. Cheap to call again after a rescan.
void LoadSystemArt();
void LoadGameArt(const std::vector<GameEntry>& games);

// Sets (or, with an empty image path, clears) a picture and applies it right away.
// Returns an error message, or empty on success.
std::string SetSystemArt(const std::string& id, const std::string& image_path);
std::string SetGameArt(const GameEntry& game, const std::string& image_path);

} // namespace Art
} // namespace SwitchFrontend
