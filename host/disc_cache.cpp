#include "disc_cache.hpp"

#include <algorithm>

namespace ps2web {

Slot *DiscCache::find(std::uint64_t index) {
    for (Slot &s : slots)
        if (s.state.load(std::memory_order_acquire) != kEmpty && s.index == index) return &s;
    return nullptr;
}

Slot *DiscCache::want(std::uint64_t index, const Slot *keep) {
    if (index * kChunk >= disc_size) return nullptr;
    if (Slot *s = find(index)) {
        s->used = ++use_clock;   // found counts as used: otherwise the next chunk asked for could take this slot
        return s;
    }
    Slot *victim = nullptr;
    for (Slot &s : slots) {
        const std::uint32_t state = s.state.load(std::memory_order_acquire);
        if (state == kEmpty) {
            victim = &s;
            break;
        }
        if (&s == keep) continue;
        if ((state == kReady || state == kFailed) && (!victim || s.used < victim->used)) victim = &s;
    }
    if (!victim) return nullptr;
    if (!victim->data) victim->data = new std::uint8_t[kChunk];
    victim->index = index;
    victim->size = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, disc_size - index * kChunk));
    victim->used = ++use_clock;
    victim->state.store(kWanted, std::memory_order_release);
    asked = true;
    return victim;
}

Slot *DiscCache::read(std::uint64_t index) {
    Slot *s = want(index);
    if (index == last_chunk + 1) ++run;
    else if (index != last_chunk) run = 0;
    last_chunk = index;
    for (std::uint64_t k = 1; k <= std::min(run * 2, kReadAhead); ++k) want(index + k, s);
    return s;
}

}
