// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace SwitchFrontend::GameClocks {

// Runs the Switch's CPU at 1785 MHz while a game plays (the speed Horizon itself uses for
// loading screens) instead of the stock 1020 MHz, unless the setting is off. A higher clock
// already set by something else (sys-clk) is left alone.
void Begin();
// Puts the raised clock back if the system reset it (loading, docking, sleep). Cheap enough
// to call about once a second.
void Maintain();
// Hands the clocks back to the system's normal settings.
void End();

} // namespace SwitchFrontend::GameClocks
