#include "dmac.hpp"

#include "kernel.hpp"

#include "gs/gif.hpp"
#include "ipu/ipu.hpp"
#include "hw.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "vif.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::dmac {

namespace {

struct Channel {
    std::uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr[2] = {}, sadr = 0;
    bool paused = false, pause_end = false, tag_end = false;
};

Channel g_ch[10];
std::uint32_t g_ctrl = 0, g_pcr = 0, g_sqwc = 0, g_rbsr = 0, g_rbor = 0, g_stadr = 0, g_enable = 0;

constexpr std::uint32_t kBase[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                     0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};

std::uint64_t g_starts[10];

const std::uint8_t *source(std::uint32_t address) {
    if (address & 0x80000000u) return g_spr + (address & (kScratchpadSize - 1));
    return g_ram + (address & (kRamSize - 16));
}

std::uint8_t *target(std::uint32_t address) {
    if (address & 0x80000000u) return g_spr + (address & (kScratchpadSize - 1));
    return g_ram + (address & (kRamSize - 16));
}

void deliver(int ch, std::uint32_t address, std::uint32_t qwc) {
    if (!qwc) return;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(qwc) * 16);
    for (std::uint32_t i = 0; i < qwc; ++i) std::memcpy(buffer.data() + i * 16, source(address + i * 16), 16);
    switch (ch) {
    case 0: vif::transfer(0, buffer.data(), qwc); break;
    case 1: vif::transfer(1, buffer.data(), qwc); break;
    case 2: gif::transfer(3, buffer.data(), qwc); break;
    case 9:
        for (std::uint32_t i = 0; i < qwc; ++i)
            std::memcpy(g_spr + ((g_ch[9].sadr + i * 16) & (kScratchpadSize - 1)), buffer.data() + i * 16, 16);
        g_ch[9].sadr = (g_ch[9].sadr + qwc * 16) & (kScratchpadSize - 1);
        break;
    default:
        std::fprintf(stderr, "[dma] channel %d data (%u qwords) not handled\n", ch, qwc);
        break;
    }
}

bool vif_stalled(int ch) { return (ch == 0 || ch == 1) && vif::stalled(ch); }

void pause(int ch, bool end) {
    g_ch[ch].paused = true;
    g_ch[ch].pause_end = end;
}

void finish(int ch) {
    Channel &c = g_ch[ch];
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace && ch <= 4) std::fprintf(stderr, "[dma] ch%d done chcr 0x%08X tadr 0x%08X madr 0x%08X\n", ch, c.chcr, c.tadr, c.madr);
    c.chcr &= ~0x100u;
    hw::dmac_raise(static_cast<unsigned>(ch));
}

void run_normal(int ch) {
    Channel &c = g_ch[ch];
    if (ch == 8) {
        for (std::uint32_t i = 0; i < c.qwc; ++i)
            std::memcpy(target(c.madr + i * 16), g_spr + ((c.sadr + i * 16) & (kScratchpadSize - 1)), 16);
        c.sadr = (c.sadr + c.qwc * 16) & (kScratchpadSize - 1);
    } else if ((c.chcr & 1u) || ch == 2 || ch == 0 || ch == 9) {
        deliver(ch, c.madr, c.qwc);
    } else {
        std::fprintf(stderr, "[dma] channel %d reads from a device (not emulated)\n", ch);
    }
    c.madr += c.qwc * 16;
    c.qwc = 0;
    if (vif_stalled(ch)) {
        pause(ch, true);
        return;
    }
    finish(ch);
}

void run_chain(int ch) {
    Channel &c = g_ch[ch];
    if (c.qwc) {
        deliver(ch, c.madr, c.qwc);
        c.madr += c.qwc * 16;
        c.qwc = 0;
        if (vif_stalled(ch)) {
            pause(ch, false);
            return;
        }
    }
    const bool tte = c.chcr & 0x40u, tie = c.chcr & 0x80u;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        const std::uint8_t *tagp = source(c.tadr);
        std::uint64_t tag;
        std::memcpy(&tag, tagp, 8);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
        const bool irq = (tag >> 31) & 1u;
        const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
        if (tte && (ch == 0 || ch == 1)) {
            std::uint8_t q[16];
            std::memcpy(q, tagp, 16);
            std::memset(q, 0, 8);
            vif::transfer(ch, q, 1);
        }
        bool end = false;
        std::uint32_t data = c.tadr + 16;
        switch (id) {
        case 0: data = addr_full; c.tadr += 16; end = true; break;
        case 1: c.tadr = data + qwc * 16; break;
        case 2: c.tadr = addr_full; break;
        case 3: case 4: data = addr_full; c.tadr += 16; break;
        case 5: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp < 2) c.asr[asp] = data + qwc * 16;
            c.chcr = (c.chcr & ~0x30u) | (std::min(asp + 1, 2u) << 4);
            c.tadr = addr_full;
            break;
        }
        case 6: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp > 0) {
                c.tadr = c.asr[asp - 1];
                c.chcr = (c.chcr & ~0x30u) | ((asp - 1) << 4);
            } else {
                end = true;
            }
            break;
        }
        default: end = true; break;
        }
        c.madr = data;
        if (ch == 2 && g_starts[2] <= 2) std::fprintf(stderr, "[dma] gif tag id %u qwc %u addr 0x%08X data 0x%08X\n", id, qwc, addr_full, data);
        deliver(ch, data, qwc);
        c.madr = data + qwc * 16;
        if (vif_stalled(ch)) {
            pause(ch, end || (tie && irq));
            return;
        }
        if (end || (tie && irq)) break;
    }
    c.qwc = 0;
    finish(ch);
}

void start(int ch) {
    Channel &c = g_ch[ch];
    c.paused = false;
    c.tag_end = false;
    ++g_starts[ch];
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace) std::fprintf(stderr, "[dma] ch%d chcr 0x%08X madr 0x%08X qwc %u tadr 0x%08X thread %d%s\n", ch, c.chcr, c.madr, c.qwc, c.tadr,
                            kernel::current_thread(), kernel::in_handler() ? " (handler)" : "");
    if (ch == 5 || ch == 6 || ch == 7) {
        finish(ch);
        return;
    }
    if (ch == 3 || ch == 4) {
        if (ch == 3 && !c.qwc) finish(ch);
        else ipu::dma_started(ch);
        return;
    }
    const std::uint32_t mode = (c.chcr >> 2) & 3u;
    if (mode == 1 && ch != 8) run_chain(ch);
    else run_normal(ch);
}

}

void resume(int ch) {
    Channel &c = g_ch[ch];
    if (!c.paused) return;
    c.paused = false;
    if (c.pause_end || ((c.chcr >> 2) & 3u) != 1) {
        c.qwc = 0;
        finish(ch);
        return;
    }
    run_chain(ch);
}

bool ipu_pull(std::uint8_t *qword) {
    Channel &c = g_ch[4];
    if (!(c.chcr & 0x100u)) return false;
    const bool chain = ((c.chcr >> 2) & 3u) == 1;
    while (!c.qwc) {
        if (!chain || c.tag_end) {
            finish(4);
            return false;
        }
        std::uint64_t tag;
        std::memcpy(&tag, source(c.tadr), 8);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
        const std::uint32_t addr = (static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u) | (static_cast<std::uint32_t>(tag >> 63) << 31);
        const bool irq = (tag >> 31) & 1u, tie = c.chcr & 0x80u;
        std::uint32_t data = c.tadr + 16;
        switch (id) {
        case 0: data = addr; c.tadr += 16; c.tag_end = true; break;
        case 1: c.tadr = data + qwc * 16; break;
        case 2: c.tadr = addr; break;
        case 3: case 4: data = addr; c.tadr += 16; break;
        default: c.tag_end = true; break;
        }
        if (tie && irq) c.tag_end = true;
        static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
        if (trace) std::fprintf(stderr, "[dma] ch4 tag id %u qwc %u addr 0x%08X data 0x%08X next 0x%08X end %d\n", id, qwc, addr, data, c.tadr, c.tag_end);
        c.madr = data;
        c.qwc = qwc;
        if (!qwc && c.tag_end) {
            finish(4);
            return false;
        }
    }
    std::memcpy(qword, source(c.madr), 16);
    c.madr += 16;
    if (!--c.qwc && (!chain || c.tag_end)) finish(4);
    return true;
}

bool ipu_push(const std::uint8_t *qword) {
    Channel &c = g_ch[3];
    if (!(c.chcr & 0x100u) || !c.qwc) return false;
    std::memcpy(target(c.madr), qword, 16);
    c.madr += 16;
    if (!--c.qwc) finish(3);
    return true;
}

void reset() {
    for (Channel &c : g_ch) c = Channel{};
    g_ctrl = g_pcr = g_sqwc = g_rbsr = g_rbor = g_stadr = g_enable = 0;
}

void stats() {
    std::fprintf(stderr, "[dma] starts:");
    for (int ch = 0; ch < 10; ++ch) std::fprintf(stderr, " ch%d=%llu", ch, static_cast<unsigned long long>(g_starts[ch]));
    std::fprintf(stderr, "\n");
}

bool owns(std::uint32_t a) { return (a >= 0x10008000u && a < 0x1000E000u) || (a >= 0x1000E000u && a < 0x1000E070u); }

std::uint32_t read(std::uint32_t a) {
    switch (a) {
    case 0x1000E000u: return g_ctrl;
    case 0x1000E020u: return g_pcr;
    case 0x1000E030u: return g_sqwc;
    case 0x1000E040u: return g_rbsr;
    case 0x1000E050u: return g_rbor;
    case 0x1000E060u: return g_stadr;
    default: break;
    }
    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        const Channel &c = g_ch[ch];
        switch (a - kBase[ch]) {
        case 0x00: return c.chcr;
        case 0x10: return c.madr;
        case 0x20: return c.qwc;
        case 0x30: return c.tadr;
        case 0x40: return c.asr[0];
        case 0x50: return c.asr[1];
        case 0x80: return c.sadr;
        default: return 0;
        }
    }
    return 0;
}

void write(std::uint32_t a, std::uint32_t v) {
    switch (a) {
    case 0x1000E000u: g_ctrl = v; return;
    case 0x1000E020u: g_pcr = v; return;
    case 0x1000E030u: g_sqwc = v; return;
    case 0x1000E040u: g_rbsr = v; return;
    case 0x1000E050u: g_rbor = v; return;
    case 0x1000E060u: g_stadr = v; return;
    default: break;
    }
    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        Channel &c = g_ch[ch];
        switch (a - kBase[ch]) {
        case 0x00: {
            const bool was = c.chcr & 0x100u;
            c.chcr = v;
            if (!(v & 0x100u)) c.paused = false;
            if (!was && (v & 0x100u)) start(ch);
            break;
        }
        case 0x10: c.madr = v; break;
        case 0x20: c.qwc = v & 0xFFFFu; break;
        case 0x30: c.tadr = v; break;
        case 0x40: c.asr[0] = v; break;
        case 0x50: c.asr[1] = v; break;
        case 0x80: c.sadr = v & (kScratchpadSize - 16); break;
        default: break;
        }
        return;
    }
}

}
