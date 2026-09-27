// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The skin's vector art (menu_skin_art.cpp): dock icons as coverage masks, and the shaded
// controller on the Systems page as cached layers tinted per frame.

#pragma once

#include <functional>
#include <memory>

#include "citra_switch/menu_gfx.h"

namespace SwitchFrontend::Skin::Shapes {

using Gfx::Canvas;
using Gfx::MakeColor;
using Gfx::u32;
using Gfx::u8;

enum class DockIconShape { Home, Controller, Download, Gear, Folder, Bolt, Picture };

// A `size` x `size` icon mask; built on first use when `may_build`, else null until then.
std::shared_ptr<const Gfx::Mask> IconMask(DockIconShape shape, int size, bool may_build);

// The controller is built once per width. `parallel(n, fn)` runs fn(0..n-1), on several cores
// if it can.
bool PadReady(int width);
void BuildPad(int width, const std::function<void(int, const std::function<void(int)>&)>& parallel);
// Draws a built controller centred on (cx, cy) with its shell in `accent`.
void DrawPad(Canvas& c, float cx, float cy, int width, u32 accent);

} // namespace SwitchFrontend::Skin::Shapes
