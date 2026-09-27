// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/cover_fetch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>

#include <curl/curl.h>

#include "citra_switch/custom_art.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/multi_system.h"
#include "common/horizon_thread.h"

namespace SwitchFrontend::Covers {
namespace {

// HTTPS first; plain HTTP (what RetroArch itself uses) if the secure connection fails.
constexpr const char* kServers[] = {"https://thumbnails.libretro.com/", "http://thumbnails.libretro.com/"};
std::atomic<int> g_server{0};
constexpr const char* kRoot = "sdmc:/switch/emuswitch";
constexpr const char* kCoverDir = "sdmc:/switch/emuswitch/covers";
constexpr const char* kCacheDir = "sdmc:/switch/emuswitch/cache";
constexpr const char* kUserAgent = "EmuSwitch-Covers/" DEKOPON_VERSION;
// A library's file list is fetched again after this long, and games it had no cover for are
// looked up again then.
constexpr std::time_t kListMaxAge = 14 * 24 * 60 * 60;
constexpr const char* kListMagic = "EMUSWITCH-BOXARTS 1";

std::mutex g_mutex; // guards g_thread
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_updated{false};
std::atomic<int> g_finished{0};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Stem(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string Extension(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    return dot == std::string::npos ? std::string{} : Lower(path.substr(dot + 1));
}

// The console's folder on the thumbnail server.
const char* LibraryFor(const GameEntry& game) {
    if (game.system < 0) {
        return "Nintendo - Nintendo 3DS";
    }
    const auto& systems = Multi::Systems();
    if (game.system >= static_cast<int>(systems.size())) {
        return nullptr;
    }
    const std::string id = systems[static_cast<std::size_t>(game.system)].id;
    if (id == "ds") return "Nintendo - Nintendo DS";
    if (id == "gba") return "Nintendo - Game Boy Advance";
    if (id == "gb") return Extension(game.path) == "gbc" ? "Nintendo - Game Boy Color" : "Nintendo - Game Boy";
    if (id == "nes") return "Nintendo - Nintendo Entertainment System";
    if (id == "snes") return "Nintendo - Super Nintendo Entertainment System";
    if (id == "n64") return "Nintendo - Nintendo 64";
    if (id == "ps1") return "Sony - PlayStation";
    if (id == "ps2") return "Sony - PlayStation 2";
    if (id == "psp") return "Sony - PlayStation Portable";
    if (id == "wiiu") return "Nintendo - Wii U";
    return nullptr;
}

std::string UrlEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string{s.substr(i + 1, 2)}, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// A folder-safe name for a library ("Nintendo - Game Boy" -> "nintendo_-_game_boy").
std::string SafeName(std::string s) {
    for (char& c : s) {
        c = std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? static_cast<char>(std::tolower(c)) : '_';
    }
    return s;
}

// The plain letters of an accented Latin-1 character encoded as UTF-8 (C3 xx).
const char* Unaccent(unsigned char c) {
    if (c >= 0x80 && c <= 0x85) return "a";
    if (c == 0x87) return "c";
    if (c >= 0x88 && c <= 0x8B) return "e";
    if (c >= 0x8C && c <= 0x8F) return "i";
    if (c == 0x91) return "n";
    if ((c >= 0x92 && c <= 0x96) || c == 0x98) return "o";
    if (c >= 0x99 && c <= 0x9C) return "u";
    if (c == 0x9D) return "y";
    if (c == 0x9F) return "ss";
    if (c >= 0xA0 && c <= 0xA5) return "a";
    if (c == 0xA7) return "c";
    if (c >= 0xA8 && c <= 0xAB) return "e";
    if (c >= 0xAC && c <= 0xAF) return "i";
    if (c == 0xB1) return "n";
    if ((c >= 0xB2 && c <= 0xB6) || c == 0xB8) return "o";
    if (c >= 0xB9 && c <= 0xBC) return "u";
    if (c == 0xBD || c == 0xBF) return "y";
    return " ";
}

// What a name comes down to for matching: lower-case words, without the tags in brackets,
// punctuation, accents, "the" and "and" ("Legend of Zelda, The - A Link Between Worlds (USA)"
// and "The Legend of Zelda: A Link Between Worlds" both give "legend of zelda a link between
// worlds"; the server writes "&" as "_", so "and" has to go too).
std::string Normalize(std::string_view s) {
    std::string flat;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '(' || c == '[') {
            ++depth;
            continue;
        }
        if (c == ')' || c == ']') {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (depth > 0) {
            continue;
        }
        if (c == 0xC3 && i + 1 < s.size()) {
            flat += Unaccent(static_cast<unsigned char>(s[++i]));
        } else if (c == '\'') {
            // "Don't" and "Dont" are the same game.
        } else if (std::isalnum(c)) {
            flat += static_cast<char>(std::tolower(c));
        } else {
            flat += ' ';
        }
    }
    std::string out;
    std::size_t pos = 0;
    while (pos < flat.size()) {
        const std::size_t start = flat.find_first_not_of(' ', pos);
        if (start == std::string::npos) {
            break;
        }
        std::size_t end = flat.find(' ', start);
        if (end == std::string::npos) {
            end = flat.size();
        }
        const std::string_view word{flat.data() + start, end - start};
        if (word != "the" && word != "and") {
            if (!out.empty()) out += ' ';
            out += word;
        }
        pos = end;
    }
    return out;
}

// Which of several releases' pictures to take: USA and World first, prototypes and demos last.
int ReleaseScore(const std::string& name) {
    const std::string n = Lower(name);
    int score = 10;
    if (n.find("(usa") != std::string::npos || n.find(", usa") != std::string::npos) score = 30;
    else if (n.find("(world") != std::string::npos) score = 28;
    else if (n.find("(europe") != std::string::npos || n.find(", europe") != std::string::npos) score = 20;
    else if (n.find("(australia") != std::string::npos) score = 15;
    else if (n.find("(japan") != std::string::npos) score = 5;
    for (const char* bad : {"(beta", "(proto", "(demo", "(sample", "(kiosk", "(debug", "[b]"}) {
        if (n.find(bad) != std::string::npos) score -= 25;
    }
    if (n.find("(rev") != std::string::npos) score -= 1;
    return score;
}

int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_stop.load() ? 1 : 0;
}

std::size_t AppendBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

enum class Got { Ok, NotFound, Failed };

// GETs `url`. NotFound is the server saying there's no such file; Failed is anything else
// (no network, a timeout, cancelled).
Got FetchStatus(const std::string& url, std::string& body, long timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return Got::Failed;
    }
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc == CURLE_OK && status == 200) {
        return Got::Ok;
    }
    return status == 404 || status == 410 ? Got::NotFound : Got::Failed;
}


// The picture names in a directory listing: every link to a .png in that folder.
std::vector<std::string> ParseListing(const std::string& html) {
    std::vector<std::string> names;
    std::size_t pos = 0;
    while ((pos = html.find("href=\"", pos)) != std::string::npos) {
        pos += 6;
        const std::size_t end = html.find('"', pos);
        if (end == std::string::npos) {
            break;
        }
        std::string href = html.substr(pos, end - pos);
        pos = end;
        if (href.find('/') != std::string::npos || href.size() <= 4 ||
            Lower(href.substr(href.size() - 4)) != ".png") {
            continue;
        }
        for (std::size_t amp; (amp = href.find("&amp;")) != std::string::npos;) {
            href.replace(amp, 5, "&");
        }
        std::string name = UrlDecode(href);
        name.resize(name.size() - 4);
        names.push_back(std::move(name));
    }
    return names;
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return lines;
    }
    std::string line;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line += static_cast<char>(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    std::fclose(f);
    return lines;
}

bool WriteFileAtomically(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".part";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// A library's picture names, from the SD card if the saved list is recent enough.
bool LoadLibrary(const std::string& library, std::vector<std::string>& names, bool& fresh) {
    const std::string cache = std::string{kCacheDir} + "/boxarts-" + SafeName(library) + ".txt";
    std::vector<std::string> lines = ReadLines(cache);
    if (lines.size() >= 1 && lines[0].rfind(kListMagic, 0) == 0) {
        const std::time_t saved = static_cast<std::time_t>(std::atoll(lines[0].c_str() + std::strlen(kListMagic)));
        if (std::time(nullptr) - saved < kListMaxAge) {
            names.assign(lines.begin() + 1, lines.end());
            fresh = false;
            return true;
        }
    }
    std::string html;
    bool listed = false, reached = false;
    for (int s = 0; s < 2 && !listed && !g_stop.load(); ++s) {
        const std::string url = std::string{kServers[s]} + UrlEncode(library) + "/Named_Boxarts/";
        const Got got = FetchStatus(url, html, 60);
        reached = reached || got != Got::Failed;
        if (got == Got::Ok) {
            g_server = s;
            listed = true;
        }
    }
    if (!listed) {
        std::printf("Covers: couldn't list the box art for %s\n", library.c_str());
        if (!reached) {
            // No way through at all: offline. Try again next time rather than library by library.
            g_stop = true;
        }
        return false;
    }
    names = ParseListing(html);
    if (names.empty()) {
        return false;
    }
    std::string text = std::string{kListMagic} + " " + std::to_string(static_cast<long long>(std::time(nullptr))) + "\n";
    for (const std::string& n : names) {
        text += n;
        text += '\n';
    }
    mkdir(kRoot, 0777);
    mkdir(kCacheDir, 0777);
    WriteFileAtomically(cache, text);
    fresh = true;
    return true;
}

// Lower-case stems of the pictures already in the covers folder.
std::set<std::string> ExistingCovers() {
    std::set<std::string> out;
    DIR* d = opendir(kCoverDir);
    if (!d) {
        return out;
    }
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        const std::size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            out.insert(Lower(name.substr(0, dot)));
        }
    }
    closedir(d);
    return out;
}

void Worker(std::vector<Request> requests) {
    // Core 2 is the menu's spare; the downloads mostly wait on the network anyway.
    Common::Horizon::PinCurrentThread(2);
    const std::set<std::string> have = ExistingCovers();
    // One library at a time, so each list is fetched once.
    std::sort(requests.begin(), requests.end(),
              [](const Request& a, const Request& b) { return a.library < b.library; });
    int saved = 0;
    std::size_t i = 0;
    while (i < requests.size() && !g_stop.load()) {
        const std::string library = requests[i].library;
        std::size_t j = i;
        while (j < requests.size() && requests[j].library == library) {
            ++j;
        }
        std::vector<std::string> names;
        bool fresh = false;
        if (LoadLibrary(library, names, fresh)) {
            // Games this library had nothing for are remembered until its list is fetched again.
            const std::string missing_path = std::string{kCacheDir} + "/boxarts-missing-" + SafeName(library) + ".txt";
            if (fresh) {
                std::remove(missing_path.c_str());
            }
            std::vector<std::string> missing_lines = ReadLines(missing_path);
            std::set<std::string> missing(missing_lines.begin(), missing_lines.end());
            const std::size_t missing_before = missing.size();

            const std::set<std::string> exact(names.begin(), names.end());
            std::unordered_map<std::string, std::pair<int, const std::string*>> best;
            for (const std::string& n : names) {
                const std::string key = Normalize(n);
                if (key.empty()) continue;
                const int score = ReleaseScore(n);
                auto [it, inserted] = best.try_emplace(key, score, &n);
                if (!inserted && score > it->second.first) {
                    it->second = {score, &n};
                }
            }
            for (std::size_t k = i; k < j && !g_stop.load(); ++k) {
                const Request& r = requests[k];
                // A cover put in by hand (named after the file or the title) stays.
                if (have.count(Lower(r.save_as)) || have.count(Lower(r.title)) || missing.count(r.rom)) {
                    continue;
                }
                // The file's own name first (dumps are usually named like the library), then
                // the file name and the title with the tags and punctuation taken out.
                const std::string* pick = nullptr;
                if (const auto e = exact.find(r.stem); e != exact.end()) {
                    pick = &*e;
                }
                for (const std::string* candidate : {&r.stem, &r.title}) {
                    if (pick || candidate->empty()) break;
                    if (const auto b = best.find(Normalize(*candidate)); b != best.end()) {
                        pick = b->second.second;
                    }
                }
                if (!pick) {
                    missing.insert(r.rom);
                    continue;
                }
                std::string png;
                const std::string path = UrlEncode(library) + "/Named_Boxarts/" + UrlEncode(*pick) + ".png";
                Got got = FetchStatus(std::string{kServers[g_server.load()]} + path, png, 30);
                if (got == Got::Failed && !g_stop.load()) {
                    // The other way in, and stay with it if it works.
                    const int other = 1 - g_server.load();
                    got = FetchStatus(std::string{kServers[other]} + path, png, 30);
                    if (got != Got::Failed) {
                        g_server = other;
                    }
                }
                if (got == Got::NotFound) {
                    missing.insert(r.rom);
                    continue;
                }
                if (got == Got::Failed) {
                    // Offline, or the server is down: nothing is marked missing, and the rest can
                    // wait for the next time.
                    g_stop = true;
                    break;
                }
                if (png.size() < 8 || png.compare(0, 4, "\x89PNG") != 0) {
                    missing.insert(r.rom);
                    continue;
                }
                mkdir(kRoot, 0777);
                mkdir(kCoverDir, 0777);
                if (WriteFileAtomically(std::string{kCoverDir} + "/" + r.save_as + ".png", png)) {
                    ++saved;
                    g_updated = true;
                    std::printf("Covers: %s -> %s\n", r.title.c_str(), pick->c_str());
                }
            }
            if (missing.size() != missing_before) {
                std::string text;
                for (const std::string& m : missing) {
                    text += m;
                    text += '\n';
                }
                mkdir(kCacheDir, 0777);
                WriteFileAtomically(missing_path, text);
            }
        }
        i = j;
    }
    g_finished = saved;
    g_running = false;
}

} // namespace

bool RequestFor(const GameEntry& game, Request& out) {
    const char* library = LibraryFor(game);
    if (!library || Art::HasPickedArt(game)) {
        return false;
    }
    out.library = library;
    out.rom = game.path;
    out.stem = Stem(game.path);
    out.title = game.title;
    if (game.installed) {
        // Installed titles' files all have the same few names; the title ID tells them apart.
        char id[20];
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(game.program_id));
        out.save_as = id;
    } else {
        out.save_as = out.stem;
    }
    return !out.save_as.empty() && out.save_as != "0000000000000000";
}

void Start(std::vector<Request> requests) {
    std::lock_guard lock{g_mutex};
    if (g_running.load() || requests.empty()) {
        return;
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
    // Initialised here, on the menu's thread, and never torn down: the updater's own
    // init/cleanup pairs then only move the count.
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) {
        return;
    }
    g_stop = false;
    g_running = true;
    g_thread = std::thread(Worker, std::move(requests));
}

bool TakeUpdates() {
    return g_updated.exchange(false);
}

int TakeFinishedCount() {
    if (g_running.load()) {
        return 0;
    }
    return g_finished.exchange(0);
}

bool IsRunning() {
    return g_running.load();
}

void Stop() {
    std::lock_guard lock{g_mutex};
    g_stop = true;
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running = false;
}

} // namespace SwitchFrontend::Covers
