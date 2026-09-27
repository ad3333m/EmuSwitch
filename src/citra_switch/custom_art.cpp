// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/custom_art.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/stat.h>

#include "citra_switch/simplewebp.h"

#include "citra_switch/camera/image_decode.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/menu_skin.h"
#include "citra_switch/multi_system.h"

namespace SwitchFrontend::Art {
namespace {

constexpr const char* kRoot = "sdmc:/switch/emuswitch";
constexpr const char* kConfig = "sdmc:/switch/emuswitch/art.ini";
constexpr int kMaxSide = 256; // pictures are shown at most ~140px; this keeps memory small
constexpr const char* kExts[] = {"png", "webp", "jpg", "jpeg"};

// "system" -> id -> path and "game" -> rom path -> path.
std::map<std::string, std::string> s_system_paths;
std::map<std::string, std::string> s_game_paths;
bool s_loaded_config = false;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Stem(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Config lines: "system<TAB>id<TAB>path" and "game<TAB>rom path<TAB>path".
void ReadConfig() {
    if (s_loaded_config) return;
    s_loaded_config = true;
    FILE* f = fopen(kConfig, "rb");
    if (!f) return;
    char buf[2048];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const size_t a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || b == a) continue;
        const std::string kind = line.substr(0, a), key = line.substr(a + 1, b - a - 1), val = line.substr(b + 1);
        if (kind == "system") s_system_paths[key] = val;
        else if (kind == "game") s_game_paths[key] = val;
    }
    fclose(f);
}

void WriteConfig() {
    mkdir(kRoot, 0777);
    FILE* f = fopen(kConfig, "wb");
    if (!f) return;
    for (const auto& [k, v] : s_system_paths) fprintf(f, "system\t%s\t%s\n", k.c_str(), v.c_str());
    for (const auto& [k, v] : s_game_paths) fprintf(f, "game\t%s\t%s\n", k.c_str(), v.c_str());
    fclose(f);
}

std::string DropIn(const std::string& dir, const std::string& stem) {
    for (const char* ext : kExts) {
        const std::string p = std::string(kRoot) + "/" + dir + "/" + stem + "." + ext;
        if (Exists(p)) return p;
    }
    return "";
}

// Box-filters RGBA down so the longer side is at most kMaxSide.
void Shrink(std::vector<std::uint8_t>& rgba, int& w, int& h) {
    const int factor = (std::max(w, h) + kMaxSide - 1) / kMaxSide;
    if (factor <= 1) return;
    const int nw = std::max(1, w / factor), nh = std::max(1, h / factor);
    std::vector<std::uint8_t> out(std::size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y * factor; yy < std::min(h, (y + 1) * factor); ++yy)
                for (int xx = x * factor; xx < std::min(w, (x + 1) * factor); ++xx) {
                    const std::uint8_t* p = &rgba[(std::size_t(yy) * w + xx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c) out[(std::size_t(y) * nw + x) * 4 + c] = std::uint8_t(sum[c] / std::max(1u, n));
        }
    rgba.swap(out);
    w = nw;
    h = nh;
}

std::string Decode(const std::string& path, std::vector<Gfx::u32>& px, int& w, int& h) {
    std::vector<std::uint8_t> rgba;
    if (Lower(path).size() > 5 && Lower(path).rfind(".webp") == path.size() - 5) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return "the file could not be read";
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<std::uint8_t> file(size > 0 ? std::size_t(size) : 0);
        const bool ok = size > 0 && fread(file.data(), 1, file.size(), f) == file.size();
        fclose(f);
        if (!ok) return "the file could not be read";
        simplewebp* webp = nullptr;
        if (simplewebp_load_from_memory(file.data(), file.size(), nullptr, &webp) != SIMPLEWEBP_NO_ERROR)
            return "not a WebP image this can read";
        size_t ww = 0, hh = 0;
        simplewebp_get_dimensions(webp, &ww, &hh);
        if (ww == 0 || hh == 0 || ww > 8192 || hh > 8192) {
            simplewebp_unload(webp);
            return "the image is empty or too large";
        }
        rgba.resize(ww * hh * 4);
        const simplewebp_error err = simplewebp_decode(webp, rgba.data(), nullptr);
        simplewebp_unload(webp);
        if (err != SIMPLEWEBP_NO_ERROR) return "the WebP could not be decoded";
        w = int(ww);
        h = int(hh);
    } else {
        DecodedImage img;
        const std::string err = DecodeImageFile(path, kMaxSide, kMaxSide, img);
        if (!err.empty()) return err;
        rgba = std::move(img.rgba);
        w = img.width;
        h = img.height;
    }
    Shrink(rgba, w, h);
    px.resize(std::size_t(w) * h);
    std::memcpy(px.data(), rgba.data(), px.size() * 4);
    return "";
}

std::string ApplySystem(const std::string& id) {
    const std::string path = SystemArtPath(id);
    if (path.empty()) {
        Skin::SetSystemImage(id, {}, 0, 0);
        return "";
    }
    std::vector<Gfx::u32> px;
    int w = 0, h = 0;
    const std::string err = Decode(path, px, w, h);
    Skin::SetSystemImage(id, err.empty() ? std::move(px) : std::vector<Gfx::u32>{}, w, h);
    return err;
}

std::string ApplyGame(const GameEntry& game) {
    const std::string path = GameArtPath(game);
    if (path.empty()) {
        Skin::SetGameImage(game.path, {}, 0, 0);
        return "";
    }
    std::vector<Gfx::u32> px;
    int w = 0, h = 0;
    const std::string err = Decode(path, px, w, h);
    Skin::SetGameImage(game.path, err.empty() ? std::move(px) : std::vector<Gfx::u32>{}, w, h);
    return err;
}

} // namespace

const std::vector<SystemInfo>& Systems() {
    static const std::vector<SystemInfo> list = [] {
        std::vector<SystemInfo> v{{"3ds", "Nintendo 3DS"}};
        for (const auto& s : Multi::Systems()) v.push_back({s.id, s.name});
        return v;
    }();
    return list;
}

bool IsImageFile(const std::string& name) {
    const std::string low = Lower(name);
    for (const char* ext : kExts) {
        const std::string suffix = std::string(".") + ext;
        if (low.size() > suffix.size() && low.compare(low.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    return false;
}

std::string SystemArtPath(const std::string& id) {
    ReadConfig();
    const auto it = s_system_paths.find(id);
    if (it != s_system_paths.end() && Exists(it->second)) return it->second;
    return DropIn("systems", id);
}

std::string GameArtPath(const GameEntry& game) {
    ReadConfig();
    const auto it = s_game_paths.find(game.path);
    if (it != s_game_paths.end() && Exists(it->second)) return it->second;
    std::string p = DropIn("covers", Stem(game.path));
    if (p.empty() && !game.title.empty()) p = DropIn("covers", game.title);
    return p;
}

void LoadSystemArt() {
    for (const SystemInfo& s : Systems()) ApplySystem(s.id);
}

void LoadGameArt(const std::vector<GameEntry>& games) {
    for (const GameEntry& g : games) ApplyGame(g);
}

std::string SetSystemArt(const std::string& id, const std::string& image_path) {
    ReadConfig();
    if (image_path.empty()) s_system_paths.erase(id);
    else s_system_paths[id] = image_path;
    const std::string err = ApplySystem(id);
    if (!err.empty() && !image_path.empty()) {
        s_system_paths.erase(id);
        ApplySystem(id);
        return err;
    }
    WriteConfig();
    return "";
}

std::string SetGameArt(const GameEntry& game, const std::string& image_path) {
    ReadConfig();
    if (image_path.empty()) s_game_paths.erase(game.path);
    else s_game_paths[game.path] = image_path;
    const std::string err = ApplyGame(game);
    if (!err.empty() && !image_path.empty()) {
        s_game_paths.erase(game.path);
        ApplyGame(game);
        return err;
    }
    WriteConfig();
    return "";
}

} // namespace SwitchFrontend::Art
