// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace SwitchFrontend {

struct GameEntry;

// Box art for games that have no picture, from the libretro thumbnail library
// (thumbnails.libretro.com). Covers are saved to sdmc:/switch/emuswitch/covers/, where the
// menu's picture loader already looks, named after the game's file (or, for titles installed
// on the emulated SD card, after the title ID).
namespace Covers {

// One game to look for, gathered on the menu's thread.
struct Request {
    std::string library; // folder on the thumbnail server, e.g. "Nintendo - Game Boy Advance"
    std::string rom;     // the game's path
    std::string stem;    // its file name without the extension
    std::string title;
    std::string save_as; // cover file name without the extension
};

// The request for `game`, or false if it has no library or already has a picture of its own.
bool RequestFor(const GameEntry& game, Request& out);

// Starts looking up `requests` in the background (the ones with a cover on the SD card
// already are skipped). Does nothing while a lookup is running.
void Start(std::vector<Request> requests);
// True, once, after new covers have been saved; the menu reloads its pictures then.
bool TakeUpdates();
// After a lookup has finished: how many covers it saved (then 0 until the next one).
int TakeFinishedCount();
bool IsRunning();
// Stops the lookup and waits for it, e.g. before a game starts.
void Stop();

} // namespace Covers
} // namespace SwitchFrontend
