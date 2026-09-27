#pragma once

#include <string>
#include <vector>
#include <switch.h>

struct ZipEntry {
    std::string name;
    u16 method = 0;
    u32 compSize = 0;
    u32 size = 0;
    u32 localOffset = 0;
};

std::vector<ZipEntry> zipList(const std::string& path);
bool zipExtract(const std::string& path, const ZipEntry& entry, const std::string& dest);
