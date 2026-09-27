// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/custom_art.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

#include "citra_switch/simplewebp.h"

#include "citra_switch/camera/image_decode.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/menu_skin.h"
#include "citra_switch/multi_system.h"
#include "common/horizon_thread.h"

namespace SwitchFrontend::Art {
namespace {

constexpr const char* kRoot = "sdmc:/switch/emuswitch";
constexpr const char* kConfig = "sdmc:/switch/emuswitch/art.ini";
constexpr const char* kExts[] = {"png", "webp", "jpg", "jpeg"};

// How big pictures are kept: the short side covers the largest size they're drawn at (a
// focused tile, a system card) with some room for a sharp resize, the long side is capped.
struct Fit {
    int cover;
    int longest;
};
constexpr Fit kGameFit{176, 300};
constexpr Fit kSystemFit{232, 360};
constexpr int kPreviewSide = 360;
// Logos are wide and drawn about 40 pixels high: the whole logo within a 560 square is plenty.
constexpr int kLogoSide = 560;

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

// Identifies a file's current contents cheaply: its path, size and modification time.
std::string Stamp(const std::string& p) {
    struct stat st;
    if (p.empty() || stat(p.c_str(), &st) != 0) {
        return p;
    }
    return p + "|" + std::to_string(static_cast<long long>(st.st_size)) + "|" +
           std::to_string(static_cast<long long>(st.st_mtime));
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

// A picture folder (covers/, systems/), listed once: lower-case stem -> file. Saves a handful
// of file checks per game or system.
std::map<std::string, std::string> ListPictures(const char* sub) {
    std::map<std::string, std::string> out;
    const std::string dir = std::string(kRoot) + "/" + sub;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::map<std::string, int> rank;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        const size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string ext = Lower(name.substr(dot + 1));
        int r = -1;
        for (int i = 0; i < 4; ++i) {
            if (ext == kExts[i]) r = i;
        }
        if (r < 0) continue;
        const std::string stem = Lower(name.substr(0, dot));
        auto it = rank.find(stem);
        if (it == rank.end() || r < it->second) {
            rank[stem] = r;
            out[stem] = dir + "/" + name;
        }
    }
    closedir(d);
    return out;
}

// Titles installed on the emulated SD card are told apart by their title ID: their files all
// have the same few names.
std::string CoverStem(const GameEntry& game) {
    if (game.installed && game.program_id != 0) {
        char id[20];
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(game.program_id));
        return id;
    }
    return Stem(game.path);
}

std::string GamePathWith(const GameEntry& game, const std::map<std::string, std::string>* covers) {
    ReadConfig();
    const auto it = s_game_paths.find(game.path);
    if (it != s_game_paths.end() && Exists(it->second)) return it->second;
    if (covers) {
        if (auto c = covers->find(Lower(CoverStem(game))); c != covers->end()) return c->second;
        if (!game.title.empty()) {
            if (auto c = covers->find(Lower(game.title)); c != covers->end()) return c->second;
        }
        return "";
    }
    std::string p = DropIn("covers", CoverStem(game));
    if (p.empty() && !game.title.empty()) p = DropIn("covers", game.title);
    return p;
}

// ---- decoding ----

enum class Format { Png, Jpeg, WebP, Unknown };

// Reads the format from the file's first bytes: pictures saved from the web are often WebP
// whatever their name says.
Format Sniff(const std::string& path) {
    unsigned char head[16] = {};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return Format::Unknown;
    const size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (n >= 8 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return Format::Png;
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return Format::Jpeg;
    if (n >= 12 && std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WEBP", 4) == 0) return Format::WebP;
    return Format::Unknown;
}

// Box-averages RGBA down by a whole factor: a cheap first step for very large pictures
// before the careful resize.
void BoxShrink(std::vector<std::uint8_t>& rgba, int& w, int& h, int factor) {
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

// Decodes `path` and scales it so its short side covers `fit.cover` (never enlarging) and its
// long side stays within `fit.longest`. `contain` instead fits the whole picture in a square
// of `fit.longest`.
std::string Decode(const std::string& path, Fit fit, bool contain, Gfx::Image& out) {
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    switch (Sniff(path)) {
    case Format::Png:
    case Format::Jpeg: {
        DecodedImage img;
        const int cover = contain ? fit.longest : fit.cover;
        const std::string err = DecodeImageFile(path, cover, cover, img);
        if (!err.empty()) return err;
        rgba = std::move(img.rgba);
        w = img.width;
        h = img.height;
        break;
    }
    case Format::WebP: {
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
        break;
    }
    case Format::Unknown:
        return Exists(path) ? "it isn't a PNG, JPEG or WebP picture" : "the file could not be read";
    }
    if (w <= 0 || h <= 0 || rgba.size() < std::size_t(w) * h * 4) return "the picture has no pixels";

    // The target size.
    float k = 1.0f;
    if (contain) {
        k = std::min(1.0f, float(fit.longest) / float(std::max(w, h)));
    } else {
        k = std::min(1.0f, float(fit.cover) / float(std::min(w, h)));
        k = std::min(k, float(fit.longest) / float(std::max(w, h)));
    }
    const int tw = std::max(1, int(w * k + 0.5f)), th = std::max(1, int(h * k + 0.5f));
    // Whole-factor box steps down to about twice the target, then Lanczos for the rest.
    BoxShrink(rgba, w, h, std::max(1, std::min(w / std::max(1, tw * 2), h / std::max(1, th * 2))));

    Gfx::Image img;
    img.w = w;
    img.h = h;
    img.px.resize(std::size_t(w) * h);
    std::memcpy(img.px.data(), rgba.data(), img.px.size() * 4);
    img.opaque = std::all_of(img.px.begin(), img.px.end(), [](Gfx::u32 p) { return (p >> 24) == 0xFF; });
    out = (w == tw && h == th) ? std::move(img) : Gfx::Resize(img, tw, th);
    return "";
}

std::string ApplySystem(const std::string& id, const std::map<std::string, std::string>* listed = nullptr) {
    std::string path;
    if (listed) {
        ReadConfig();
        const auto it = s_system_paths.find(id);
        if (it != s_system_paths.end() && Exists(it->second)) {
            path = it->second;
        } else if (const auto l = listed->find(Lower(id)); l != listed->end()) {
            path = l->second;
        }
    } else {
        path = SystemArtPath(id);
    }
    if (path.empty()) {
        Skin::SetSystemImage(id, Gfx::Image{});
        return "";
    }
    Gfx::Image img;
    const std::string err = Decode(path, kSystemFit, false, img);
    Skin::SetSystemImage(id, err.empty() ? std::move(img) : Gfx::Image{});
    return err;
}

// ---- the background loader ----

enum JobKind { kGameJob, kSystemJob, kLogoJob };
struct Job {
    bool preview = false;
    std::string key;   // rom path (games), system id (systems, logos) or file (previews)
    std::string path;  // the picture
    std::string stamp;
    JobKind kind = kGameJob;
};

// Settles `key` on `stamp` (a picture applied by hand), dropping requests still on their way.
// Takes g_mutex.
void Settle(const std::string& key, const std::string& stamp);

// What g_applied / g_pending know a job by: games by rom path, the rest by kind and id.
std::string AppliedKey(JobKind kind, const std::string& key) {
    switch (kind) {
    case kSystemJob:
        return "system:" + key;
    case kLogoJob:
        return "logo:" + key;
    default:
        return key;
    }
}
struct Done {
    Job job;
    Gfx::Image img;
    std::string error;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_queue;
std::vector<Done> g_done;
std::thread g_thread;
bool g_stop = false;
// Game pictures in the skin (rom path -> stamp) and the ones on their way.
std::map<std::string, std::string> g_applied;
std::set<std::string> g_pending;
// The picker's latest preview.
std::string g_preview_want;
bool g_preview_ready = false;
Done g_preview;

// Makes a console logo read on the dark menu: a white background becomes see-through, then
// black and dark grey lettering turns light. Coloured parts, and anything already light, stay as
// they are, so a logo that was made for a dark background comes through unchanged.
void ForDarkMenu(Gfx::Image& img) {
    if (img.Empty()) return;
    // Mostly solid pixels means the logo sits on a background, taken to be white.
    std::size_t solid = 0;
    for (const Gfx::u32 p : img.px) solid += (p >> 24) >= 250 ? 1 : 0;
    const bool on_white = solid > img.px.size() * 9 / 10;
    for (Gfx::u32& p : img.px) {
        int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF, a = p >> 24;
        if (on_white) {
            // Colour to alpha against white: the least white channel sets the opacity.
            const int k = 255 - std::min({r, g, b});
            if (k == 0) {
                p = 0;
                continue;
            }
            r = std::clamp(255 - (255 - r) * 255 / k, 0, 255);
            g = std::clamp(255 - (255 - g) * 255 / k, 0, 255);
            b = std::clamp(255 - (255 - b) * 255 / k, 0, 255);
            a = a * k / 255;
        }
        if (a == 0) {
            p = 0;
            continue;
        }
        // Greys up to mid-light (the Wii U's "Wii", say) end up light too; anything lighter was
        // already made for a dark background.
        const int lum = (r * 299 + g * 587 + b * 114) / 1000;
        if (std::max({r, g, b}) - std::min({r, g, b}) < 60 && lum < 180) {
            r = g = b = 255 - lum * 32 / 100;
        } else if (lum < 60) {
            // Deep colours come up a little so they read on dark grey.
            r += (255 - r) * 22 / 100;
            g += (255 - g) * 22 / 100;
            b += (255 - b) * 22 / 100;
        }
        p = Gfx::u32(r) | (Gfx::u32(g) << 8) | (Gfx::u32(b) << 16) | (Gfx::u32(a) << 24);
    }
    img.opaque = false;
}

void Settle(const std::string& key, const std::string& stamp) {
    std::lock_guard lock{g_mutex};
    const std::string prefix = key + "\n";
    for (auto it = g_pending.lower_bound(prefix); it != g_pending.end() && it->rfind(prefix, 0) == 0;) {
        it = g_pending.erase(it);
    }
    g_applied[key] = stamp;
}

void Loader() {
    // Core 2 is idle between frames; the menu's own thread keeps core 0.
    Common::Horizon::PinCurrentThread(2);
    while (true) {
        Job job;
        {
            std::unique_lock lock{g_mutex};
            g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            job = std::move(g_queue.front());
            g_queue.pop_front();
            if (job.preview && job.key != g_preview_want) continue; // the picker moved on
        }
        Done done;
        if (job.preview) {
            done.error = Decode(job.path, Fit{kPreviewSide, kPreviewSide}, true, done.img);
        } else if (job.kind == kSystemJob) {
            done.error = Decode(job.path, kSystemFit, false, done.img);
        } else if (job.kind == kLogoJob) {
            done.error = Decode(job.path, Fit{kLogoSide, kLogoSide}, true, done.img);
            // The logos EmuSwitch ships are made for the dark menu already.
            if (done.error.empty() && job.path.rfind("romfs:/", 0) != 0) {
                ForDarkMenu(done.img);
            }
        } else {
            done.error = Decode(job.path, kGameFit, false, done.img);
        }
        done.job = std::move(job);
        std::lock_guard lock{g_mutex};
        if (done.job.preview) {
            if (done.job.key == g_preview_want) {
                g_preview = std::move(done);
                g_preview_ready = true;
            }
        } else {
            g_done.push_back(std::move(done));
        }
    }
}

void EnsureLoader() {
    if (!g_thread.joinable()) {
        g_stop = false;
        g_thread = std::thread(Loader);
    }
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
    return GamePathWith(game, nullptr);
}

bool HasPickedArt(const GameEntry& game) {
    ReadConfig();
    const auto it = s_game_paths.find(game.path);
    return it != s_game_paths.end() && Exists(it->second);
}

std::string LoadProfile() {
    std::string name = "Player";
#ifdef __SWITCH__
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        return name;
    }
    AccountUid uid{};
    if (R_FAILED(accountGetPreselectedUser(&uid)) || !accountUidIsValid(&uid)) {
        if (R_FAILED(accountGetLastOpenedUser(&uid))) {
            uid = AccountUid{};
        }
    }
    AccountProfile profile;
    if (accountUidIsValid(&uid) && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountUserData user{};
        AccountProfileBase base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, &user, &base)) && base.nickname[0] != 0) {
            name = base.nickname;
        }
        u32 size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0 && size < (1u << 20)) {
            std::vector<std::uint8_t> jpg(size);
            u32 real = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpg.data(), size, &real)) && real > 0) {
                // The decoder reads files, so the avatar takes a short trip through the SD card.
                mkdir(kRoot, 0777);
                const std::string path = std::string(kRoot) + "/avatar.jpg";
                if (FILE* f = fopen(path.c_str(), "wb")) {
                    const bool ok = fwrite(jpg.data(), 1, real, f) == real;
                    fclose(f);
                    DecodedImage img;
                    if (ok && DecodeImageFile(path, 128, 128, img).empty() && img.width > 0) {
                        std::vector<Gfx::u32> px(std::size_t(img.width) * img.height);
                        std::memcpy(px.data(), img.rgba.data(), px.size() * 4);
                        Skin::SetAvatar(std::move(px), img.width, img.height);
                    }
                }
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
#endif
    return name;
}

void LoadSystemArt() {
    // Decoded in the background like the games' pictures, so start-up doesn't wait on them;
    // ones that haven't changed since the last call aren't read again.
    const auto listed = ListPictures("systems");
    const auto logos = ListPictures("logos");
    ReadConfig();
    std::vector<Job> jobs;
    const auto want = [&jobs](JobKind kind, const std::string& id, const std::string& path) {
        const std::string stamp = Stamp(path);
        const std::string key = AppliedKey(kind, id);
        std::lock_guard lock{g_mutex};
        const auto it = g_applied.find(key);
        if ((it != g_applied.end() && it->second == stamp) || g_pending.count(key + "\n" + stamp)) {
            return;
        }
        if (path.empty()) {
            if (kind == kSystemJob) {
                Skin::SetSystemImage(id, Gfx::Image{});
            } else {
                Skin::SetSystemLogo(id, Gfx::Image{});
            }
            g_applied[key] = stamp;
            return;
        }
        g_pending.insert(key + "\n" + stamp);
        jobs.push_back({false, id, path, stamp, kind});
    };
    for (const SystemInfo& sys : Systems()) {
        std::string path;
        if (const auto it = s_system_paths.find(sys.id); it != s_system_paths.end() && Exists(it->second)) {
            path = it->second;
        } else if (const auto l = listed.find(Lower(sys.id)); l != listed.end()) {
            path = l->second;
        }
        want(kSystemJob, sys.id, path);
        // A logo put in the logos folder wins over the one that comes with EmuSwitch.
        const auto logo = logos.find(Lower(sys.id));
        std::string logo_path = logo != logos.end() ? logo->second : std::string{};
        if (logo_path.empty()) {
            const std::string built_in = "romfs:/logos/" + sys.id + ".png";
            if (Exists(built_in)) logo_path = built_in;
        }
        want(kLogoJob, sys.id, logo_path);
    }
    if (jobs.empty()) return;
    std::lock_guard lock{g_mutex};
    // Ahead of any game pictures: there are only a few and they're on every page.
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) g_queue.push_front(std::move(*it));
    EnsureLoader();
    g_wake.notify_all();
}

void LoadGameArt(const std::vector<GameEntry>& games) {
    const auto covers = ListPictures("covers");
    std::vector<Job> jobs;
    for (const GameEntry& g : games) {
        const std::string path = GamePathWith(g, &covers);
        const std::string stamp = Stamp(path);
        std::lock_guard lock{g_mutex};
        const auto it = g_applied.find(g.path);
        if ((it != g_applied.end() && it->second == stamp) || g_pending.count(g.path + "\n" + stamp)) {
            continue;
        }
        if (path.empty()) {
            // Nothing (any more): back to the icon or initials.
            Skin::SetGameImage(g.path, Gfx::Image{});
            g_applied[g.path] = stamp;
            continue;
        }
        g_pending.insert(g.path + "\n" + stamp);
        jobs.push_back({false, g.path, path, stamp});
    }
    if (jobs.empty()) return;
    std::lock_guard lock{g_mutex};
    for (Job& j : jobs) g_queue.push_back(std::move(j));
    EnsureLoader();
    g_wake.notify_all();
}

bool Pump() {
    std::vector<Done> done;
    {
        std::lock_guard lock{g_mutex};
        done.swap(g_done);
    }
    for (Done& d : done) {
        const std::string key = AppliedKey(d.job.kind, d.job.key);
        {
            // A picture set by hand in the meantime wins over this older request.
            std::lock_guard lock{g_mutex};
            if (g_pending.erase(key + "\n" + d.job.stamp) == 0) continue;
            g_applied[key] = d.job.stamp;
        }
        Gfx::Image img = d.error.empty() ? std::move(d.img) : Gfx::Image{};
        switch (d.job.kind) {
        case kSystemJob:
            Skin::SetSystemImage(d.job.key, std::move(img));
            break;
        case kLogoJob:
            Skin::SetSystemLogo(d.job.key, std::move(img));
            break;
        default:
            Skin::SetGameImage(d.job.key, std::move(img));
            break;
        }
    }
    return !done.empty();
}

void StopLoading() {
    {
        std::lock_guard lock{g_mutex};
        if (!g_thread.joinable()) return;
        g_stop = true;
        g_queue.clear();
    }
    g_wake.notify_all();
    g_thread.join();
    std::lock_guard lock{g_mutex};
    // Finished work is still good; unfinished work is asked for again by the next rescan.
    g_pending.clear();
    for (const Done& d : g_done) g_pending.insert(AppliedKey(d.job.kind, d.job.key) + "\n" + d.job.stamp);
    g_preview_want.clear();
    g_preview_ready = false;
}

std::string SetSystemArt(const std::string& id, const std::string& image_path) {
    ReadConfig();
    if (image_path.empty()) s_system_paths.erase(id);
    else s_system_paths[id] = image_path;
    std::string err = ApplySystem(id);
    if (!err.empty() && !image_path.empty()) {
        s_system_paths.erase(id);
        ApplySystem(id);
    } else {
        err.clear();
        WriteConfig();
    }
    Settle(AppliedKey(kSystemJob, id), Stamp(SystemArtPath(id)));
    return err;
}

std::string SetGameArt(const GameEntry& game, const std::string& image_path) {
    ReadConfig();
    const std::string before = s_game_paths.count(game.path) ? s_game_paths[game.path] : std::string{};
    if (image_path.empty()) s_game_paths.erase(game.path);
    else s_game_paths[game.path] = image_path;
    const std::string path = GameArtPath(game);
    Gfx::Image img;
    std::string err;
    if (!path.empty()) {
        err = Decode(path, kGameFit, false, img);
    }
    if (!err.empty() && !image_path.empty()) {
        // Keep whatever was there before.
        if (before.empty()) s_game_paths.erase(game.path);
        else s_game_paths[game.path] = before;
        return err;
    }
    Skin::SetGameImage(game.path, std::move(img));
    Settle(game.path, Stamp(path));
    WriteConfig();
    return "";
}

std::string DecodePicture(const std::string& path, int longest, Gfx::Image& out) {
    return Decode(path, Fit{longest, longest}, true, out);
}

std::string PictureStem(const GameEntry& game) {
    return CoverStem(game);
}

void RequestPreview(const std::string& path) {
    std::lock_guard lock{g_mutex};
    if (g_preview_want == path) return;
    g_preview_want = path;
    g_preview_ready = false;
    // Ahead of any game pictures still loading: the picker is what the user is looking at.
    g_queue.push_front({true, path, path, ""});
    EnsureLoader();
    g_wake.notify_all();
}

bool TakePreview(const std::string& path, Gfx::Image& img, std::string& error) {
    std::lock_guard lock{g_mutex};
    if (!g_preview_ready || g_preview.job.key != path) return false;
    img = g_preview.img;
    error = g_preview.error;
    return true;
}

} // namespace SwitchFrontend::Art
