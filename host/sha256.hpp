#pragma once

// SHA-256 (FIPS 180-4), written from the standard. The page uses it to check that the executable it reads
// from the user's disc is the one the game's code was translated from (host/web.cpp).

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2web {

// The digest as 64 lower-case hex digits, the form CMake's file(SHA256) gives.
std::string sha256_hex(const std::uint8_t *data, std::size_t size);

}
