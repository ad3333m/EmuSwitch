// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/game_clocks.h"

#include <cstdio>

#include <switch.h>

#include "citra_switch/config.h"

namespace SwitchFrontend::GameClocks {
namespace {

constexpr u32 kCpuHz = 1785000000;

bool g_active = false;
bool g_clkrst = false; // clkrst on 8.0.0 and later, pcv before that

bool GetCpuRate(u32& hz) {
    hz = 0;
    if (!g_clkrst) {
        return R_SUCCEEDED(pcvGetClockRate(PcvModule_CpuBus, &hz));
    }
    ClkrstSession session{};
    if (R_FAILED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) {
        return false;
    }
    const bool ok = R_SUCCEEDED(clkrstGetClockRate(&session, &hz));
    clkrstCloseSession(&session);
    return ok;
}

bool SetCpuRate(u32 hz) {
    if (!g_clkrst) {
        return R_SUCCEEDED(pcvSetClockRate(PcvModule_CpuBus, hz));
    }
    ClkrstSession session{};
    if (R_FAILED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) {
        return false;
    }
    const bool ok = R_SUCCEEDED(clkrstSetClockRate(&session, hz));
    clkrstCloseSession(&session);
    return ok;
}

} // namespace

void Begin() {
    if (g_active || !IsGameCpuBoostEnabled()) {
        return;
    }
    g_clkrst = hosversionAtLeast(8, 0, 0);
    const Result rc = g_clkrst ? clkrstInitialize() : pcvInitialize();
    if (R_FAILED(rc)) {
        // printf: the logging headers' u128 clashes with libnx's.
        std::printf("GameClocks: no clock service (0x%x); the CPU stays at its usual speed\n",
                    static_cast<unsigned>(rc));
        return;
    }
    g_active = true;
    Maintain();
}

void Maintain() {
    if (!g_active) {
        return;
    }
    u32 hz = 0;
    // Only ever raised: a faster clock someone else set stays.
    if (GetCpuRate(hz) && hz < kCpuHz && !SetCpuRate(kCpuHz)) {
        std::printf("GameClocks: couldn't raise the CPU clock\n");
    }
}

void End() {
    if (!g_active) {
        return;
    }
    g_active = false;
    if (g_clkrst) {
        clkrstExit();
    } else {
        pcvExit();
    }
    // Leaving boost mode makes the system apply its normal clocks again.
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

} // namespace SwitchFrontend::GameClocks
