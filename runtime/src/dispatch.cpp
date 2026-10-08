#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace ps2 {

namespace {

std::vector<UnitInfo> g_units;
std::vector<const UnitInfo *> g_index;
std::uint32_t g_origin = 0;
std::uint32_t g_unit_size = 1;
std::set<std::uint32_t> g_misses;

const UnitInfo *unit_for(std::uint32_t pc) {
    const std::uint32_t slot = (pc - g_origin) / g_unit_size;
    if (pc < g_origin || slot >= g_index.size()) return nullptr;
    return g_index[slot];
}

void record_miss(std::uint32_t pc) {
    if (!g_misses.insert(pc).second || g_options.misses_path.empty()) return;
    std::ofstream out(g_options.misses_path, std::ios::app);
    out << std::hex << pc << "\n";
}

}

void register_units(const UnitInfo *units, std::uint32_t count) {
    g_units.assign(units, units + count);
    if (g_units.empty()) return;
    std::sort(g_units.begin(), g_units.end(), [](const UnitInfo &a, const UnitInfo &b) { return a.base < b.base; });
    g_origin = g_units.front().base;
    g_unit_size = g_units.front().size;
    g_index.assign((g_units.back().base - g_origin) / g_unit_size + 1, nullptr);
    for (const UnitInfo &u : g_units) g_index[(u.base - g_origin) / g_unit_size] = &u;
}

bool has_code(std::uint32_t pc) {
    const UnitInfo *u = unit_for(pc);
    return u && u->entries[(pc - u->base) >> 2] != 0;
}

void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {
    const std::uint32_t pc = c.pc;
    if (pc & kUnwind) return;
    if (pc == kHandlerReturn || pc == kIdle) return;
    const UnitInfo *u = unit_for(pc);
    const std::uint16_t id = u ? u->entries[(pc - u->base) >> 2] : 0;
    if (!id) {
        record_miss(pc);
        char reason[64];
        std::snprintf(reason, sizeof reason, "missing code at 0x%08X (ra 0x%08X)", pc, c.r[31].w[0]);
        std::fprintf(stderr, "[ee] no translated code at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
        halt(reason);
        c.pc = pc | kUnwind;
        return;
    }
    u->function(c, id, depth);
}

void call(Cpu &c, std::uint32_t target, std::uint32_t depth) {
    c.pc = target;
    if (depth >= kMaxDepth) return;
    dispatch(c, 0, depth);
}

void bad_entry(Cpu &c, std::uint32_t unit_base) {
    std::fprintf(stderr, "[ee] bad entry into unit 0x%08X for pc 0x%08X\n", unit_base, c.pc);
    halt("bad entry");
    c.pc |= kUnwind;
}

void stop(Cpu &c, std::uint32_t pc, const char *what) {
    std::fprintf(stderr, "[ee] stopped at 0x%08X: %s\n", pc, what);
    halt(what);
    c.pc = pc | kUnwind;
}

void probe(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X ra %08X v0 %08X\n", pc, c.r[4].w[0], c.r[5].w[0], c.r[6].w[0],
                 c.r[7].w[0], c.r[31].w[0], c.r[2].w[0]);
}

void breakpoint(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] break at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

void trap(Cpu &c, std::uint32_t pc) {
    std::fprintf(stderr, "[ee] trap at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
}

}
