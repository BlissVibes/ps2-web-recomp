#pragma once

// The page's cache of disc chunks. In the browser the disc is a file that only the main thread can read,
// asynchronously; the emulation thread asks for 256 KB chunks, waits for the one it is reading and asks
// ahead while the game reads forwards. This is the bookkeeping alone: which slot holds which chunk and
// which slot is given up for a new one. The host fetches the data and wakes the waiter (host/web.cpp),
// so this part can be tested without a browser.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ps2web {

constexpr std::size_t kChunk = 256u << 10;
constexpr std::uint32_t kSlots = 256;
constexpr std::uint64_t kReadAhead = 16;

// A slot goes kEmpty -> kWanted (asked for here) -> kLoading (the host has started the fetch) -> kReady or
// kFailed (the fetch has ended). Only a ready or failed slot can be given to another chunk.
enum : std::uint32_t { kEmpty, kWanted, kLoading, kReady, kFailed };

struct Slot {
    std::atomic<std::uint32_t> state{kEmpty};
    std::uint64_t index = 0, used = 0;
    std::uint32_t size = 0;
    std::uint8_t *data = nullptr;
};

struct DiscCache {
    Slot slots[kSlots];
    std::uint64_t disc_size = 0, use_clock = 0, last_chunk = ~0ull, run = 0;
    bool asked = false;   // a slot has gone to kWanted since the host last cleared this: there is something to fetch

    Slot *find(std::uint64_t index);
    // The slot of a chunk, asked for if the cache does not have it. Null past the end of the disc and when
    // no slot can be given up (every one is wanted or loading, or is `keep`). The slot given up is the
    // ready or failed one used longest ago, never `keep`.
    Slot *want(std::uint64_t index, const Slot *keep = nullptr);
    // The chunk the game reads now: its slot as want() gives it, and the chunks after it are asked for
    // while the game reads forwards (two more for every chunk of the run, up to kReadAhead). Asking ahead
    // never takes the slot of the chunk being read.
    Slot *read(std::uint64_t index);
};

}
