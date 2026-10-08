#include "hw.hpp"

#include "kernel.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ps2::hw {

namespace {

constexpr std::uint32_t kIntcStat = 0x1000F000u, kIntcMask = 0x1000F010u;
constexpr std::uint32_t kDmacStat = 0x1000E010u;
constexpr std::uint32_t kGsCsr = 0x12001000u, kGsImr = 0x12001010u;

std::uint32_t g_intc_stat = 0, g_intc_mask = 0;
std::uint32_t g_dmac_stat = 0;
std::uint64_t g_gs_csr = 0x551B0000ull, g_gs_imr = 0x7F00ull;
std::uint32_t g_sif[8] = {};
std::map<std::uint32_t, std::uint64_t> g_regs;
std::set<std::uint32_t> g_reported;
std::uint32_t g_crt[3] = {};

void report(const char *what, std::uint32_t a, std::uint64_t v) {
    if (g_options.trace_io || g_reported.insert(a).second)
        std::fprintf(stderr, "[io] %s 0x%08X = 0x%llX (pc 0x%08X)\n", what, a, static_cast<unsigned long long>(v), g_cpu.pc);
}

std::uint32_t timer_count(std::uint32_t base) {
    const std::uint32_t mode = static_cast<std::uint32_t>(g_regs[base + 0x10]);
    const std::uint64_t bus = now() / 2;
    std::uint64_t ticks;
    switch (mode & 3u) {
    case 0: ticks = bus; break;
    case 1: ticks = bus / 16; break;
    case 2: ticks = bus / 256; break;
    default: ticks = now() / 18874u; break;
    }
    return static_cast<std::uint32_t>((ticks - g_regs[base + 0x40]) & 0xFFFFu);
}

bool is_timer(std::uint32_t a) { return a < 0x10002000u && (a & 0x7FFu) < 0x40u; }

}

void reset() {
    g_intc_stat = g_intc_mask = g_dmac_stat = 0;
    g_gs_csr = 0x551B0000ull;
    g_gs_imr = 0x7F00ull;
    std::memset(g_sif, 0, sizeof g_sif);
    g_regs.clear();
    g_regs[0x10001810u] = 0x183u;
}

void vblank_start() {
    g_gs_csr ^= 0x2000u;
    g_gs_csr |= 0x8u;
    intc_raise(2);
    if (!(g_gs_imr & 0x800u)) intc_raise(0);
    schedule(kEventVblankEnd, now() + 18874u * 22u, [] { intc_raise(3); });
}

std::uint32_t intc_pending() { return g_intc_stat & g_intc_mask; }

void intc_raise(unsigned cause) {
    g_intc_stat |= 1u << cause;
    if (g_intc_mask & (1u << cause)) kernel::wake_idle();
}

void intc_ack(unsigned cause) { g_intc_stat &= ~(1u << cause); }

bool intc_set_mask(unsigned cause, bool enabled) {
    const std::uint32_t bit = 1u << cause;
    const bool was = g_intc_mask & bit;
    if (enabled) g_intc_mask |= bit;
    else g_intc_mask &= ~bit;
    return was != enabled;
}

std::uint32_t dmac_pending() {
    const std::uint32_t cis = g_dmac_stat & 0x3FFu, cim = (g_dmac_stat >> 16) & 0x3FFu;
    return cis & cim;
}

void dmac_ack(unsigned channel) { g_dmac_stat &= ~(1u << channel); }

bool dmac_set_mask(unsigned channel, bool enabled) {
    const std::uint32_t bit = 0x10000u << channel;
    const bool was = g_dmac_stat & bit;
    if (enabled) g_dmac_stat |= bit;
    else g_dmac_stat &= ~bit;
    return was != enabled;
}

std::uint64_t gs_imr() { return g_gs_imr; }
void gs_set_imr(std::uint64_t value) { g_gs_imr = value; }

void gs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    g_crt[0] = interlace;
    g_crt[1] = mode;
    g_crt[2] = field;
    std::fprintf(stderr, "[gs] SetGsCrt interlace %u mode 0x%X field %u\n", interlace, mode, field);
}

std::uint32_t sif_get(std::uint32_t reg) {
    if (reg & 0x80000000u) return g_sif[7];
    return g_sif[reg & 7u];
}

void sif_set(std::uint32_t reg, std::uint32_t value) {
    if (reg & 0x80000000u) g_sif[7] = value;
    else g_sif[reg & 7u] = value;
}

std::uint32_t read32(std::uint32_t a) {
    if (a == kIntcStat) return g_intc_stat;
    if (a == kIntcMask) return g_intc_mask;
    if (a == kDmacStat) return g_dmac_stat;
    if (a == kGsCsr) return static_cast<std::uint32_t>(g_gs_csr);
    if (a == kGsCsr + 4) return static_cast<std::uint32_t>(g_gs_csr >> 32);
    if (a == kGsImr) return static_cast<std::uint32_t>(g_gs_imr);
    if (is_timer(a) && (a & 0x3Fu) == 0) return timer_count(a);
    if (a >= 0x1000F200u && a < 0x1000F280u) return g_sif[(a >> 4) & 7u];
    if (a == 0x1000F130u) return 0;
    if (a == 0x1000F520u) return static_cast<std::uint32_t>(g_regs[0x1000F590u]);
    auto it = g_regs.find(a);
    const std::uint64_t v = it == g_regs.end() ? 0 : it->second;
    report("read", a, v);
    return static_cast<std::uint32_t>(v);
}

void write32(std::uint32_t a, std::uint32_t v) {
    if (a == kIntcStat) { g_intc_stat &= ~v; return; }
    if (a == kIntcMask) { g_intc_mask ^= v; return; }
    if (a == kDmacStat) {
        g_dmac_stat &= ~(v & 0xFFFFu);
        g_dmac_stat ^= v & 0xFFFF0000u;
        return;
    }
    if (a == kGsCsr) {
        g_gs_csr &= ~static_cast<std::uint64_t>(v & 0x1Fu);
        if (v & 0x200u) g_gs_csr = 0x551B0000ull | (g_gs_csr & 0x2000u);
        return;
    }
    if (a == kGsImr) { g_gs_imr = v; return; }
    if (is_timer(a) && (a & 0x3Fu) == 0) {
        g_regs[a + 0x40] = 0;
        g_regs[a + 0x40] = static_cast<std::uint64_t>(timer_count(a)) - v;
        return;
    }
    if (a >= 0x1000F200u && a < 0x1000F280u) { g_sif[(a >> 4) & 7u] = v; return; }
    report("write", a, v);
    g_regs[a] = v;
}

std::uint64_t read64(std::uint32_t a) {
    if (a == kGsCsr) return g_gs_csr;
    if (a == kGsImr) return g_gs_imr;
    return read32(a) | (static_cast<std::uint64_t>(read32(a + 4)) << 32);
}

void write64(std::uint32_t a, std::uint64_t v) {
    if (a == kGsCsr || a == kGsImr) { write32(a, static_cast<std::uint32_t>(v)); return; }
    if (a >= 0x12000000u && a < 0x12002000u) {
        report("write64", a, v);
        g_regs[a] = v;
        return;
    }
    write32(a, static_cast<std::uint32_t>(v));
    write32(a + 4, static_cast<std::uint32_t>(v >> 32));
}

void read128(std::uint32_t a, Reg128 &out) {
    report("read128", a, 0);
    std::memset(&out, 0, sizeof out);
}

void write128(std::uint32_t a, const Reg128 &v) {
    report("write128", a, v.d[0]);
}

}
