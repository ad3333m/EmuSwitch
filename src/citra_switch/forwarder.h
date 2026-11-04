// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

// HOME Menu forwarders: an icon on the console's HOME Menu that opens EmuSwitch, or a game in
// it, like any installed software. Built and installed on the console, the way Sphaira does it:
// a small loader (tools/forwarder_hbl, in the romfs as forwarder/) that starts EmuSwitch.nro
// with the game's path. Kept free of <switch.h> (see multi_handoff.cpp).
namespace SwitchFrontend::Forwarder {

struct Request {
    std::string name;   // the title on the HOME Menu
    std::string author; // the line under it
    std::string nro;    // EmuSwitch.nro, as an sdmc:/ path
    std::string rom;    // the game it opens, or empty for EmuSwitch's library
    // A 256x256 picture in Gfx::Image's pixel order, or null for EmuSwitch's own icon.
    const std::uint32_t* icon_rgba = nullptr;
};

// Builds the forwarder and installs it on the SD card, replacing one made before for the same
// game. Returns an error message, or empty on success.
std::string Install(const Request& request);

} // namespace SwitchFrontend::Forwarder
