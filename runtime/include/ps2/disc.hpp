#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps2::disc {

inline constexpr std::uint32_t kSector = 2048;

struct Entry {
    std::uint32_t sector;
    std::uint32_t size;
    bool directory;
};

bool open(const std::string &path);
bool is_open();
bool read(std::uint32_t sector, std::uint32_t count, void *out);
std::optional<Entry> find(const std::string &path);
std::uint32_t layer1_start();
std::uint64_t sector_count();

}
