// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// EmuSwitch's other systems. 3DS games run in-process as before; DS, GBA, Game
// Boy, PS2 and Wii U games are listed in the same library and handed to the
// emulator bundled for them (envSetNextLoad), which the console runs once
// EmuSwitch exits.

#include "citra_switch/multi_system.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <thread>

#include "citra_switch/emu_zip.h"
#include "citra_switch/menu_data.h"
#include "common/logging/log.h"

namespace SwitchFrontend::Multi {
namespace {

constexpr const char* ROOT = "sdmc:/switch/emuswitch";
constexpr const char* EMUS = "sdmc:/switch/emuswitch/emus";
constexpr const char* ROMS = "sdmc:/roms";
constexpr const char* PS2_ROOT = "sdmc:/switch/armsx2";
constexpr const char* PS2_BIOS_DIR = "sdmc:/switch/armsx2/bios";
constexpr const char* PS2_INI = "sdmc:/switch/armsx2/armsx2.ini";
constexpr std::int64_t PS2_BIOS_SIZE = 4 * 1024 * 1024;

std::thread s_setup;
std::mutex s_setup_mutex;
std::atomic<bool> s_setup_done{false};
std::string s_ps2_bios;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ExtOf(const std::string& name) {
    const size_t dot = name.rfind('.');
    const size_t slash = name.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return Lower(name.substr(dot + 1));
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::int64_t FileSize(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<std::int64_t>(st.st_size) : -1;
}

void MakeDirs(const std::string& path) {
    for (size_t i = 6; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
}

void Walk(const std::string& dir, int depth, const std::function<void(const std::string&, const std::string&)>& on_file) {
    if (depth < 0) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string p = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) subdirs.push_back(p);
        else on_file(p, e->d_name);
    }
    closedir(d);
    for (const auto& s : subdirs) Walk(s, depth - 1, on_file);
}

bool CopyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    const std::string tmp = to + ".part";
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    while (true) {
        const size_t n = fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(to.c_str());
    return rename(tmp.c_str(), to.c_str()) == 0;
}

std::string EmulatorPath(int system) {
    return std::string(EMUS) + "/" + Systems()[system].emu + ".nro";
}

// "Pokemon - Emerald Version (USA, Europe)" -> "Pokemon - Emerald Version".
std::string PrettyTitle(const std::string& file) {
    std::string t = file;
    const size_t dot = t.rfind('.');
    if (dot != std::string::npos) t = t.substr(0, dot);
    std::string out;
    int depth = 0;
    for (char c : t) {
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { if (depth) depth--; continue; }
        if (depth) continue;
        out += c == '_' ? ' ' : c;
    }
    std::string squeezed;
    for (char c : out)
        if (!(c == ' ' && (squeezed.empty() || squeezed.back() == ' '))) squeezed += c;
    while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
    return squeezed.empty() ? t : squeezed;
}

// ---- PS2 BIOS -----------------------------------------------------------------

int BiosRank(const std::string& name) {
    const std::string n = Lower(name);
    for (const char* k : {"usa", "(u)", "scph-39001", "scph-50001", "scph-70012", "scph-7000"})
        if (n.find(k) != std::string::npos) return 0;
    for (const char* k : {"europe", "eur", "(e)"})
        if (n.find(k) != std::string::npos) return 1;
    for (const char* k : {"japan", "(j)"})
        if (n.find(k) != std::string::npos) return 2;
    return 3;
}

bool IsBiosName(const std::string& name) {
    const std::string x = ExtOf(name);
    return x == "bin" || x == "rom0" || x == "rom";
}

void GatherBios() {
    MakeDirs(PS2_BIOS_DIR);
    for (const char* spot : {"sdmc:/roms/ps2", "sdmc:/bios", "sdmc:/switch/emuswitch/bios"}) {
        Walk(spot, 4, [](const std::string& p, const std::string& name) {
            const std::string x = ExtOf(name);
            if (x == "zip") {
                for (const ZipEntry& e : zipList(p)) {
                    if (e.size != PS2_BIOS_SIZE || !IsBiosName(e.name)) continue;
                    const std::string dest = std::string(PS2_BIOS_DIR) + "/" + BaseName(e.name);
                    if (FileSize(dest) != PS2_BIOS_SIZE) zipExtract(p, e, dest);
                }
            } else if (IsBiosName(name) && FileSize(p) == PS2_BIOS_SIZE) {
                const std::string dest = std::string(PS2_BIOS_DIR) + "/" + name;
                if (FileSize(dest) != PS2_BIOS_SIZE) CopyFile(p, dest);
            }
        });
    }
}

std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> lines;
    FILE* f = fopen(path, "rb");
    if (!f) return lines;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        lines.push_back(l);
    }
    fclose(f);
    return lines;
}

void WriteLines(const char* path, const std::vector<std::string>& lines) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Points armsx2.ini at the BIOS; a new ini gets ARMSX2-NX's own first-run defaults.
void ConfigurePs2(const std::string& bios) {
    MakeDirs(PS2_ROOT);
    std::vector<std::string> lines = ReadLines(PS2_INI);
    if (lines.empty()) {
        WriteLines(PS2_INI, {
            "[Filenames]", "BIOS = " + bios, "Game = ",
            "", "[EmuCore/GS]", "Renderer = 14", "AspectRatio = 4:3", "deinterlace_mode = 1",
            "FrameLimitEnable = false", "VsyncEnable = 0",
            "", "[SPU2/Output]", "Backend = Horizon",
            "", "[UI]", "EnableFullscreenUI = true",
            "", "[GameList]", "RecursivePaths = sdmc:/switch/armsx2/games", "RecursivePaths = sdmc:/roms/ps2",
            "", "[Achievements]", "Enabled = false",
            "", "[InputSources]", "SDL = false",
            "", "[Logging]", "EnableSystemConsole = true", "EnableFileLogging = true", "EnableVerbose = false",
        });
        return;
    }
    int section = -1, key = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = Trim(lines[i]);
        if (t.size() > 1 && t.front() == '[') {
            if (section >= 0) break;
            if (t == "[Filenames]") section = static_cast<int>(i);
        } else if (section >= 0 && t.rfind("BIOS", 0) == 0) {
            const std::string rest = Trim(t.substr(4));
            if (!rest.empty() && rest[0] == '=') { key = static_cast<int>(i); break; }
        }
    }
    if (key >= 0) {
        const std::string cur = Trim(lines[key].substr(lines[key].find('=') + 1));
        if (!cur.empty() && FileSize(std::string(PS2_BIOS_DIR) + "/" + cur) == PS2_BIOS_SIZE) {
            s_ps2_bios = cur;
            return;
        }
        lines[key] = "BIOS = " + bios;
    } else if (section >= 0) {
        lines.insert(lines.begin() + section + 1, "BIOS = " + bios);
    } else {
        lines.insert(lines.begin(), {"[Filenames]", "BIOS = " + bios, ""});
    }
    WriteLines(PS2_INI, lines);
}

void SetupPs2() {
    GatherBios();
    std::vector<std::string> found;
    if (DIR* d = opendir(PS2_BIOS_DIR)) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n[0] != '.' && FileSize(std::string(PS2_BIOS_DIR) + "/" + n) == PS2_BIOS_SIZE)
                found.push_back(n);
        }
        closedir(d);
    }
    if (found.empty()) return;
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
        const int ra = BiosRank(a), rb = BiosRank(b);
        return ra != rb ? ra < rb : a < b;
    });
    s_ps2_bios = found[0];
    ConfigurePs2(found[0]);
}

// Adds `key = "value"` to a RetroArch options file unless the key is already set.
void EnsureOption(const std::string& file, const char* key, const char* value) {
    std::vector<std::string> lines = ReadLines(file.c_str());
    for (const std::string& l : lines)
        if (Trim(l).rfind(key, 0) == 0) return;
    lines.push_back(std::string(key) + " = \"" + value + "\"");
    const size_t slash = file.find_last_of('/');
    MakeDirs(file.substr(0, slash));
    WriteLines(file.c_str(), lines);
}

// Dreamcast runs on Flycast's built-in BIOS, so no dc_boot.bin is needed.
void SetupRetroArchOptions() {
    EnsureOption("sdmc:/retroarch/config/Flycast/Flycast.opt", "flycast_hle_bios", "enabled");
    EnsureOption("sdmc:/retroarch/retroarch-core-options.cfg", "flycast_hle_bios", "enabled");
}

void SetupThread() {
    MakeDirs(EMUS);
    for (const System& s : Systems()) MakeDirs(std::string(ROMS) + "/" + s.id);
    MakeDirs(std::string(ROMS) + "/3ds");
    SetupPs2();
    SetupRetroArchOptions();
    s_setup_done = true;
}

// Emulators unpack the first time their system is played, not all up front: with
// every system bundled that would be over half a gigabyte of SD card.
bool EnsureEmulator(int system) {
    const std::string src = std::string("romfs:/emus/") + Systems()[system].emu + ".nro";
    const std::string dst = EmulatorPath(system);
    const std::int64_t want = FileSize(src);
    if (want <= 0) return FileSize(dst) > 0;
    if (FileSize(dst) == want) return true;
    LOG_INFO(Frontend, "Unpacking the {} emulator", Systems()[system].name);
    return CopyFile(src, dst);
}

void WaitSetup() {
    std::lock_guard lock{s_setup_mutex};
    if (s_setup.joinable()) s_setup.join();
}

}  // namespace

const std::vector<System>& Systems() {
    // Cartridge systems also take .zip/.7z inside their own folder (RetroArch opens them).
    static const std::vector<System> systems = {
        {"ds", "Nintendo DS", "DS", "ds", 0x3D, 0x7B, 0xFF, {"nds", "dsi", "zip", "7z"}, {"nds", "dsi"}, true},
        {"gba", "Game Boy Advance", "GBA", "gba", 0x8B, 0x5C, 0xF6, {"gba", "zip", "7z"}, {"gba"}, true},
        {"gb", "Game Boy", "GB", "gb", 0x22, 0xC5, 0x5E, {"gb", "gbc", "sgb", "zip", "7z"}, {"gb", "gbc", "sgb"}, true},
        {"nes", "NES", "NES", "nes", 0xE1, 0x3B, 0x3B, {"nes", "fds", "unf", "unif", "zip", "7z"}, {"nes", "fds", "unf", "unif"}, true},
        {"snes", "Super Nintendo", "SNES", "snes", 0x7E, 0x6C, 0xD8, {"sfc", "smc", "fig", "swc", "bs", "zip", "7z"}, {"sfc", "smc", "fig", "swc", "bs"}, true},
        {"n64", "Nintendo 64", "N64", "n64", 0x10, 0x9A, 0x4E, {"n64", "z64", "v64", "zip", "7z"}, {"n64", "z64", "v64"}, true},
        {"vb", "Virtual Boy", "VB", "vb", 0xD1, 0x1F, 0x3A, {"vb", "vboy", "zip"}, {"vb", "vboy"}, true},
        {"ps1", "PlayStation", "PS1", "ps1", 0x9C, 0xA3, 0xB5, {"cue", "chd", "pbp", "m3u", "ccd", "iso", "ecm"}, {"pbp", "cue", "chd", "m3u", "ccd", "ecm"}, true},
        {"ps2", "PlayStation 2", "PS2", "ps2", 0x38, 0xBD, 0xF8, {"iso", "chd", "cso", "zso", "cue"}, {"iso", "cso", "zso"}, true},
        {"psp", "PSP", "PSP", "psp", 0x4B, 0x55, 0x63, {"iso", "cso", "pbp", "chd"}, {}, true},
        {"md", "Mega Drive / Genesis", "MD", "genesis", 0x1F, 0x1F, 0x2E, {"md", "gen", "smd", "68k", "sgd", "bin", "zip", "7z"}, {"md", "gen", "smd", "68k", "sgd"}, true},
        {"sms", "Master System", "SMS", "genesis", 0x2B, 0x55, 0xC7, {"sms", "sg", "zip", "7z"}, {"sms", "sg"}, true},
        {"gg", "Game Gear", "GG", "genesis", 0x33, 0x33, 0x40, {"gg", "zip", "7z"}, {"gg"}, true},
        {"dc", "Dreamcast", "DC", "dc", 0xF0, 0x7A, 0x1E, {"cdi", "gdi", "chd", "cue"}, {"cdi", "gdi"}, true},
        {"arcade", "Arcade", "Arcade", "arcade", 0xF5, 0x9E, 0x0B, {"zip", "7z"}, {}, true},
        {"pce", "PC Engine", "PCE", "pce", 0xF4, 0x72, 0xB6, {"pce", "sgx", "cue", "ccd", "chd", "zip"}, {"pce", "sgx"}, true},
        {"ngp", "Neo Geo Pocket", "NGP", "ngp", 0x0E, 0xA5, 0xE9, {"ngp", "ngc", "zip"}, {"ngp", "ngc"}, true},
        {"ws", "WonderSwan", "WS", "ws", 0x64, 0x74, 0x8B, {"ws", "wsc", "zip"}, {"ws", "wsc"}, true},
        {"a2600", "Atari 2600", "2600", "a2600", 0xB4, 0x53, 0x09, {"a26", "bin", "zip"}, {"a26"}, true},
        {"lynx", "Atari Lynx", "Lynx", "lynx", 0xCA, 0x8A, 0x04, {"lnx", "zip"}, {"lnx"}, true},
        {"wiiu", "Wii U", "Wii U", "wiiu", 0x2D, 0xD4, 0xBF, {"wua", "wud", "wux", "rpx"}, {"wua", "wud", "wux", "rpx"}, false},
    };
    return systems;
}

int SystemForPath(const std::string& path) {
    const std::string ext = ExtOf(path);
    const std::string low = Lower(path);
    const auto& sys = Systems();
    auto has = [&ext](const std::vector<std::string>& list) {
        return std::find(list.begin(), list.end(), ext) != list.end();
    };
    // Inside sdmc:/roms/<id>/, that folder decides.
    for (size_t i = 0; i < sys.size(); i++)
        if (low.find(std::string("/roms/") + sys[i].id + "/") != std::string::npos)
            return has(sys[i].exts) ? static_cast<int>(i) : -1;
    if (low.find("/switch/armsx2/games/") != std::string::npos) {
        for (size_t i = 0; i < sys.size(); i++)
            if (std::string(sys[i].id) == "ps2") return has(sys[i].exts) ? static_cast<int>(i) : -1;
    }
    // Anywhere else only unambiguous extensions count.
    for (size_t i = 0; i < sys.size(); i++)
        if (has(sys[i].loose)) return static_cast<int>(i);
    return -1;
}

void StartSetup() {
    std::lock_guard lock{s_setup_mutex};
    if (!s_setup.joinable() && !s_setup_done) s_setup = std::thread(SetupThread);
}

void FinishSetup() {
    WaitSetup();
}

void AddGames(std::vector<GameEntry>& games) {
    auto seen = [&games](const std::string& path) {
        return std::any_of(games.begin(), games.end(), [&](const GameEntry& g) { return g.path == path; });
    };
    auto add = [&](const std::string& path, const std::string& name, int only) {
        const std::string low = Lower(path);
        if (low.find("/bios") != std::string::npos) return;
        const int s = SystemForPath(path);
        if (s < 0 || (only >= 0 && s != only) || seen(path)) return;
        GameEntry e;
        e.path = path;
        e.title = PrettyTitle(name);
        e.file_type = Systems()[s].badge;
        e.system = s;
        games.push_back(std::move(e));
    };
    // Each system's own folder, then anything else under sdmc:/roms by extension.
    const auto& sys = Systems();
    for (size_t s = 0; s < sys.size(); s++)
        Walk(std::string(ROMS) + "/" + sys[s].id, 6,
             [&](const std::string& p, const std::string& n) { add(p, n, static_cast<int>(s)); });
    if (DIR* d = opendir(ROMS)) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name[0] == '.') continue;
            const std::string p = std::string(ROMS) + "/" + name;
            bool own = Lower(name) == "3ds";
            for (const auto& s : sys) own = own || Lower(name) == s.id;
            if (own) continue;
            if (e->d_type == DT_DIR) Walk(p, 5, [&](const std::string& fp, const std::string& fn) { add(fp, fn, -1); });
            else add(p, name, -1);
        }
        closedir(d);
    }
    Walk(std::string(PS2_ROOT) + "/games", 4, [&](const std::string& p, const std::string& n) { add(p, n, -1); });
}

bool Launch(const std::string& path, std::string& error) {
    const int s = SystemForPath(path);
    if (s < 0) {
        error = "Not a game EmuSwitch hands to another emulator";
        return false;
    }
    WaitSetup();
    const System& sys = Systems()[s];
    const std::string nro = EmulatorPath(s);
    if (!EnsureEmulator(s) || FileSize(nro) <= 0) {
        error = std::string("Couldn't unpack the ") + sys.name + " emulator. Is the SD card full?";
        return false;
    }
    if (std::string(sys.id) == "ps2" && s_ps2_bios.empty()) {
        error = "PS2 needs a BIOS: copy your BIOS .zip or .bin into sdmc:/roms/ps2/ and reopen EmuSwitch";
        return false;
    }
    // hbloader opens the .nro by its SD path, without the sdmc: prefix.
    const std::string sd = nro.substr(5);
    std::string argv = "\"" + sd + "\"";
    if (sys.takes_game) argv += " \"" + path + "\"";
    if (!HandOff(sd, argv)) {
        error = "Couldn't hand over to the emulator";
        return false;
    }
    LOG_INFO(Frontend, "Handing {} to {}", path, nro);
    return true;
}

}  // namespace SwitchFrontend::Multi
