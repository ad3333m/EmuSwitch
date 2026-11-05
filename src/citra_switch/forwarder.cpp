// SPDX-FileCopyrightText: EmuSwitch
// SPDX-License-Identifier: GPL-2.0-or-later
//
// HOME Menu forwarders, built in memory and installed on the SD card: a program NCA (the loader
// in tools/forwarder_hbl plus the paths it starts), a control NCA (title and icon) and a meta
// NCA, registered with ncm and ns like any installed software. The approach is Sphaira's
// (ITotalJustice; it follows hacbrewpack): the sections are plaintext and hashed (SHA-256 for
// the PFS0s, IVFC for the RomFSs), and only the NCA header is encrypted, with the console's own
// header key. Like every forwarder, it only runs with sigpatches.

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <switch.h>
#include <jpeglib.h>

#include "citra_switch/forwarder.h"

namespace SwitchFrontend::Forwarder {
namespace {

using Bytes = std::vector<u8>;
using Hash = std::array<u8, 32>;

struct File {
    std::string name;
    Bytes data;
};

void Append(Bytes& out, const void* data, std::size_t size) {
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
}

void PadTo(Bytes& out, std::size_t align) {
    out.resize((out.size() + align - 1) / align * align, 0);
}

template <typename T>
void Put(Bytes& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

template <typename T>
T Get(const Bytes& in, std::size_t offset) {
    T value{};
    std::memcpy(&value, in.data() + offset, sizeof(value));
    return value;
}

Hash Sha256(const void* data, std::size_t size) {
    Hash hash{};
    sha256CalculateHash(hash.data(), data, size);
    return hash;
}

Bytes ToBytes(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

Bytes ReadWholeFile(const char* path) {
    Bytes data;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return data;
    }
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long size = std::ftell(f);
        if (size > 0 && std::fseek(f, 0, SEEK_SET) == 0) {
            data.resize(static_cast<std::size_t>(size));
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
                data.clear();
            }
        }
    }
    std::fclose(f);
    return data;
}

// ---- containers -------------------------------------------------------------------------------

// A PFS0: the ExeFS, and the meta NCA's content.
Bytes BuildPfs0(const std::vector<File>& files) {
    std::string strings;
    std::vector<u32> name_offsets;
    for (const File& f : files) {
        name_offsets.push_back(static_cast<u32>(strings.size()));
        strings += f.name;
        strings += '\0';
    }
    strings.resize((strings.size() + 0x1F) & ~std::size_t{0x1F}, '\0');

    Bytes out;
    const u32 header[4] = {0x30534650u /* PFS0 */, static_cast<u32>(files.size()),
                           static_cast<u32>(strings.size()), 0};
    Append(out, header, sizeof(header));
    u64 offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const u64 place[2] = {offset, static_cast<u64>(files[i].data.size())};
        const u32 name[2] = {name_offsets[i], 0};
        Append(out, place, sizeof(place));
        Append(out, name, sizeof(name));
        offset += files[i].data.size();
    }
    Append(out, strings.data(), strings.size());
    for (const File& f : files) {
        Append(out, f.data.data(), f.data.size());
    }
    return out;
}

u32 RomfsPathHash(u32 parent, std::string_view name) {
    u32 hash = parent ^ 123456789u;
    for (const char c : name) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= static_cast<u8>(c);
    }
    return hash;
}

u32 RomfsBuckets(u32 entries) {
    if (entries < 3) {
        return 3;
    }
    if (entries < 19) {
        return entries | 1;
    }
    u32 count = entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 ||
           count % 13 == 0 || count % 17 == 0) {
        ++count;
    }
    return count;
}

// A RomFS with `files` in its root (names without the leading '/').
Bytes BuildRomfs(const std::vector<File>& files) {
    constexpr u64 kDataStart = 0x200;
    constexpr u32 kNone = 0xFFFFFFFFu;

    std::vector<u64> data_offsets;
    u64 data_size = 0;
    for (const File& f : files) {
        data_size = (data_size + 0xF) & ~u64{0xF};
        data_offsets.push_back(data_size);
        data_size += f.data.size();
    }

    std::vector<u32> entry_offsets;
    u32 file_table_size = 0;
    for (const File& f : files) {
        entry_offsets.push_back(file_table_size);
        file_table_size += 0x20 + ((static_cast<u32>(f.name.size()) + 3) & ~3u);
    }
    Bytes file_table(file_table_size, 0);
    const u32 file_buckets = RomfsBuckets(static_cast<u32>(files.size()));
    std::vector<u32> file_hashes(file_buckets, kNone);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t e = entry_offsets[i];
        const u32 bucket = RomfsPathHash(0, files[i].name) % file_buckets;
        Put<u32>(file_table, e + 0x00, 0); // parent: the root
        Put<u32>(file_table, e + 0x04, i + 1 < files.size() ? entry_offsets[i + 1] : kNone);
        Put<u64>(file_table, e + 0x08, data_offsets[i]);
        Put<u64>(file_table, e + 0x10, static_cast<u64>(files[i].data.size()));
        Put<u32>(file_table, e + 0x18, file_hashes[bucket]);
        Put<u32>(file_table, e + 0x1C, static_cast<u32>(files[i].name.size()));
        std::memcpy(file_table.data() + e + 0x20, files[i].name.data(), files[i].name.size());
        file_hashes[bucket] = entry_offsets[i];
    }

    const u32 dir_buckets = RomfsBuckets(1);
    std::vector<u32> dir_hashes(dir_buckets, kNone);
    Bytes dir_table(0x18, 0);
    const u32 root_bucket = RomfsPathHash(0, {}) % dir_buckets;
    Put<u32>(dir_table, 0x00, 0);     // parent: itself
    Put<u32>(dir_table, 0x04, kNone); // sibling
    Put<u32>(dir_table, 0x08, kNone); // first child directory
    Put<u32>(dir_table, 0x0C, files.empty() ? kNone : 0);
    Put<u32>(dir_table, 0x10, dir_hashes[root_bucket]);
    Put<u32>(dir_table, 0x14, 0);     // name length
    dir_hashes[root_bucket] = 0;

    const u64 dir_hash_off = (kDataStart + data_size + 3) & ~u64{3};
    const u64 dir_table_off = dir_hash_off + u64{dir_buckets} * 4;
    const u64 file_hash_off = dir_table_off + dir_table.size();
    const u64 file_table_off = file_hash_off + u64{file_buckets} * 4;
    Bytes out(file_table_off + file_table.size(), 0);
    const u64 header[10] = {0x50,
                            dir_hash_off,
                            u64{dir_buckets} * 4,
                            dir_table_off,
                            static_cast<u64>(dir_table.size()),
                            file_hash_off,
                            u64{file_buckets} * 4,
                            file_table_off,
                            static_cast<u64>(file_table.size()),
                            kDataStart};
    std::memcpy(out.data(), header, sizeof(header));
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::memcpy(out.data() + kDataStart + data_offsets[i], files[i].data.data(), files[i].data.size());
    }
    std::memcpy(out.data() + dir_hash_off, dir_hashes.data(), dir_hashes.size() * 4);
    std::memcpy(out.data() + dir_table_off, dir_table.data(), dir_table.size());
    std::memcpy(out.data() + file_hash_off, file_hashes.data(), file_hashes.size() * 4);
    std::memcpy(out.data() + file_table_off, file_table.data(), file_table.size());
    return out;
}

// ---- NCAs ---------------------------------------------------------------------------------------

constexpr std::size_t kNcaHeaderSize = 0xC00;
constexpr std::size_t kIvfcBlock = 0x4000;

// NCA header content types (not ncm's NcmContentType).
constexpr u8 kNcaProgram = 0;
constexpr u8 kNcaMeta = 1;
constexpr u8 kNcaControl = 2;

std::size_t FsHeaderAt(int index) {
    return 0x400 + static_cast<std::size_t>(index) * 0x200;
}

// Closes section `index`, which started at `start`: its place in the section table and the
// hash of its FS header (so it goes last).
void FinishSection(Bytes& nca, int index, std::size_t start) {
    PadTo(nca, 0x200);
    const std::size_t entry = 0x240 + static_cast<std::size_t>(index) * 0x10;
    Put<u32>(nca, entry + 0x0, static_cast<u32>(start / 0x200));
    Put<u32>(nca, entry + 0x4, static_cast<u32>(nca.size() / 0x200));
    nca[entry + 0x8] = 1;
    const Hash hash = Sha256(nca.data() + FsHeaderAt(index), 0x200);
    std::memcpy(nca.data() + 0x280 + static_cast<std::size_t>(index) * 0x20, hash.data(), hash.size());
}

// A PFS0 section, its data hashed in `block`-sized pieces (HierarchicalSha256).
void AddPfs0Section(Bytes& nca, int index, const std::vector<File>& files, u32 block) {
    const Bytes pfs0 = BuildPfs0(files);
    Bytes table;
    for (std::size_t off = 0; off < pfs0.size(); off += block) {
        const Hash h = Sha256(pfs0.data() + off, std::min<std::size_t>(block, pfs0.size() - off));
        Append(table, h.data(), h.size());
    }
    const std::size_t start = nca.size();
    Append(nca, table.data(), table.size());
    PadTo(nca, 0x200);
    const u64 pfs0_offset = nca.size() - start;
    Append(nca, pfs0.data(), pfs0.size());

    const std::size_t fs = FsHeaderAt(index);
    Put<u16>(nca, fs + 0x0, 2); // version
    nca[fs + 0x2] = 1;          // PFS0
    nca[fs + 0x3] = 2;          // HierarchicalSha256
    nca[fs + 0x4] = 1;          // not encrypted
    const std::size_t h = fs + 0x8;
    const Hash master = Sha256(table.data(), table.size());
    std::memcpy(nca.data() + h, master.data(), master.size());
    Put<u32>(nca, h + 0x20, block);
    Put<u32>(nca, h + 0x24, 2); // the hash table and the data
    Put<u64>(nca, h + 0x28, 0);
    Put<u64>(nca, h + 0x30, static_cast<u64>(table.size()));
    Put<u64>(nca, h + 0x38, pfs0_offset);
    Put<u64>(nca, h + 0x40, static_cast<u64>(pfs0.size()));
    FinishSection(nca, index, start);
}

// A RomFS section under six levels of IVFC hashes.
void AddRomfsSection(Bytes& nca, int index, const std::vector<File>& files) {
    std::array<Bytes, 6> level;
    level[5] = BuildRomfs(files);
    const u64 romfs_size = level[5].size();
    PadTo(level[5], kIvfcBlock);
    for (int i = 4; i >= 0; --i) {
        const Bytes& below = level[static_cast<std::size_t>(i) + 1];
        Bytes& hashes = level[static_cast<std::size_t>(i)];
        for (std::size_t off = 0; off < below.size(); off += kIvfcBlock) {
            const Hash h = Sha256(below.data() + off, kIvfcBlock);
            Append(hashes, h.data(), h.size());
        }
        PadTo(hashes, kIvfcBlock);
    }

    const std::size_t start = nca.size();
    const std::size_t fs = FsHeaderAt(index);
    Put<u16>(nca, fs + 0x0, 2); // version
    nca[fs + 0x2] = 0;          // RomFS
    nca[fs + 0x3] = 3;          // HierarchicalIntegrity
    nca[fs + 0x4] = 1;          // not encrypted
    const std::size_t h = fs + 0x8;
    Put<u32>(nca, h + 0x0, 0x43465649u); // IVFC
    Put<u32>(nca, h + 0x4, 0x20000);
    Put<u32>(nca, h + 0x8, 0x20); // master hash size
    Put<u32>(nca, h + 0xC, 7);    // the six levels and the master hash
    u64 logical = 0;
    for (std::size_t i = 0; i < level.size(); ++i) {
        const std::size_t l = h + 0x10 + i * 0x18;
        Put<u64>(nca, l + 0x0, logical);
        Put<u64>(nca, l + 0x8, i == 5 ? romfs_size : static_cast<u64>(level[i].size()));
        Put<u32>(nca, l + 0x10, 14); // log2 of the 0x4000 block
        logical += level[i].size();
        Append(nca, level[i].data(), level[i].size());
    }
    const Hash master = Sha256(level[0].data(), level[0].size());
    std::memcpy(nca.data() + h + 0xC0, master.data(), master.size());
    FinishSection(nca, index, start);
}

// Fills in the header and encrypts it (AES-XTS, Nintendo's sector tweak, NCA3).
void SealNca(Bytes& nca, u8 content_type, u64 program_id, const u8* header_key) {
    Put<u32>(nca, 0x200, 0x3341434Eu); // NCA3
    nca[0x204] = 0;                    // distribution: download
    nca[0x205] = content_type;
    Put<u64>(nca, 0x208, static_cast<u64>(nca.size()));
    Put<u64>(nca, 0x210, program_id);
    Put<u32>(nca, 0x21C, 0x000C1100); // SDK version, as Sphaira's
    Aes128XtsContext ctx{};
    aes128XtsContextCreate(&ctx, header_key, header_key + 0x10, true);
    for (u64 sector = 0; sector < kNcaHeaderSize / 0x200; ++sector) {
        u8* p = nca.data() + sector * 0x200;
        aes128XtsContextResetSector(&ctx, sector, true);
        aes128XtsEncrypt(&ctx, p, p, 0x200);
    }
}

struct Nca {
    Bytes data;
    u8 type; // NcmContentType
    Hash hash;
};

Nca MakeNca(Bytes data, u8 type) {
    const Hash hash = Sha256(data.data(), data.size());
    return {std::move(data), type, hash};
}

NcmContentInfo InfoFor(const Nca& nca) {
    NcmContentInfo info{};
    std::memcpy(&info.content_id, nca.hash.data(), sizeof(info.content_id));
    ncmU64ToContentInfoSize(nca.data.size(), &info);
    info.content_type = nca.type;
    return info;
}

// ---- keys, NPDM, NACP, icon ---------------------------------------------------------------------

// The console's NCA header key, from its own key derivation (only the public key sources are
// here, as in Sphaira).
bool HeaderKey(u8 key[0x20]) {
    static constexpr u8 kHeaderKekSource[0x10] = {0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
                                                  0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
    static constexpr u8 kHeaderKeySource[0x20] = {
        0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
        0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};
    if (R_FAILED(splCryptoInitialize())) {
        return false;
    }
    u8 kek[0x10]{};
    const bool ok = R_SUCCEEDED(splCryptoGenerateAesKek(kHeaderKekSource, 0, 0, kek)) &&
                    R_SUCCEEDED(splCryptoGenerateAesKey(kek, kHeaderKeySource, key)) &&
                    R_SUCCEEDED(splCryptoGenerateAesKey(kek, kHeaderKeySource + 0x10, key + 0x10));
    splCryptoExit();
    return ok;
}

bool AtmosphereAtLeast(u32 major, u32 minor, u32 micro) {
    if (R_FAILED(splInitialize())) {
        return false;
    }
    u64 version = 0;
    // ExosphereApiVersion: the version sits in the top 24 bits.
    const bool ok = R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &version));
    splExit();
    return ok && (version >> 40) >= MAKEHOSVERSION(major, minor, micro);
}

// The kernel capability for debug flags (the 16 low bits set, then a clear one) becomes just
// the ForceDebug bit of Atmosphère 1.8.0's layout for HOS 19.0.0, as Sphaira does.
void SetForceDebug(Bytes& npdm, std::size_t offset, std::size_t size) {
    constexpr u32 kPattern = (1u << 16) - 1;
    constexpr u32 kMask = (1u << 16) | kPattern;
    for (std::size_t i = 0; i + 4 <= size && offset + i + 4 <= npdm.size(); i += 4) {
        if ((Get<u32>(npdm, offset + i) & kMask) == kPattern) {
            Put<u32>(npdm, offset + i, (1u << 19) | kPattern);
            return;
        }
    }
}

// The loader's NPDM (from tools/forwarder_hbl/hbl.json), made out to `program_id`.
bool PatchNpdm(Bytes& npdm, u64 program_id) {
    if (npdm.size() < 0x80 || Get<u32>(npdm, 0) != 0x4154454Du /* META */) {
        return false;
    }
    const std::size_t aci0 = Get<u32>(npdm, 0x70);
    const std::size_t acid = Get<u32>(npdm, 0x78);
    if (aci0 + 0x40 > npdm.size() || acid + 0x240 > npdm.size()) {
        return false;
    }
    Put<u64>(npdm, aci0 + 0x10, program_id);
    Put<u64>(npdm, acid + 0x210, program_id);
    Put<u64>(npdm, acid + 0x218, program_id);
    if (AtmosphereAtLeast(1, 8, 0)) {
        SetForceDebug(npdm, aci0 + Get<u32>(npdm, aci0 + 0x30), Get<u32>(npdm, aci0 + 0x34));
        SetForceDebug(npdm, acid + Get<u32>(npdm, acid + 0x230), Get<u32>(npdm, acid + 0x234));
    }
    return true;
}

// EmuSwitch's own icon (a 256x256 JPEG) and NACP, from its NRO's assets.
bool ReadNroAssets(const std::string& nro, Bytes& icon, Bytes& nacp) {
    FILE* f = std::fopen(nro.c_str(), "rb");
    if (!f) {
        return false;
    }
    NroStart start{};
    NroHeader header{};
    NroAssetHeader assets{};
    bool ok = std::fread(&start, sizeof(start), 1, f) == 1 && std::fread(&header, sizeof(header), 1, f) == 1 &&
              header.magic == NROHEADER_MAGIC && std::fseek(f, static_cast<long>(header.size), SEEK_SET) == 0 &&
              std::fread(&assets, sizeof(assets), 1, f) == 1 && assets.magic == NROASSETHEADER_MAGIC;
    const auto section = [&](const NroAssetSection& s, Bytes& out) {
        if (s.size == 0 || s.size > (4u << 20)) {
            return false;
        }
        out.resize(s.size);
        return std::fseek(f, static_cast<long>(header.size + s.offset), SEEK_SET) == 0 &&
               std::fread(out.data(), 1, out.size(), f) == out.size();
    };
    ok = ok && section(assets.icon, icon) && section(assets.nacp, nacp) && nacp.size() == 0x4000;
    std::fclose(f);
    return ok;
}

// EmuSwitch's NACP, made into the forwarder's: its name, and settings for software that keeps
// no saves of its own (as Sphaira sets them). Offsets are the NACP's.
void PatchNacp(Bytes& nacp, const Request& request, u64 tid) {
    if (nacp.size() != 0x4000) {
        nacp.assign(0x4000, 0);
    }
    for (std::size_t lang = 0; lang < 16; ++lang) {
        char* name = reinterpret_cast<char*>(nacp.data() + lang * 0x300);
        if (!request.name.empty()) {
            std::memset(name, 0, 0x200);
            std::memcpy(name, request.name.data(), std::min<std::size_t>(request.name.size(), 0x1FF));
        }
        if (!request.author.empty()) {
            std::memset(name + 0x200, 0, 0x100);
            std::memcpy(name + 0x200, request.author.data(), std::min<std::size_t>(request.author.size(), 0xFF));
        }
    }
    nacp[0x3025] = 0;                  // no user picked at launch
    nacp[0x3026] = 0;                  // user switching allowed
    nacp[0x3027] = 1;                  // add-on content on demand
    Put<u32>(nacp, 0x302C, 1);         // its title and icon are the AmericanEnglish ones
    nacp[0x3034] = 0;                  // screenshots allowed
    nacp[0x3035] = 2;                  // video capture allowed
    nacp[0x3036] = 0;                  // nothing to lose on quitting
    nacp[0x3037] = 0;                  // play log on
    Put<u64>(nacp, 0x3038, tid);       // presence group
    Put<u64>(nacp, 0x3070, tid ^ 0x1000); // add-on content base
    Put<u64>(nacp, 0x3078, tid);       // save data owner
    for (std::size_t off = 0x3080; off < 0x30A0; off += 8) {
        Put<u64>(nacp, off, 0);        // no save data
    }
    std::memcpy(nacp.data() + 0x30A8, "emuswtch", 8); // error code category
    for (std::size_t off = 0x30B0; off < 0x30F0; off += 8) {
        Put<u64>(nacp, off, tid);      // local communication
    }
    nacp[0x30F0] = 2;                  // Nintendo logo
    nacp[0x30F1] = 0;
    Put<u64>(nacp, 0x30F8, tid);       // pseudo device ID seed
    for (std::size_t off = 0x3148; off < 0x3168; off += 8) {
        Put<u64>(nacp, off, 0);        // no save data, at most
    }
    nacp[0x3210] = 0;                  // play log queries
    nacp[0x3213] = 0;                  // no online membership needed
}

struct JpegError {
    jpeg_error_mgr pub;
    std::jmp_buf jump;
};

[[noreturn]] void JpegFail(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
}

void JpegQuiet(j_common_ptr) {}

// A baseline JPEG of the 256x256 icon, the format the HOME Menu reads.
Bytes EncodeIcon(const std::uint32_t* rgba) {
    constexpr int kSize = 256;
    jpeg_compress_struct cinfo{};
    JpegError error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = JpegFail;
    error.pub.output_message = JpegQuiet;
    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    if (setjmp(error.jump)) {
        // What libjpeg allocated for `out` is lost on this (never seen) path.
        jpeg_destroy_compress(&cinfo);
        return {};
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &out, &out_size);
    cinfo.image_width = kSize;
    cinfo.image_height = kSize;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 92, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = reinterpret_cast<JSAMPROW>(const_cast<std::uint32_t*>(rgba + cinfo.next_scanline * kSize));
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    Bytes jpeg(out, out + out_size);
    jpeg_destroy_compress(&cinfo);
    std::free(out);
    return jpeg;
}

std::string Quoted(const std::string& s) {
    return "\"" + s + "\"";
}

// ---- installing ---------------------------------------------------------------------------------

struct ContentStorageRecord {
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[7];
};
static_assert(sizeof(ContentStorageRecord) == 0x18);

std::string Failed(const char* what, Result rc) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s (error 0x%X)", what, static_cast<unsigned>(rc));
    return text;
}

std::string InstallContents(u64 tid, const std::vector<Nca>& ncas, const Bytes& meta_value,
                            const NcmContentMetaKey& key) {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        return Failed("Couldn't reach the HOME Menu's software list", rc);
    }
    // One made before for the same game goes first, with its contents: they'd have the same IDs
    // as the new ones.
    nsDeleteApplicationCompletely(tid);

    std::string error;
    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        nsExit();
        return Failed("Couldn't reach the SD card's software storage", rc);
    }
    NcmContentStorage storage{};
    rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        error = Failed("Couldn't open the SD card's software storage", rc);
    } else {
        for (const Nca& nca : ncas) {
            NcmContentId id{};
            std::memcpy(&id, nca.hash.data(), sizeof(id));
            NcmPlaceHolderId placeholder{};
            rc = ncmContentStorageGeneratePlaceHolderId(&storage, &placeholder);
            if (R_SUCCEEDED(rc)) {
                ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
                rc = ncmContentStorageCreatePlaceHolder(&storage, &id, &placeholder,
                                                        static_cast<s64>(nca.data.size()));
            }
            if (R_SUCCEEDED(rc)) {
                rc = ncmContentStorageWritePlaceHolder(&storage, &placeholder, 0, nca.data.data(),
                                                       nca.data.size());
            }
            if (R_SUCCEEDED(rc)) {
                ncmContentStorageDelete(&storage, &id);
                rc = ncmContentStorageRegister(&storage, &id, &placeholder);
            }
            if (R_FAILED(rc)) {
                ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
                error = Failed("Couldn't write the forwarder to the SD card", rc);
                break;
            }
        }
        ncmContentStorageClose(&storage);
    }
    if (error.empty()) {
        NcmContentMetaDatabase db{};
        rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
        if (R_SUCCEEDED(rc)) {
            rc = ncmContentMetaDatabaseSet(&db, &key, meta_value.data(), meta_value.size());
            if (R_SUCCEEDED(rc)) {
                rc = ncmContentMetaDatabaseCommit(&db);
            }
            ncmContentMetaDatabaseClose(&db);
        }
        if (R_FAILED(rc)) {
            error = Failed("Couldn't record the forwarder on the SD card", rc);
        }
    }
    ncmExit();

    if (error.empty()) {
        // IApplicationManagerInterface: PushApplicationRecord (16), then
        // InvalidateApplicationControlCache (404) so the HOME Menu reads the new title and icon.
        // On [3.0.0+] libnx doesn't keep one open (its own session getter returns an unopened
        // service there, which failed with 0xE401, invalid handle), so it's opened here.
        Service am{};
        rc = nsGetApplicationManagerInterface(&am);
        if (R_SUCCEEDED(rc)) {
            const ContentStorageRecord record{key, NcmStorageId_SdCard, {}};
            const struct {
                u8 last_modified_event; // installed
                u8 padding[7];
                u64 application_id;
            } in{3, {}, tid};
            rc = serviceDispatchIn(&am, 16, in, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                                   .buffers = {{&record, sizeof(record)}});
            if (R_SUCCEEDED(rc)) {
                serviceDispatchIn(&am, 404, tid);
            }
            serviceClose(&am);
        }
        if (R_FAILED(rc)) {
            error = Failed("Couldn't add the forwarder to the HOME Menu", rc);
        }
    }
    nsExit();
    return error;
}

} // namespace

std::string Install(const Request& request) {
    if (request.nro.empty()) {
        return "Couldn't tell where EmuSwitch.nro is. Open EmuSwitch from hbmenu and try again";
    }
    Bytes loader = ReadWholeFile("romfs:/forwarder/main");
    Bytes npdm = ReadWholeFile("romfs:/forwarder/main.npdm");
    if (loader.empty() || npdm.empty()) {
        return "This EmuSwitch build is missing its forwarder loader";
    }

    // Its title ID comes from what it opens, so the same game always gets the same one (and
    // adding it again replaces it). 05.. is the range forwarders use.
    const std::string what = request.nro + "\n" + request.rom;
    const Hash digest = Sha256(what.data(), what.size());
    u64 bits = 0;
    std::memcpy(&bits, digest.data(), sizeof(bits));
    const u64 tid = 0x0500000000000000ull | (bits & 0x00FFFFFFFFFFF000ull);

    if (!PatchNpdm(npdm, tid)) {
        return "The forwarder loader's NPDM couldn't be read";
    }
    u8 header_key[0x20]{};
    if (!HeaderKey(header_key)) {
        return "Couldn't get the console's NCA key (this needs Atmosphère)";
    }

    Bytes own_icon, nacp;
    ReadNroAssets(request.nro, own_icon, nacp);
    Bytes icon = request.icon_rgba ? EncodeIcon(request.icon_rgba) : own_icon;
    if (icon.empty()) {
        icon = own_icon;
    }
    if (icon.empty()) {
        return "Couldn't make the HOME Menu icon";
    }
    PatchNacp(nacp, request, tid);

    // The loader starts EmuSwitch with the game. After an emulator EmuSwitch handed the game to
    // quits, it goes back to EmuSwitch's library ("return"), and quitting that goes back to the
    // HOME Menu.
    const std::string nro_argv = Quoted(request.nro);
    std::vector<File> romfs{{"nextArgv", ToBytes(request.rom.empty() ? nro_argv : nro_argv + " " + Quoted(request.rom))},
                            {"nextNroPath", ToBytes(request.nro)}};
    if (!request.rom.empty()) {
        romfs.push_back({"returnArgv", ToBytes(nro_argv)});
        romfs.push_back({"returnNroPath", ToBytes(request.nro)});
    }

    std::vector<Nca> ncas;
    {
        Bytes nca(kNcaHeaderSize, 0);
        AddPfs0Section(nca, 0, {{"main", std::move(loader)}, {"main.npdm", std::move(npdm)}}, 0x10000);
        AddRomfsSection(nca, 1, romfs);
        SealNca(nca, kNcaProgram, tid, header_key);
        ncas.push_back(MakeNca(std::move(nca), NcmContentType_Program));
    }
    {
        Bytes nca(kNcaHeaderSize, 0);
        AddRomfsSection(nca, 0, {{"control.nacp", nacp}, {"icon_AmericanEnglish.dat", icon}});
        SealNca(nca, kNcaControl, tid, header_key);
        ncas.push_back(MakeNca(std::move(nca), NcmContentType_Control));
    }

    // The content meta: in the meta NCA as a .cnmt, and in ncm's database.
    NcmApplicationMetaExtendedHeader extended{};
    extended.patch_id = tid | 0x800;
    const NcmContentInfo program_info = InfoFor(ncas[0]);
    const NcmContentInfo control_info = InfoFor(ncas[1]);
    {
        Bytes cnmt(0x20, 0);
        Put<u64>(cnmt, 0x00, tid);
        cnmt[0x0C] = NcmContentMetaType_Application;
        Put<u16>(cnmt, 0x0E, sizeof(extended));
        Put<u16>(cnmt, 0x10, 2); // program and control
        Append(cnmt, &extended, sizeof(extended));
        for (const Nca* nca : {&ncas[0], &ncas[1]}) {
            const NcmContentInfo info = InfoFor(*nca);
            Append(cnmt, nca->hash.data(), nca->hash.size());
            Append(cnmt, &info, sizeof(info));
        }
        cnmt.resize(cnmt.size() + 0x20, 0); // digest
        char name[48];
        std::snprintf(name, sizeof(name), "Application_%016llx.cnmt", static_cast<unsigned long long>(tid));
        Bytes nca(kNcaHeaderSize, 0);
        AddPfs0Section(nca, 0, {{name, std::move(cnmt)}}, 0x1000);
        SealNca(nca, kNcaMeta, tid, header_key);
        ncas.push_back(MakeNca(std::move(nca), NcmContentType_Meta));
    }

    NcmContentMetaKey key{};
    key.id = tid;
    key.version = 0;
    key.type = NcmContentMetaType_Application;
    key.install_type = NcmContentInstallType_Full;

    NcmContentMetaHeader meta_header{};
    meta_header.extended_header_size = sizeof(extended);
    meta_header.content_count = 3;
    meta_header.content_meta_count = 0;
    Bytes meta_value;
    Append(meta_value, &meta_header, sizeof(meta_header));
    Append(meta_value, &extended, sizeof(extended));
    const NcmContentInfo meta_info = InfoFor(ncas[2]);
    for (const NcmContentInfo* info : {&meta_info, &program_info, &control_info}) {
        Append(meta_value, info, sizeof(*info));
    }

    return InstallContents(tid, ncas, meta_value, key);
}

} // namespace SwitchFrontend::Forwarder
