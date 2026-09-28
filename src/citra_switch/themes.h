// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace SwitchFrontend {

// The launcher's colour themes (Settings > Advanced > Theme). They're all dark: the console
// logos are made for a dark menu and the text stays white. Colours are 0xRRGGBB.
struct Theme {
    const char* name;
    std::uint32_t accent;    // focus, selection, highlights
    std::uint32_t accent2;   // the second accent: progress bars, the focus ring
    std::uint32_t accent3;   // the focus ring's third colour
    std::uint32_t bg_top;    // the backdrop at the top of the screen...
    std::uint32_t bg_bottom; // ...and at the bottom
    std::uint32_t glow;      // the big glow rising from a bottom corner
    float glow_k;            // its strength
    std::uint32_t glow2;     // the faint glow across the middle
    float ambient_k;         // how strongly the focused game's colour tints the backdrop
    std::uint32_t surface;   // glass panels and rows
    std::uint32_t text_dim;  // secondary text
};

inline constexpr std::array<Theme, 8> kThemes{{
    {"Midnight", 0x5EE7DF, 0x8B7CFF, 0xF07CD8, 0x090A10, 0x040508, 0x7640D6, 0.22f, 0x187896, 0.34f,
     0x2C2C3A, 0xA4A3BA},
    {"Graphite", 0x7AA2F7, 0xB4BCCB, 0x7AE0C3, 0x121214, 0x08080A, 0x4A5060, 0.20f, 0x2E3440, 0.26f,
     0x303036, 0xA8A8B0},
    {"Ocean", 0x4FC3F7, 0x3D7BFF, 0x2DD4BF, 0x07111D, 0x03070E, 0x1D4ED8, 0.26f, 0x0E7490, 0.30f,
     0x223249, 0x9DB2C9},
    {"Crimson", 0xFF6B6B, 0xFF9F43, 0xFFD166, 0x15090A, 0x080405, 0xB91C1C, 0.24f, 0x7C2D12, 0.28f,
     0x3A2729, 0xC4A5A8},
    {"Forest", 0x8BE36C, 0x34D399, 0xD9F99D, 0x08130C, 0x040805, 0x15803D, 0.24f, 0x0F766E, 0.28f,
     0x243A2C, 0xA3BCA9},
    {"Sunset", 0xFF9F43, 0xFF6FA8, 0xFFD166, 0x170B14, 0x0A0509, 0xC2410C, 0.24f, 0x9D174D, 0.28f,
     0x3A2A36, 0xC6AABA},
    {"Sakura", 0xFF8FC7, 0xC39BFF, 0xFFD1E8, 0x160A14, 0x09050A, 0xBE185D, 0.22f, 0x7E22CE, 0.28f,
     0x3A2838, 0xC9A9C0},
    {"OLED Black", 0xF2F2F2, 0xB0B0B0, 0x8A8A8A, 0x000000, 0x000000, 0x2A2A2A, 0.10f, 0x101010, 0.16f,
     0x1E1E1E, 0xA2A2A2},
}};

inline const Theme& ThemeAt(int index) {
    return kThemes[index >= 0 && static_cast<std::size_t>(index) < kThemes.size()
                       ? static_cast<std::size_t>(index)
                       : 0];
}

inline float ThemeChannel(std::uint32_t rgb, int shift) {
    return static_cast<float>((rgb >> shift) & 0xFF) / 255.0f;
}

// The in-game overlay's colours for a theme (RGB, 0..1): its accent, its glass panel and the
// dimming behind it.
inline void ThemeOverlayColors(int index, std::array<float, 3>& accent, std::array<float, 3>& panel,
                               std::array<float, 3>& scrim) {
    const Theme& t = ThemeAt(index);
    for (int i = 0; i < 3; ++i) {
        const int shift = 16 - 8 * i;
        accent[i] = ThemeChannel(t.accent, shift);
        // The panel is the theme's glass, darkened; the dimming leans towards its backdrop.
        panel[i] = ThemeChannel(t.surface, shift) * 0.4f;
        scrim[i] = ThemeChannel(t.bg_bottom, shift) * 0.6f + 0.01f;
    }
}

} // namespace SwitchFrontend
