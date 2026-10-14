// Replays a route's disc reads against the page's disc chunk cache (host/disc_cache.cpp), to choose its
// number of slots without a browser. The reads come from the native program:
//
//     ps2run <elf> --disc <iso> --disc-log reads.log [--press ...] --frames N
//
// writes one line "frame offset bytes" per read. The page's read_disc (host/web.cpp) asks the cache for
// every 256 KB chunk a read covers, in order; this does the same. Every fetch is taken to arrive at once,
// so a chunk asked for ahead is always there when it is read: what is counted is the reads that find their
// chunk missing, each of which holds the emulation thread in the page for a round trip to the main thread.
//
// Build once per slot count:
//     c++ -std=c++20 -O2 -I host -DPS2WEB_DISC_SLOTS=64 scripts/disc_replay.cpp host/disc_cache.cpp -o disc_replay64
// usage: disc_replay64 <reads.log> <iso>   (DISC_REPLAY_MISSES=1 lists every miss with its frame)

#include "disc_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

using ps2web::DiscCache, ps2web::Slot, ps2web::kChunk, ps2web::kSlots;

struct Phase {
    const char *name;
    std::uint64_t from;   // first frame
    std::uint64_t reads = 0, misses = 0, again = 0;
};

}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: disc_replay <reads.log> <iso>\n");
        return 2;
    }
    std::FILE *log = std::fopen(argv[1], "r");
    if (!log) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    const bool list_misses = std::getenv("DISC_REPLAY_MISSES") != nullptr;   // every miss, with its frame
    auto cache = std::make_unique<DiscCache>();
    cache->disc_size = std::filesystem::file_size(argv[2]);
    // The route's phases (docs/re/menu-route.md in the Burnout 3 repo).
    Phase phases[] = {{"boot and menus", 0}, {"training video", 4400}, {"race loading", 11925}, {"race", 12575}};
    std::unordered_map<std::uint64_t, bool> seen;   // chunks read before
    // How many other chunks were read between two reads of the same chunk (the LRU stack distance): a cache
    // of N slots that kept the chunks read last would miss on every read again at a distance of N or more.
    std::list<std::uint64_t> stack;
    std::map<std::uint64_t, std::uint64_t> distances;
    std::uint64_t lines = 0, reads = 0, misses = 0, again = 0, fetched = 0, wrong = 0, no_slot = 0;
    unsigned long long frame, offset, bytes;
    while (std::fscanf(log, "%llu %llu %llu", &frame, &offset, &bytes) == 3) {
        ++lines;
        if (!bytes) continue;
        Phase *phase = &phases[0];
        for (Phase &p : phases)
            if (frame >= p.from) phase = &p;
        for (std::uint64_t index = offset / kChunk; index <= (offset + bytes - 1) / kChunk && index * kChunk < cache->disc_size; ++index) {
            ++reads;
            ++phase->reads;
            const Slot *before = cache->find(index);
            const bool miss = !before || before->state.load() != ps2web::kReady;
            if (miss) {
                if (list_misses) std::printf("  miss: frame %llu chunk %llu%s\n", frame, static_cast<unsigned long long>(index), seen.count(index) ? " (read before)" : "");
                ++misses;
                ++phase->misses;
                if (seen.count(index)) {
                    ++again;
                    ++phase->again;
                }
            }
            seen[index] = true;
            std::uint64_t distance = 0;
            bool found = false;
            for (auto it = stack.begin(); it != stack.end(); ++it, ++distance)
                if (*it == index) {
                    stack.erase(it);
                    found = true;
                    break;
                }
            if (found) ++distances[distance];
            stack.push_front(index);
            Slot *s = cache->read(index);
            if (!s) ++no_slot;
            else if (s->index != index) ++wrong;
            for (Slot &w : cache->slots)   // every fetch asked for arrives
                if (w.state.load() == ps2web::kWanted) {
                    w.state.store(ps2web::kReady);
                    ++fetched;
                }
            cache->asked = false;
        }
    }
    std::printf("slots %u (%u MB): %llu reads in the log, %llu chunk reads, %zu distinct chunks (%.1f MB)\n", kSlots,
                static_cast<unsigned>(kSlots * kChunk >> 20), static_cast<unsigned long long>(lines), static_cast<unsigned long long>(reads), seen.size(),
                seen.size() * kChunk / 1048576.0);
    std::printf("  misses %llu (of them chunks read before: %llu), chunks fetched %llu (%.1f MB), no slot %llu, wrong chunk %llu\n",
                static_cast<unsigned long long>(misses), static_cast<unsigned long long>(again), static_cast<unsigned long long>(fetched),
                fetched * kChunk / 1048576.0, static_cast<unsigned long long>(no_slot), static_cast<unsigned long long>(wrong));
    for (const Phase &p : phases)
        std::printf("  %-15s from frame %5llu: %7llu chunk reads, %5llu misses, %5llu of chunks read before\n", p.name, static_cast<unsigned long long>(p.from),
                    static_cast<unsigned long long>(p.reads), static_cast<unsigned long long>(p.misses), static_cast<unsigned long long>(p.again));
    // Reads again of a chunk, by how many other chunks were read in between.
    const std::uint64_t edges[] = {0, 1, 16, 32, 48, 64, 96, 128, 192, 256, 512, 1024, ~0ull};
    std::printf("  reads again of a chunk, by other chunks read in between:");
    for (std::size_t e = 0; e + 1 < std::size(edges); ++e) {
        std::uint64_t n = 0;
        for (const auto &[d, count] : distances)
            if (d >= edges[e] && d < edges[e + 1]) n += count;
        if (edges[e + 1] == ~0ull) std::printf(" %llu+: %llu", static_cast<unsigned long long>(edges[e]), static_cast<unsigned long long>(n));
        else std::printf(" %llu-%llu: %llu", static_cast<unsigned long long>(edges[e]), static_cast<unsigned long long>(edges[e + 1] - 1), static_cast<unsigned long long>(n));
    }
    std::printf("\n");
    return wrong || no_slot ? 1 : 0;
}
