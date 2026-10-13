#pragma once

#include <cstdint>

// Positions in a reverb work area of `size` halfwords (2 to 0x100000). The reverb reads and writes at the
// area's moving origin plus an offset register, which holds any 32-bit value, or plus one register less
// another, or less 1; all of it wraps round the area. An offset is wrapped into the area once, with the one
// division; after that positions are added and subtracted with a compare.
namespace ps2::iop::spu2::work_area {

inline std::uint32_t wrap(std::uint32_t offset, std::uint32_t size) { return offset % size; }

// a + b and a - b round the area, for two positions in it (each below size).
inline std::uint32_t add(std::uint32_t a, std::uint32_t b, std::uint32_t size) {
    const std::uint32_t sum = a + b;
    return sum >= size ? sum - size : sum;
}
inline std::uint32_t sub(std::uint32_t a, std::uint32_t b, std::uint32_t size) { return a >= b ? a - b : a + size - b; }

}
