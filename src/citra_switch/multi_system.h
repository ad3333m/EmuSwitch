// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SwitchFrontend {

struct GameEntry;

// The systems EmuSwitch plays through a bundled emulator rather than in-process.
// Each one's emulator ships in romfs:/emus/<id>.nro and is unpacked to
// sdmc:/switch/emuswitch/emus/ on first start.
namespace Multi {

struct System {
    const char* id;     // folder under sdmc:/roms
    const char* name;
    const char* badge;  // short label on library tiles
    const char* emu;    // emulator file: romfs:/emus/<emu>.nro (systems can share one)
    std::uint8_t r, g, b;
    std::vector<std::string> exts;   // accepted inside sdmc:/roms/<id>/
    std::vector<std::string> loose;  // recognised anywhere else (no clashes with other systems)
    bool takes_game;    // false: opens the emulator's own library (Wii U)
};

const std::vector<System>& Systems();

// Index into Systems() for a file, or -1 (a 3DS title, handled here). A system's own
// folder decides first, since .iso/.cue/.chd are shared by several disc systems.
int SystemForPath(const std::string& path);

// Unpacks the bundled emulators and sets up a PS2 BIOS on a background thread.
void StartSetup();

// Waits for the background setup; call before exiting.
void FinishSetup();

// Adds every non-3DS game under sdmc:/roms, plus 3DS games in sdmc:/roms/3ds.
void AddGames(std::vector<GameEntry>& games);

// Hands the console to the game's emulator; EmuSwitch then has to exit.
// Returns false with a reason in `error` when it can't.
bool Launch(const std::string& path, std::string& error);

// envSetNextLoad, kept in multi_handoff.cpp: <switch.h> clashes with the core's types.
bool HandOff(const std::string& nro, const std::string& argv);

}  // namespace Multi
}  // namespace SwitchFrontend
