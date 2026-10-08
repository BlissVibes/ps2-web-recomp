#include "spu2.hpp"

#include "iop.hpp"

#include <cstring>
#include <vector>

namespace ps2::iop::spu2 {

namespace {

constexpr std::uint32_t kRamWords = 1u << 20;

std::vector<std::uint16_t> g_ram(kRamWords);
std::uint16_t g_regs[0x400];

struct Core {
    std::uint32_t tsa;
};
Core g_cores[2];

std::uint32_t reg_index(std::uint32_t a) { return (a & 0x7FFu) >> 1; }

int core_of(std::uint32_t a) {
    const std::uint32_t off = a & 0x7FFu;
    if (off < 0x400u) return 0;
    if (off < 0x760u) return 1;
    return off < 0x788u ? 0 : 1;
}

std::uint32_t local(std::uint32_t a) {
    const std::uint32_t off = a & 0x7FFu;
    return off < 0x760u ? (off & 0x3FFu) : off;
}

}

void reset() {
    std::fill(g_ram.begin(), g_ram.end(), 0);
    std::memset(g_regs, 0, sizeof g_regs);
    std::memset(g_cores, 0, sizeof g_cores);
}

std::uint16_t *ram() { return g_ram.data(); }

std::uint32_t read(std::uint32_t a, unsigned) {
    const std::uint32_t off = local(a);
    const int core = core_of(a);
    if (off == 0x344u) return 0x0080u;
    if (off == 0x1A8u) return g_cores[core].tsa >> 16;
    if (off == 0x1AAu) return g_cores[core].tsa & 0xFFFFu;
    return g_regs[reg_index(a)];
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    const std::uint32_t off = local(a);
    const int core = core_of(a);
    if (size == 4) {
        write(a, v & 0xFFFFu, 2);
        write(a + 2, v >> 16, 2);
        return;
    }
    v &= 0xFFFFu;
    g_regs[reg_index(a)] = static_cast<std::uint16_t>(v);
    Core &c = g_cores[core];
    if (off == 0x1A8u) c.tsa = ((v & 0xFu) << 16) | (c.tsa & 0xFFFFu);
    else if (off == 0x1AAu) c.tsa = (c.tsa & 0xF0000u) | v;
    else if (off == 0x1ACu) {
        g_ram[c.tsa & (kRamWords - 1)] = static_cast<std::uint16_t>(v);
        c.tsa = (c.tsa + 1) & (kRamWords - 1);
    }
}

void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu) {
    Core &c = g_cores[core];
    for (std::uint32_t i = 0; i < words * 2; ++i) {
        std::uint16_t *spu = &g_ram[(c.tsa + i) & (kRamWords - 1)];
        std::uint8_t *iop = ram_ptr(madr + i * 2);
        if (to_spu) std::memcpy(spu, iop, 2);
        else std::memcpy(iop, spu, 2);
    }
    c.tsa = (c.tsa + words * 2) & (kRamWords - 1);
}

}
