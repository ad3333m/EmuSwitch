// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/steamgriddb.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <thread>

#include <curl/curl.h>
#include <json.hpp>

#include "citra_switch/custom_art.h"
#include "common/horizon_thread.h"

namespace SwitchFrontend::SteamGrid {
namespace {

constexpr const char* kApi = "https://www.steamgriddb.com/api/v2";
constexpr const char* kRoot = "sdmc:/switch/emuswitch";
constexpr const char* kCacheParent = "sdmc:/switch/emuswitch/cache";
constexpr const char* kCacheDir = "sdmc:/switch/emuswitch/cache/steamgriddb";
constexpr const char* kUserAgent = "EmuSwitch/" DEKOPON_VERSION;
// Square pictures fit the Home tiles best, so they come first; portrait ones fill the rest.
constexpr const char* kSquare = "512x512,1024x1024";
constexpr const char* kPortrait = "600x900,342x482,660x930";
constexpr std::size_t kMaxPictures = 20;
// Thumbnails are decoded this big: enough for the picker's preview panel.
constexpr int kThumbSide = 360;

struct Item {
    int width = 0;
    int height = 0;
    std::string style;
    std::string url;
    std::string thumb_url;
    Gfx::Image thumb;
    bool loaded = false;
};

std::mutex g_thread_mutex; // guards g_thread
std::thread g_thread;
std::atomic<bool> g_stop{false};

std::mutex g_mutex; // guards the state below
std::condition_variable g_wake;
Stage g_stage = Stage::Idle;
std::string g_game;
std::string g_error;
std::string g_saved;
std::vector<Item> g_items;
int g_save_index = -1;
std::string g_save_dest;
unsigned g_generation = 0;
unsigned g_version = 1;

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

// Letters and digits only, lower case, single spaces: for comparing names.
std::string Simplify(std::string_view s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != ' ') {
            out += ' ';
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_stop.load() ? 1 : 0;
}

std::size_t AppendBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

// GETs `url`; the API key goes along only to SteamGridDB's API. Returns the HTTP status, or 0
// if there was no answer (no network, a timeout, cancelled).
long Get(const std::string& url, const std::string& api_key, std::string& body, long timeout) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        return 0;
    }
    curl_slist* headers = nullptr;
    if (!api_key.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
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
    curl_slist_free_all(headers);
    return rc == CURLE_OK ? status : 0;
}

// The picture format from its first bytes, as a file extension; empty if it isn't one.
const char* PictureExtension(const std::string& data) {
    if (data.size() >= 8 && data.compare(0, 4, "\x89PNG") == 0) return "png";
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8 && static_cast<unsigned char>(data[2]) == 0xFF)
        return "jpg";
    if (data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) return "webp";
    return "";
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

// Publishes a change for Poll(); `update` runs under the lock.
template <typename F>
void Publish(F&& update) {
    std::lock_guard lock{g_mutex};
    update();
    ++g_version;
}

void Fail(std::string message) {
    Publish([&] {
        g_stage = Stage::Failed;
        g_error = std::move(message);
    });
}

std::string ApiError(long status) {
    if (status == 0) return "Couldn't reach SteamGridDB. Check the internet connection.";
    if (status == 401 || status == 403) return "SteamGridDB didn't accept the API key. Check it in Settings > Advanced.";
    if (status == 429) return "SteamGridDB is busy. Try again in a minute.";
    return "SteamGridDB answered with an error (" + std::to_string(status) + ").";
}

// The data array of an API answer, or null if it isn't one.
const nlohmann::json* Data(const nlohmann::json& answer) {
    if (!answer.is_object()) return nullptr;
    const auto it = answer.find("data");
    return it != answer.end() && it->is_array() ? &*it : nullptr;
}

std::string Text(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int Number(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number_integer() ? it->get<int>() : 0;
}

// Finds the game: an exact name match if there is one, else SteamGridDB's best guess.
bool FindGame(const std::string& term, const std::string& key, long long& id, std::string& name) {
    std::string body;
    const long status = Get(std::string{kApi} + "/search/autocomplete/" + UrlEncode(term), key, body, 20);
    if (g_stop.load()) return false;
    if (status == 404) {
        Fail("SteamGridDB has no game called \"" + term + "\". Press X to search another name.");
        return false;
    }
    if (status != 200) {
        Fail(ApiError(status));
        return false;
    }
    const nlohmann::json answer = nlohmann::json::parse(body, nullptr, false);
    const nlohmann::json* data = Data(answer);
    if (!data) {
        Fail("SteamGridDB sent something unexpected. Try again later.");
        return false;
    }
    const std::string want = Simplify(term);
    const nlohmann::json* best = nullptr;
    for (const nlohmann::json& game : *data) {
        if (!game.is_object() || !game.contains("id") || !game["id"].is_number_integer()) continue;
        if (!best) best = &game;
        if (Simplify(Text(game, "name")) == want) {
            best = &game;
            break;
        }
    }
    if (!best) {
        Fail("SteamGridDB has no game called \"" + term + "\". Press X to search another name.");
        return false;
    }
    id = (*best)["id"].get<long long>();
    name = Text(*best, "name");
    return true;
}

// Adds the game's pictures in `dimensions` to `items`, up to kMaxPictures.
bool ListGrids(long long id, const char* dimensions, const std::string& key, std::vector<Item>& items) {
    std::string body;
    const std::string url = std::string{kApi} + "/grids/game/" + std::to_string(id) + "?dimensions=" +
                            UrlEncode(dimensions) + "&types=static&nsfw=false&humor=false";
    const long status = Get(url, key, body, 20);
    if (g_stop.load()) return false;
    if (status == 404) return true; // nothing in these sizes
    if (status != 200) {
        Fail(ApiError(status));
        return false;
    }
    const nlohmann::json answer = nlohmann::json::parse(body, nullptr, false);
    const nlohmann::json* data = Data(answer);
    if (!data) {
        Fail("SteamGridDB sent something unexpected. Try again later.");
        return false;
    }
    for (const nlohmann::json& grid : *data) {
        if (items.size() >= kMaxPictures) break;
        if (!grid.is_object()) continue;
        Item item;
        item.url = Text(grid, "url");
        item.thumb_url = Text(grid, "thumb");
        item.width = Number(grid, "width");
        item.height = Number(grid, "height");
        item.style = Text(grid, "style");
        if (item.url.rfind("https://", 0) != 0) continue;
        if (item.thumb_url.rfind("https://", 0) != 0) item.thumb_url = item.url;
        items.push_back(std::move(item));
    }
    return true;
}

void LoadThumb(unsigned generation, std::size_t index, const std::string& url) {
    std::string data;
    Gfx::Image img;
    if (Get(url, "", data, 20) == 200) {
        if (const char* ext = PictureExtension(data); *ext) {
            mkdir(kRoot, 0777);
            mkdir(kCacheParent, 0777);
            mkdir(kCacheDir, 0777);
            const std::string path = std::string{kCacheDir} + "/thumb." + ext;
            if (WriteFileAtomically(path, data)) {
                Art::DecodePicture(path, kThumbSide, img);
                std::remove(path.c_str());
            }
        }
    }
    Publish([&] {
        if (generation == g_generation && index < g_items.size()) {
            g_items[index].thumb = std::move(img);
            g_items[index].loaded = true;
        }
    });
}

void SavePicture(const std::string& url, const std::string& dest_stem) {
    std::string data;
    const long status = Get(url, "", data, 60);
    if (g_stop.load()) return;
    const char* ext = status == 200 ? PictureExtension(data) : "";
    std::string saved;
    if (*ext) {
        // Everything up to the file's folder, then the file; any older download in another
        // format goes.
        std::string dir = dest_stem.substr(0, dest_stem.find_last_of('/'));
        for (std::size_t slash = dir.find('/', std::string{"sdmc:/"}.size()); slash != std::string::npos;
             slash = dir.find('/', slash + 1)) {
            mkdir(dir.substr(0, slash).c_str(), 0777);
        }
        mkdir(dir.c_str(), 0777);
        for (const char* other : {"png", "jpg", "webp"}) {
            if (std::string_view{other} != ext) std::remove((dest_stem + "." + other).c_str());
        }
        const std::string path = dest_stem + "." + ext;
        if (WriteFileAtomically(path, data)) saved = path;
    }
    Publish([&] {
        if (!saved.empty()) {
            g_stage = Stage::Saved;
            g_saved = saved;
            g_error.clear();
        } else {
            g_stage = Stage::Ready;
            g_error = status == 200 ? "That picture couldn't be saved." : "Couldn't download that picture. Try again.";
        }
    });
}

void Worker(std::string term, std::string key, unsigned generation) {
    // Core 2 is the menu's spare; this mostly waits on the network anyway.
    Common::Horizon::PinCurrentThread(2);
    long long id = 0;
    std::string name;
    if (!FindGame(term, key, id, name)) return;
    std::vector<Item> items;
    if (!ListGrids(id, kSquare, key, items) || !ListGrids(id, kPortrait, key, items)) return;
    std::vector<std::string> thumbs;
    for (const Item& item : items) thumbs.push_back(item.thumb_url);
    Publish([&] {
        g_stage = Stage::Ready;
        g_game = name;
        g_items = std::move(items);
    });

    // Thumbnails one by one; a picture picked meanwhile is downloaded first.
    std::size_t next = 0;
    while (!g_stop.load()) {
        int save = -1;
        std::string url, dest;
        {
            std::unique_lock lock{g_mutex};
            if (g_save_index < 0 && next >= thumbs.size()) {
                g_wake.wait(lock, [] { return g_stop.load() || g_save_index >= 0; });
            }
            if (g_stop.load()) break;
            if (g_save_index >= 0) {
                save = g_save_index;
                g_save_index = -1;
                url = static_cast<std::size_t>(save) < g_items.size() ? g_items[static_cast<std::size_t>(save)].url : "";
                dest = g_save_dest;
            }
        }
        if (save >= 0) {
            SavePicture(url, dest);
        } else {
            LoadThumb(generation, next, thumbs[next]);
            ++next;
        }
    }
}

} // namespace

std::string SearchTerm(const std::string& title) {
    // Without the tags in brackets.
    std::string plain;
    int depth = 0;
    for (const char c : title) {
        if (c == '(' || c == '[') {
            ++depth;
        } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
        } else if (depth == 0) {
            plain += c;
        }
    }
    // "Name, The - Subtitle" -> "The Name: Subtitle", a part at a time.
    std::vector<std::string> parts;
    for (std::size_t start = 0;;) {
        const std::size_t dash = plain.find(" - ", start);
        parts.push_back(plain.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
        if (dash == std::string::npos) break;
        start = dash + 3;
    }
    std::string out;
    for (std::string part : parts) {
        while (!part.empty() && part.back() == ' ') part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        for (const char* article : {"The", "A", "An"}) {
            const std::string suffix = std::string{", "} + article;
            if (part.size() > suffix.size() && part.compare(part.size() - suffix.size(), suffix.size(), suffix) == 0) {
                part = std::string{article} + " " + part.substr(0, part.size() - suffix.size());
                break;
            }
        }
        if (part.empty()) continue;
        if (!out.empty()) out += ": ";
        out += part;
    }
    return out.empty() ? title : out;
}

void Search(const std::string& term, const std::string& api_key) {
    Cancel();
    // Initialised on the menu's thread and never torn down, like the cover downloader's.
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    unsigned generation;
    {
        std::lock_guard lock{g_mutex};
        generation = ++g_generation;
        ++g_version;
        g_items.clear();
        g_game.clear();
        g_error.clear();
        g_saved.clear();
        g_save_index = -1;
        g_stage = curl_ready ? Stage::Searching : Stage::Failed;
        if (!curl_ready) {
            g_error = "The network couldn't be set up.";
            return;
        }
    }
    std::lock_guard lock{g_thread_mutex};
    g_stop = false;
    g_thread = std::thread(Worker, term, api_key, generation);
}

void Save(int index, const std::string& dest_stem) {
    std::lock_guard lock{g_mutex};
    if (g_stage != Stage::Ready || index < 0 || static_cast<std::size_t>(index) >= g_items.size()) {
        return;
    }
    g_save_index = index;
    g_save_dest = dest_stem;
    g_stage = Stage::Saving;
    g_error.clear();
    ++g_version;
    g_wake.notify_all();
}

bool Poll(Results& out) {
    std::lock_guard lock{g_mutex};
    if (out.generation != g_generation) {
        out = Results{};
        out.generation = g_generation;
        out.version = 0;
    }
    if (out.version == g_version) {
        return false;
    }
    out.version = g_version;
    out.stage = g_stage;
    out.game = g_game;
    out.error = g_error;
    out.saved = g_saved;
    if (out.pictures.size() < g_items.size()) {
        out.pictures.resize(g_items.size());
    }
    for (std::size_t i = 0; i < g_items.size(); ++i) {
        Item& item = g_items[i];
        Picture& p = out.pictures[i];
        p.width = item.width;
        p.height = item.height;
        p.style = item.style;
        if (item.loaded && !p.loaded) {
            p.thumb = std::move(item.thumb);
            p.loaded = true;
        }
    }
    return true;
}

void Cancel() {
    std::lock_guard lock{g_thread_mutex};
    {
        // Set under the lock, so a worker about to wait can't miss it.
        std::lock_guard state{g_mutex};
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_thread.joinable()) {
        g_thread.join();
    }
    std::lock_guard state{g_mutex};
    if (g_stage == Stage::Searching || g_stage == Stage::Saving) {
        g_stage = Stage::Idle;
        ++g_version;
    }
}

} // namespace SwitchFrontend::SteamGrid
