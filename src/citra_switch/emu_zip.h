#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t compSize = 0;
    std::uint32_t size = 0;
    std::uint32_t localOffset = 0;
};

std::vector<ZipEntry> zipList(const std::string& path);
bool zipExtract(const std::string& path, const ZipEntry& entry, const std::string& dest);
