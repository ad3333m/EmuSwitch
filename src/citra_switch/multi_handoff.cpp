// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The only EmuSwitch code that needs <switch.h>: its u128/Result/Service names clash
// with the emulator core's, so it stays in a file that includes nothing else.

#include <string>
#include <switch.h>

namespace SwitchFrontend::Multi {

bool HandOff(const std::string& nro, const std::string& argv) {
    return R_SUCCEEDED(envSetNextLoad(nro.c_str(), argv.c_str()));
}

}  // namespace SwitchFrontend::Multi
