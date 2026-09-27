// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <unordered_map>

#include <sys/iosupport.h>
#include <sys/stat.h>

#include <fmt/format.h>

#include "citra_switch/config.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/multi_system.h"
#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/zstd_compression.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

namespace SwitchFrontend {

namespace {

constexpr std::uint64_t kTidHighMask = 0xFFFFFFFF00000000ULL;
constexpr std::uint64_t kTidHighApplication = 0x0004000000000000ULL;
constexpr std::uint64_t kTidHighDemo = 0x0004000200000000ULL;
constexpr std::uint64_t kTidHighUpdate = 0x0004000E00000000ULL;
constexpr std::uint64_t kTidHighDlc = 0x0004008C00000000ULL;
constexpr std::uint64_t kTidHighSystemApplication = 0x0004001000000000ULL;

// Don't scan updates/dlcs into the library window.
constexpr std::array<std::uint64_t, 2> kLibraryTidHighs{kTidHighApplication, kTidHighDemo};

// The per-backend cache subdirectories of the shader directory.
constexpr std::array<const char*, 2> kShaderCacheDirs{"vulkan", "opengl"};

std::string ShaderCacheDir(const char* backend) {
    return FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + backend;
}

std::uint64_t DirectorySize(const std::string& directory) {
    FileUtil::FSTEntry root;
    FileUtil::ScanDirectoryTree(directory, root, 8);
    std::vector<FileUtil::FSTEntry> files;
    FileUtil::GetAllFilesFromNestedEntries(root, files);

    std::uint64_t total = 0;
    for (const FileUtil::FSTEntry& file : files) {
        total += file.size;
    }
    return total;
}

// Decode game icons
std::uint32_t Rgb565ToRgba8888(std::uint16_t c) {
    const std::uint32_t r5 = (c >> 11) & 0x1F;
    const std::uint32_t g6 = (c >> 5) & 0x3F;
    const std::uint32_t b5 = c & 0x1F;
    const std::uint32_t r = (r5 << 3) | (r5 >> 2);
    const std::uint32_t g = (g6 << 2) | (g6 >> 4);
    const std::uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Trims a UTF-16 title into a single clean UTF-8 line.
std::string CleanTitle(const std::array<char16_t, 0x80>& raw) {
    std::u16string u16{raw.data(),
                       std::char_traits<char16_t>::length(raw.data()) > raw.size()
                           ? raw.size()
                           : std::char_traits<char16_t>::length(raw.data())};
    std::string out = Common::UTF16ToUTF8(u16);
    for (char& ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    // Collapse the runs of spaces the newline replacement leaves.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (char ch : out) {
        const bool space = ch == ' ';
        if (space && prev_space) {
            continue;
        }
        collapsed.push_back(ch);
        prev_space = space;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

void FillFromSmdh(GameEntry& entry, const Loader::SMDH& smdh) {
    using Language = Loader::SMDH::TitleLanguage;
    const auto title = CleanTitle(smdh.GetLongTitle(Language::English));
    if (!title.empty()) {
        entry.title = title;
    }
    const auto& pub = smdh.titles[static_cast<std::size_t>(Language::English)].publisher;
    entry.publisher = Common::UTF16ToUTF8(std::u16string{pub.data(),
        std::char_traits<char16_t>::length(pub.data())});

    const std::vector<u16> icon = smdh.GetIcon(true);
    if (icon.size() == 48 * 48) {
        entry.icon_size = 48;
        entry.icon.resize(icon.size());
        std::transform(icon.begin(), icon.end(), entry.icon.begin(), Rgb565ToRgba8888);
    }
}

// Reads one candidate file into a GameEntry, or returns false if it isn't a title.
// `fallback_title` names the entry when there is no SMDH to read a long title out of.
bool TryLoad(const std::string& path, const std::string& fallback_title, GameEntry& entry) {
    std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path);
    if (!loader) {
        return false;
    }
    bool is_executable = false;
    if (loader->IsExecutable(is_executable) != Loader::ResultStatus::Success || !is_executable) {
        return false;
    }

    entry.path = path;
    entry.title = fallback_title;
    entry.file_type = Loader::GetFileTypeString(loader->GetFileType(), loader->IsFileCompressed());
    entry.insertable = loader->GetFileType() == Loader::FileType::CCI;
    loader->ReadProgramId(entry.program_id);

    std::vector<u8> smdh_buffer;
    const Loader::ResultStatus icon_result = loader->ReadIcon(smdh_buffer);
    if (icon_result == Loader::ResultStatus::ErrorEncrypted) {
        entry.encrypted = true;
    } else if (icon_result == Loader::ResultStatus::Success &&
               Loader::IsValidSMDH(smdh_buffer)) {
        Loader::SMDH smdh;
        std::memcpy(&smdh, smdh_buffer.data(), sizeof(Loader::SMDH));
        FillFromSmdh(entry, smdh);
    }
    return true;
}

// ---- the scan cache ----
//
// Reading a title's SMDH means opening the file and walking its container (decompressing it,
// for .z3ds and friends), which is most of what makes a library slow to appear. What was read
// is kept in user/cache/library.bin, keyed by path, size and modification time; a file that
// hasn't changed is taken from there. Files that turned out not to be titles are remembered
// too, so they aren't probed again.

struct ScanRecord {
    std::uint64_t size{};
    std::int64_t mtime{};
    bool is_title{};
    GameEntry entry;
};

constexpr std::uint32_t kScanCacheMagic = 0x43534D45; // "EMSC"
constexpr std::uint32_t kScanCacheVersion = 1;

std::unordered_map<std::string, ScanRecord> s_scan_cache;
std::unordered_map<std::string, ScanRecord> s_scan_seen; // what this scan used
bool s_scan_cache_loaded = false;
bool s_scan_cache_dirty = false;

std::string ScanCachePath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "cache/library.bin";
}

class ScanCacheReader {
public:
    explicit ScanCacheReader(std::FILE* f) : file{f} {}
    bool ok = true;
    template <typename T>
    T Get() {
        T v{};
        ok = ok && std::fread(&v, sizeof(T), 1, file) == 1;
        return v;
    }
    std::string Str() {
        const auto n = Get<std::uint32_t>();
        if (!ok || n > 4096) {
            ok = false;
            return {};
        }
        std::string s(n, '\0');
        ok = ok && (n == 0 || std::fread(s.data(), 1, n, file) == n);
        return s;
    }

private:
    std::FILE* file;
};

void LoadScanCache() {
    if (s_scan_cache_loaded) {
        return;
    }
    s_scan_cache_loaded = true;
    std::FILE* f = std::fopen(ScanCachePath().c_str(), "rb");
    if (!f) {
        return;
    }
    ScanCacheReader r{f};
    const auto magic = r.Get<std::uint32_t>();
    const auto version = r.Get<std::uint32_t>();
    const auto count = r.Get<std::uint32_t>();
    if (r.ok && magic == kScanCacheMagic && version == kScanCacheVersion && count < 100000) {
        for (std::uint32_t i = 0; i < count && r.ok; ++i) {
            std::string path = r.Str();
            ScanRecord rec;
            rec.size = r.Get<std::uint64_t>();
            rec.mtime = r.Get<std::int64_t>();
            rec.is_title = r.Get<std::uint8_t>() != 0;
            if (rec.is_title) {
                GameEntry& e = rec.entry;
                e.path = path;
                e.title = r.Str();
                e.publisher = r.Str();
                e.file_type = r.Str();
                e.encrypted = r.Get<std::uint8_t>() != 0;
                e.insertable = r.Get<std::uint8_t>() != 0;
                e.program_id = r.Get<std::uint64_t>();
                e.icon_size = r.Get<std::int32_t>();
                const auto pixels = r.Get<std::uint32_t>();
                if (!r.ok || pixels > 64 * 64 ||
                    (pixels != 0 && pixels != static_cast<std::uint32_t>(e.icon_size * e.icon_size))) {
                    r.ok = false;
                    break;
                }
                e.icon.resize(pixels);
                r.ok = r.ok && (pixels == 0 || std::fread(e.icon.data(), sizeof(std::uint32_t), pixels, f) == pixels);
            }
            if (r.ok) {
                s_scan_cache.emplace(std::move(path), std::move(rec));
            }
        }
    }
    std::fclose(f);
    if (!r.ok) {
        // A damaged cache costs one slow scan, nothing more.
        s_scan_cache.clear();
    }
}

void SaveScanCache() {
    if (!s_scan_cache_dirty && s_scan_seen.size() == s_scan_cache.size()) {
        return;
    }
    const std::string path = ScanCachePath();
    FileUtil::CreateFullPath(path);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return;
    }
    bool ok = true;
    auto put = [&](const void* data, std::size_t n) { ok = ok && (n == 0 || std::fwrite(data, 1, n, f) == n); };
    auto put_str = [&](const std::string& str) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(str.size(), 4096));
        put(&n, sizeof(n));
        put(str.data(), n);
    };
    const std::uint32_t header[3] = {kScanCacheMagic, kScanCacheVersion,
                                     static_cast<std::uint32_t>(s_scan_seen.size())};
    put(header, sizeof(header));
    for (const auto& [file, rec] : s_scan_seen) {
        put_str(file);
        put(&rec.size, sizeof(rec.size));
        put(&rec.mtime, sizeof(rec.mtime));
        const std::uint8_t is_title = rec.is_title ? 1 : 0;
        put(&is_title, 1);
        if (rec.is_title) {
            const GameEntry& e = rec.entry;
            put_str(e.title);
            put_str(e.publisher);
            put_str(e.file_type);
            const std::uint8_t flags[2] = {static_cast<std::uint8_t>(e.encrypted), static_cast<std::uint8_t>(e.insertable)};
            put(flags, 2);
            put(&e.program_id, sizeof(e.program_id));
            const std::int32_t icon_size = e.icon_size;
            put(&icon_size, sizeof(icon_size));
            const auto pixels = static_cast<std::uint32_t>(e.icon.size());
            put(&pixels, sizeof(pixels));
            put(e.icon.data(), e.icon.size() * sizeof(std::uint32_t));
        }
    }
    ok = std::fclose(f) == 0 && ok;
    if (ok) {
        std::remove(path.c_str());
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) {
        std::remove(tmp.c_str());
    }
    s_scan_cache = s_scan_seen;
    s_scan_cache_dirty = false;
}

// TryLoad through the cache.
bool TryLoadCached(const std::string& path, const std::string& fallback_title, GameEntry& entry) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return TryLoad(path, fallback_title, entry);
    }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    const auto mtime = static_cast<std::int64_t>(st.st_mtime);
    if (const auto it = s_scan_cache.find(path);
        it != s_scan_cache.end() && it->second.size == size && it->second.mtime == mtime) {
        s_scan_seen[path] = it->second;
        if (!it->second.is_title) {
            return false;
        }
        entry = it->second.entry;
        return true;
    }
    ScanRecord rec;
    rec.size = size;
    rec.mtime = mtime;
    rec.is_title = TryLoad(path, fallback_title, rec.entry);
    if (rec.is_title) {
        entry = rec.entry;
    }
    s_scan_seen[path] = std::move(rec);
    s_scan_cache_dirty = true;
    return s_scan_seen[path].is_title;
}

void ScanDirectory(const std::string& directory, std::vector<GameEntry>& out, int depth,
                   bool recursive) {
    if (depth > 4) {
        return;
    }
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out, depth, recursive](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                if (recursive) {
                    ScanDirectory(path + '/', out, depth + 1, recursive);
                }
                return true;
            }
            GameEntry entry;
            if (TryLoadCached(path, std::string{FileUtil::GetFilename(path)}, entry)) {
                out.push_back(std::move(entry));
            }
            return true;
        });
}

// Parses a title tree folder name ("00053f00") into the low word of a title ID.
bool ParseTidLow(const std::string& name, std::uint32_t& out) {
    if (name.size() != 8) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : name) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    out = value;
    return true;
}

// Adds the titles under one title-high folder of a media's title tree.
void ScanInstalledHigh(Service::FS::MediaType media, std::uint64_t high,
                       std::vector<GameEntry>& out) {
    const std::string dir =
        fmt::format("{}{:08x}/", Service::AM::GetMediaTitlePath(media), high >> 32);
    FileUtil::ForeachDirectoryEntry(
        nullptr, dir,
        [&out, media, high](u64*, const std::string& parent, const std::string& virtual_name) {
            if (!FileUtil::IsDirectory(parent + virtual_name)) {
                return true;
            }
            std::uint32_t low = 0;
            if (!ParseTidLow(virtual_name, low)) {
                return true;
            }
            const std::uint64_t tid = high | low;
            // Resolves the boot content through the title's TMD, so it picks the same .app
            // the loader would boot.
            const std::string content = Service::AM::GetTitleContentPath(media, tid);
            if (content.empty() || !FileUtil::Exists(content)) {
                return true;
            }
            GameEntry entry;
            if (!TryLoadCached(content, fmt::format("{:016X}", tid), entry)) {
                return true;
            }
            entry.installed = true;
            if (entry.program_id == 0) {
                entry.program_id = tid;
            }
            out.push_back(std::move(entry));
            return true;
        });
}

// Adds the titles installed on the emulated SD card, plus the launchable system applications
// on the emulated NAND
void ScanInstalled(std::vector<GameEntry>& out) {
    for (const std::uint64_t high : kLibraryTidHighs) {
        ScanInstalledHigh(Service::FS::MediaType::SDMC, high, out);
    }
    ScanInstalledHigh(Service::FS::MediaType::NAND, kTidHighSystemApplication, out);
}

// Reads a CIA's header and TMD
bool ReadCiaEntry(const std::string& path, CiaEntry& entry) {
    std::unique_ptr<FileUtil::IOFileBase> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (!file->IsOpen()) {
        return false;
    }
    entry.compressed = FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt;
    if (entry.compressed) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }

    std::vector<u8> header(FileSys::CIA_HEADER_SIZE);
    if (file->ReadBytes(header.data(), header.size()) != header.size()) {
        return false;
    }
    FileSys::CIAContainer container;
    if (container.LoadHeader(header) != Loader::ResultStatus::Success) {
        return false;
    }

    std::vector<u8> tmd(container.GetTitleMetadataSize());
    if (file->ReadAtBytes(tmd.data(), tmd.size(), container.GetTitleMetadataOffset()) !=
            tmd.size() ||
        container.LoadTitleMetadata(tmd) != Loader::ResultStatus::Success) {
        return false;
    }

    entry.program_id = container.GetTitleMetadata().GetTitleID();
    entry.kind = ClassifyTitle(entry.program_id);
    entry.version = container.GetTitleMetadata().GetTitleVersion();
    return true;
}

struct InstalledTmd {
    std::uint16_t version{};
    int content_count{};
};

// Reads the TMD of `tid`, or nothing if that title isn't installed.
std::optional<InstalledTmd> ReadInstalledTmd(std::uint64_t tid) {
    FileSys::TitleMetadata tmd;
    const std::string path =
        Service::AM::GetTitleMetadataPath(Service::AM::GetTitleMediaType(tid), tid);
    if (tmd.Load(path) != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    return InstalledTmd{tmd.GetTitleVersion(), static_cast<int>(tmd.GetContentCount())};
}

// The files directly under `directory` carrying one of `extensions`, sorted by name.
std::vector<FileEntry> ListFilesByExtension(const std::string& directory,
                                            std::initializer_list<const char*> extensions) {
    std::vector<FileEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out, &extensions](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                return true;
            }
            std::string extension;
            Common::SplitPath(path, nullptr, nullptr, &extension);
            extension = Common::ToLower(extension);
            for (const char* wanted : extensions) {
                if (extension == wanted) {
                    out.push_back({virtual_name, path});
                    break;
                }
            }
            return true;
        });
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

} // namespace

TitleKind ClassifyTitle(std::uint64_t program_id) {
    switch (program_id & kTidHighMask) {
    case kTidHighApplication:
        return TitleKind::Application;
    case kTidHighDemo:
        return TitleKind::Demo;
    case kTidHighUpdate:
        return TitleKind::Update;
    case kTidHighDlc:
        return TitleKind::AddOnContent;
    default:
        break;
    }
    // Everything the emulated NAND holds.
    return Service::AM::GetTitleMediaType(program_id) == Service::FS::MediaType::NAND
               ? TitleKind::System
               : TitleKind::Other;
}

const char* TitleKindName(TitleKind kind) {
    switch (kind) {
    case TitleKind::Application:
        return "Game";
    case TitleKind::Demo:
        return "Demo";
    case TitleKind::Update:
        return "Update";
    case TitleKind::AddOnContent:
        return "DLC";
    case TitleKind::System:
        return "System";
    default:
        return "Other";
    }
}

std::string FormatTitleVersion(std::uint16_t version) {
    return fmt::format("v{}.{}.{} ({})", (version >> 10) & 0x3F, (version >> 4) & 0x3F,
                       version & 0xF, version);
}

std::string FormatSize(std::uint64_t bytes) {
    constexpr std::array<const char*, 4> units{"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? fmt::format("{:.0f} {}", value, units[unit])
                     : fmt::format("{:.1f} {}", value, units[unit]);
}

std::uint64_t GetShaderCacheSize() {
    std::uint64_t total = 0;
    for (const char* backend : kShaderCacheDirs) {
        total += DirectorySize(ShaderCacheDir(backend));
    }
    return total;
}

std::uint64_t ClearShaderCache() {
    std::uint64_t freed = 0;
    for (const char* backend : kShaderCacheDirs) {
        const std::string dir = ShaderCacheDir(backend);
        if (!FileUtil::IsDirectory(dir)) {
            continue;
        }
        const std::uint64_t size = DirectorySize(dir);
        if (FileUtil::DeleteDirRecursively(dir)) {
            freed += size;
        } else {
            LOG_ERROR(Frontend, "Failed to delete shader cache directory {}", dir);
        }
    }
    return freed;
}

TitleDetails GetTitleDetails(const GameEntry& entry) {
    TitleDetails details;
    details.program_id = entry.program_id;
    details.kind = ClassifyTitle(entry.program_id);
    if (entry.program_id == 0) {
        return details;
    }

    if (entry.installed) {
        if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(entry.program_id)) {
            details.has_base_version = true;
            details.base_version = tmd->version;
        }
    }
    if (details.kind != TitleKind::Application && details.kind != TitleKind::Demo) {
        return details;
    }

    // The loader keys the update off the base title's low word.
    const std::uint64_t low = entry.program_id & 0xFFFFFFFFULL;
    if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(kTidHighUpdate | low)) {
        details.has_update = true;
        details.update_version = tmd->version;
    }
    if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(kTidHighDlc | low)) {
        details.has_dlc = true;
        details.dlc_contents = tmd->content_count;
    }
    return details;
}

bool GetInstalledVersion(std::uint64_t program_id, std::uint16_t& version) {
    const std::optional<InstalledTmd> tmd = ReadInstalledTmd(program_id);
    if (!tmd) {
        return false;
    }
    version = tmd->version;
    return true;
}

std::vector<CiaEntry> ListCiaFiles(const std::string& directory) {
    std::vector<CiaEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                return true;
            }
            std::string extension;
            Common::SplitPath(path, nullptr, nullptr, &extension);
            extension = Common::ToLower(extension);
            if (extension != ".cia" && extension != ".zcia") {
                return true;
            }
            CiaEntry entry;
            entry.name = virtual_name;
            entry.path = path;
            entry.size = FileUtil::GetSize(path);
            entry.readable = ReadCiaEntry(path, entry);
            out.push_back(std::move(entry));
            return true;
        });
    std::sort(out.begin(), out.end(), [](const CiaEntry& a, const CiaEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

std::vector<FileEntry> ListAmiiboFiles() {
    return ListFilesByExtension(GetActiveUserDir() + "amiibo/", {".bin"});
}

std::vector<FileEntry> ListCameraImages() {
    return ListFilesByExtension(GetActiveUserDir() + "camera/", {".png", ".jpg", ".jpeg"});
}

InstallResult InstallCia(const std::string& path,
                         const std::function<void(std::size_t, std::size_t)>& progress) {
    const Service::AM::InstallStatus status = Service::AM::InstallCIA(
        path, [&progress](std::size_t written, std::size_t total) {
            if (progress) {
                progress(written, total);
            }
        });
    switch (status) {
    case Service::AM::InstallStatus::Success:
        return InstallResult::Success;
    case Service::AM::InstallStatus::ErrorFileNotFound:
        return InstallResult::FileNotFound;
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        return InstallResult::FailedToOpen;
    case Service::AM::InstallStatus::ErrorAborted:
        return InstallResult::Aborted;
    case Service::AM::InstallStatus::ErrorEncrypted:
        return InstallResult::Encrypted;
    default:
        return InstallResult::Invalid;
    }
}

const char* InstallResultText(InstallResult result) {
    switch (result) {
    case InstallResult::Success:
        return "Installed";
    case InstallResult::FileNotFound:
        return "File not found";
    case InstallResult::FailedToOpen:
        return "Couldn't open the file";
    case InstallResult::Aborted:
        return "Install aborted";
    case InstallResult::Encrypted:
        return "CIA is encrypted. Please decrypt it or add aes_keys.txt";
    default:
        return "Not a valid CIA";
    }
}

std::vector<GameEntry> ScanGames() {
    const SwitchPaths& paths = GetPaths();
    FileUtil::CreateFullPath(paths.roms_dir);

    LoadScanCache();
    s_scan_seen.clear();
    std::vector<GameEntry> games;
    ScanDirectory(paths.roms_dir, games, 0, paths.scan_recursive);
    // Only scan second dir when present.
    if (!paths.roms_dir_2.empty() && paths.roms_dir_2 != paths.roms_dir &&
        FileUtil::IsDirectory(paths.roms_dir_2)) {
        ScanDirectory(paths.roms_dir_2, games, 0, paths.scan_recursive);
    }
    ScanInstalled(games);
    // EmuSwitch: 3DS games in sdmc:/roms/3ds too, and every other system's games.
    const std::string shared_3ds = "sdmc:/roms/3ds/";
    if (shared_3ds != paths.roms_dir && shared_3ds != paths.roms_dir_2 && FileUtil::IsDirectory(shared_3ds)) {
        ScanDirectory(shared_3ds, games, 0, true);
    }
    SaveScanCache();
    Multi::AddGames(games);

    // Lower-case each title once rather than twice per comparison.
    std::vector<std::pair<std::string, std::size_t>> keys;
    keys.reserve(games.size());
    for (std::size_t i = 0; i < games.size(); ++i) {
        keys.emplace_back(Common::ToLower(games[i].title), i);
    }
    std::sort(keys.begin(), keys.end());
    std::vector<GameEntry> sorted;
    sorted.reserve(games.size());
    for (const auto& [key, i] : keys) {
        sorted.push_back(std::move(games[i]));
    }
    return sorted;
}

std::vector<DirEntry> ListDevices() {
    std::vector<DirEntry> out;
    for (int i = 0; i < STD_MAX; ++i) {
        const devoptab_t* device = devoptab_list[i];
        if (device == nullptr || device->name == nullptr) {
            continue;
        }
        // The table also holds the console and the std streams, which have no browsable root.
        const std::string root = std::string{device->name} + ":/";
        if (!FileUtil::IsDirectory(root)) {
            continue;
        }
        out.push_back(DirEntry{std::string{device->name} + ':', root});
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

std::vector<DirEntry> ListSubdirectories(const std::string& directory) {
    std::vector<DirEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                out.push_back(DirEntry{virtual_name, path + '/'});
            }
            return true;
        });
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

std::vector<FileEntry> ListFilesIn(const std::string& directory) {
    std::vector<FileEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (!FileUtil::IsDirectory(path)) {
                out.push_back({virtual_name, path});
            }
            return true;
        });
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

std::string ParentDirectory(const std::string& directory) {
    if (directory.size() <= 1) {
        return "";
    }
    // Skip the trailing '/' so the search lands on the separator above it.
    const std::size_t sep = directory.find_last_of('/', directory.size() - 2);
    if (sep == std::string::npos) {
        return "";
    }
    return directory.substr(0, sep + 1);
}

bool EnsureDirectory(const std::string& directory) {
    return FileUtil::CreateFullPath(directory) && FileUtil::IsDirectory(directory);
}

bool DirectoryExists(const std::string& directory) {
    return !directory.empty() && FileUtil::IsDirectory(directory);
}

} // namespace SwitchFrontend
