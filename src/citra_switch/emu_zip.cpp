// EmuSwitch - just enough zip reading to unpack BIOS archives: the central
// directory, stored and deflated entries, no zip64 or encryption.

#include "citra_switch/emu_zip.h"

#include <cstdio>
#include <cstring>
#include <zlib.h>

namespace {

u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }

bool readAt(FILE* f, long off, void* buf, size_t len) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

}  // namespace

std::vector<ZipEntry> zipList(const std::string& path) {
    std::vector<ZipEntry> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    // The end-of-central-directory record sits in the last 64 KB + 22 bytes.
    long tail = size < 65557 ? size : 65557;
    std::vector<u8> buf(tail);
    if (!readAt(f, size - tail, buf.data(), tail)) { fclose(f); return out; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(&buf[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { fclose(f); return out; }
    u16 count = rd16(&buf[eocd + 10]);
    u32 cdSize = rd32(&buf[eocd + 12]);
    u32 cdOff = rd32(&buf[eocd + 16]);
    std::vector<u8> cd(cdSize);
    if (!readAt(f, cdOff, cd.data(), cdSize)) { fclose(f); return out; }
    size_t p = 0;
    for (u16 i = 0; i < count && p + 46 <= cd.size(); i++) {
        if (rd32(&cd[p]) != 0x02014b50) break;
        ZipEntry e;
        e.method = rd16(&cd[p + 10]);
        e.compSize = rd32(&cd[p + 20]);
        e.size = rd32(&cd[p + 24]);
        u16 nameLen = rd16(&cd[p + 28]), extraLen = rd16(&cd[p + 30]), commentLen = rd16(&cd[p + 32]);
        e.localOffset = rd32(&cd[p + 42]);
        e.name.assign((const char*)&cd[p + 46], nameLen);
        out.push_back(e);
        p += 46 + nameLen + extraLen + commentLen;
    }
    fclose(f);
    return out;
}

bool zipExtract(const std::string& path, const ZipEntry& e, const std::string& dest) {
    if (e.method != 0 && e.method != 8) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u8 local[30];
    if (!readAt(f, e.localOffset, local, 30) || rd32(local) != 0x04034b50) { fclose(f); return false; }
    long data = e.localOffset + 30 + rd16(&local[26]) + rd16(&local[28]);
    std::vector<u8> comp(e.compSize);
    if (!readAt(f, data, comp.data(), comp.size())) { fclose(f); return false; }
    fclose(f);

    std::vector<u8> raw;
    if (e.method == 0) {
        raw.swap(comp);
    } else {
        raw.resize(e.size);
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in = comp.data();
        zs.avail_in = comp.size();
        zs.next_out = raw.data();
        zs.avail_out = raw.size();
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END || zs.total_out != e.size) return false;
    }

    std::string tmp = dest + ".part";
    FILE* o = fopen(tmp.c_str(), "wb");
    if (!o) return false;
    bool ok = fwrite(raw.data(), 1, raw.size(), o) == raw.size();
    fclose(o);
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(dest.c_str());
    return rename(tmp.c_str(), dest.c_str()) == 0;
}
