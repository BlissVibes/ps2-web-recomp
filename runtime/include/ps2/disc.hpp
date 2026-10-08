#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

// Reads up to size bytes at a byte offset of the image and returns how many it got.
using Reader = std::function<std::size_t(std::uint64_t offset, void *out, std::size_t size)>;

bool open(const std::string &path);
bool open(const std::string &name, std::uint64_t size, Reader reader);
bool is_open();
bool read(std::uint32_t sector, std::uint32_t count, void *out);
std::optional<Entry> find(const std::string &path);
std::uint32_t layer1_start();
std::uint64_t sector_count();

}
