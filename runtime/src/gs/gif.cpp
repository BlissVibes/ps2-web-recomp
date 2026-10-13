#include "gif.hpp"

#include "gs.hpp"
#include "queue.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::gif {

namespace {

struct Path {
    bool active = false;
    std::uint32_t nloop = 0, loop = 0, nreg = 0, reg = 0, flg = 0;
    std::uint64_t regs = 0;
    bool eop = false;
    bool address_data = false;
};

Path g_paths[4];
std::uint32_t g_stat = 0;
bool g_mask_vif1 = false;       // PATH3 masked by VIF1 MSKPATH3 (GIF_STAT.M3P)
std::uint32_t g_mode = 0;       // GIF_MODE: bit 0 M3R, bit 2 IMT (mirrored into GIF_STAT bits 0 and 2)
// PATH3 data received while PATH3 was masked, in arrival order. What has already been let through is
// the first g_path3_passed bytes: a release moves that mark instead of the data, and the storage is
// kept from frame to frame (a game that meters texture uploads through the mask holds megabytes here
// every frame and releases them a packet at a time).
std::vector<std::uint8_t> g_path3_held;
std::size_t g_path3_passed = 0;

std::size_t held_bytes() { return g_path3_held.size() - g_path3_passed; }
const std::uint8_t *held_data() { return g_path3_held.data() + g_path3_passed; }

// Marks the first bytes of what is held as passed; forgets everything once nothing is left.
void pass_held(std::size_t bytes) {
    g_path3_passed += bytes;
    if (g_path3_passed == g_path3_held.size()) {
        g_path3_held.clear();
        g_path3_passed = 0;
    }
}

// Drops the passed bytes for real, so that the vector holds only what waits.
void compact_held() {
    if (!g_path3_passed) return;
    g_path3_held.erase(g_path3_held.begin(), g_path3_held.begin() + static_cast<std::ptrdiff_t>(g_path3_passed));
    g_path3_passed = 0;
}
constexpr std::uint32_t kGifFifoQwords = 16;
constexpr std::size_t kHeldWorthSaying = 16u << 20;   // several frames of uploads: the game holds 3.5 MB at its fullest

bool path3_masked() { return g_mask_vif1 || (g_mode & 1u); }
std::uint64_t g_tags[4][4];

template <typename Sink>
void walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {
    for (std::uint32_t i = 0; i < qwc; ++i) {
        Reg128 q;
        std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
        if (!p.active) {
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.eop = (q.d[0] >> 15) & 1u;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            sink.tag(p, q);
            p.active = p.nloop != 0;
            continue;
        }
        switch (p.flg) {
        case 0:
            sink.packed(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q);
            if (++p.reg >= p.nreg) {
                p.reg = 0;
                if (++p.loop >= p.nloop) p.active = false;
            }
            break;
        case 1:
            for (int half = 0; half < 2 && p.active; ++half) {
                sink.reglist(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q.d[half]);
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            break;
        default: {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            sink.image(data + static_cast<std::size_t>(i) * 16, run);
            p.loop += run;
            i += run - 1;
            if (p.loop >= p.nloop) p.active = false;
            break;
        }
        }
    }
}

struct GsSink {
    int path;
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
        gs::g_state.q = 1.0f;
        if (((q.d[0] >> 46) & 1u) && p.flg == 0) gs::write(0x00, (q.d[0] >> 47) & 0x7FFu);
    }
    void packed(std::uint32_t reg, const Reg128 &q) { gs::write_packed(reg, q); }
    void reglist(std::uint32_t reg, std::uint64_t value) { gs::write(reg, value); }
    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }
};

void scan(Path &p, const std::uint8_t *data, std::uint32_t qwc) {
    std::uint32_t i = 0;
    while (i < qwc) {
        Reg128 q;
        if (!p.active) {
            std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            p.address_data = false;
            if (p.flg == 0)
                for (std::uint32_t r = 0; r < p.nreg; ++r) p.address_data = p.address_data || ((p.regs >> (r * 4)) & 0xFu) == 0xEu;
            p.active = p.nloop != 0;
            ++i;
            continue;
        }
        if (p.flg == 0 && p.address_data) {
            for (; i < qwc && p.active; ++i) {
                const std::uint8_t *at = data + static_cast<std::size_t>(i) * 16;
                if (((p.regs >> (p.reg * 4)) & 0xFu) == 0xEu && at[8] >= 0x60 && at[8] <= 0x62) {
                    std::uint64_t value;
                    std::memcpy(&value, at, 8);
                    gs::signal(at[8], value);
                }
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            continue;
        }
        const std::uint64_t done = static_cast<std::uint64_t>(p.loop) * p.nreg + p.reg;
        const std::uint64_t left = static_cast<std::uint64_t>(p.nloop) * p.nreg - done;
        std::uint64_t take, used;
        if (p.flg == 0) {
            take = used = std::min<std::uint64_t>(left, qwc - i);
        } else if (p.flg == 1) {
            take = std::min<std::uint64_t>((left + 1) / 2, qwc - i);
            used = std::min<std::uint64_t>(take * 2, left);
        } else {
            take = std::min<std::uint64_t>(p.nloop - p.loop, qwc - i);
            p.loop += static_cast<std::uint32_t>(take);
            if (p.loop >= p.nloop) p.active = false;
            i += static_cast<std::uint32_t>(take);
            continue;
        }
        p.loop = static_cast<std::uint32_t>((done + used) / p.nreg);
        p.reg = static_cast<std::uint32_t>((done + used) % p.nreg);
        if (used == left) p.active = false;
        i += static_cast<std::uint32_t>(take);
    }
}

Path g_scan[4];

// PATH3 masking works on GS packets, not on bytes: a mask that arrives while a packet is on its way
// takes effect when that packet ends (the tag with EOP and its data), and lifting the mask lets
// the next packet start at once. A game can therefore queue many packets on the GIF channel and
// meter them out one at a time with MSKPATH3 off/on pairs in the VIF1 stream, each pair releasing
// exactly one packet (RenderWare does this with one batch of texture uploads per packet, all
// into the same GS memory, each followed by the geometry that uses it).
//
// g_cut follows the PATH3 stream as it is let through, so that the end of the current packet can
// be found. It lives in slot 0 of g_scan, which no path uses and which is already part of the
// saved state, so the state layout does not change: active = inside a tag's data, nloop/loop =
// that data in qwords and how much of it has passed, eop = the tag ends the packet,
// address_data = a packet is open (some of it has passed and its end has not).
Path &g_cut = g_scan[0];

bool trace_path3() {
    static const bool enabled = std::getenv("PS2_TRACE_PATH3") != nullptr;
    return enabled;
}

// How many of the qwc qwords belong to the packet the stream is in, or to the next one when it is
// between packets. Advances g_cut past them.
std::uint32_t packet_span(const std::uint8_t *data, std::uint32_t qwc) {
    Path &p = g_cut;
    std::uint32_t i = 0;
    while (i < qwc) {
        if (!p.active) {
            std::uint64_t tag;
            std::memcpy(&tag, data + static_cast<std::size_t>(i) * 16, 8);
            const std::uint32_t nloop = static_cast<std::uint32_t>(tag) & 0x7FFFu, flg = static_cast<std::uint32_t>(tag >> 58) & 3u;
            std::uint32_t nreg = static_cast<std::uint32_t>(tag >> 60) & 0xFu;
            if (!nreg) nreg = 16;
            p.eop = (tag >> 15) & 1u;
            p.nloop = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            p.loop = 0;
            p.active = p.nloop != 0;
            p.address_data = true;
            ++i;
        } else {
            const std::uint32_t take = std::min(qwc - i, p.nloop - p.loop);
            p.loop += take;
            i += take;
            if (p.loop >= p.nloop) p.active = false;
        }
        if (!p.active && p.eop) {
            p.address_data = false;
            return i;
        }
    }
    return i;
}

void follow(const std::uint8_t *data, std::uint32_t qwc) {
    while (qwc) {
        const std::uint32_t n = packet_span(data, qwc);
        data += static_cast<std::size_t>(n) * 16;
        qwc -= n;
    }
}

}

void reset() {
    g_stat = 0;
    g_mask_vif1 = false;
    g_mode = 0;
    g_path3_held.clear();
    g_path3_passed = 0;
    g_cut = Path{};
    if (gs::queue::active()) {
        for (Path &p : g_scan) p = Path{};
        gs::queue::post(gs::queue::kGifReset, 0, 0, nullptr, 0);
        return;
    }
    render_reset();
}

void render_reset() {
    for (Path &p : g_paths) p = Path{};
}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) {
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);
}

namespace {

void forward(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
    // One message carries a limited amount, and what PATH3 holds has no limit. The data may be cut between
    // any two qwords: the messages of one call follow each other in the queue, and the reader walks the
    // path's stream with what it kept from the message before (g_paths).
    constexpr std::uint32_t kMost = gs::queue::kMaxPayload / 16;
    while (qwc) {
        const std::uint32_t n = std::min(qwc, kMost);
        gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), n, data, n * 16);
        data += static_cast<std::size_t>(n) * 16;
        qwc -= n;
    }
}

// Everything held goes to the GS: PATH3 has been open long enough to drain.
void release_path3() {
    if (!held_bytes()) return;
    const std::uint32_t qwc = static_cast<std::uint32_t>(held_bytes() / 16);
    if (trace_path3()) std::fprintf(stderr, "[gif] path3 open: all %u held qwords pass\n", qwc);
    follow(held_data(), qwc);
    forward(3, held_data(), qwc);
    pass_held(static_cast<std::size_t>(qwc) * 16);
}

// The mask has just been lifted: the first held packet starts, and nothing can stop it any more.
void start_path3() {
    if (!held_bytes()) return;
    const std::uint32_t total = static_cast<std::uint32_t>(held_bytes() / 16);
    const std::uint32_t n = packet_span(held_data(), total);
    if (trace_path3()) std::fprintf(stderr, "[gif] path3 unmasked: one packet of %u qwords passes, %u stay held\n", n, total - n);
    forward(3, held_data(), n);
    pass_held(static_cast<std::size_t>(n) * 16);
}

}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path != 3) {
        forward(path, data, qwc);
        return;
    }
    if (!path3_masked()) {
        release_path3();
        follow(data, qwc);
        forward(3, data, qwc);
        return;
    }
    // Masked. The rest of a packet that was already on its way still passes; whole packets wait.
    if (!held_bytes() && g_cut.address_data) {
        const std::uint32_t n = packet_span(data, qwc);
        forward(3, data, n);
        data += static_cast<std::size_t>(n) * 16;
        qwc -= n;
    }
    const std::size_t bytes = static_cast<std::size_t>(qwc) * 16;
    if (g_path3_held.size() + bytes > g_path3_held.capacity()) compact_held();   // before the storage grows, reuse what has passed
    g_path3_held.insert(g_path3_held.end(), data, data + bytes);
    // The hardware would stall the transfer; here it is held, without limit. A game that keeps sending and
    // never lifts the mask should show in the log rather than only in the memory used.
    static bool said = false;
    if (!said && held_bytes() > kHeldWorthSaying) {
        said = true;
        std::fprintf(stderr, "[gif] more than %zu MB of PATH3 data is held behind the mask and it is still growing\n", kHeldWorthSaying >> 20);
    }
}

void path3_advance() {
    if (held_bytes() && !path3_masked()) release_path3();
}

void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) { scan(g_scan[path], data, qwc); }

bool busy(int path) { return g_paths[path].active; }

void stats() {
    for (int p = 1; p < 4; ++p)
        std::fprintf(stderr, "[gif] path %d tags: packed %llu reglist %llu image %llu\n", p,
                     static_cast<unsigned long long>(g_tags[p][0]), static_cast<unsigned long long>(g_tags[p][1]),
                     static_cast<unsigned long long>(g_tags[p][2] + g_tags[p][3]));
}

void set_path3_mask(bool masked) {
    const bool was = path3_masked();
    g_mask_vif1 = masked;
    if (trace_path3()) std::fprintf(stderr, "[gif] mskpath3 %d, %zu qwords held\n", masked ? 1 : 0, held_bytes() / 16);
    if (was && !path3_masked()) start_path3();
}

std::uint32_t read(std::uint32_t a) {
    if ((a & 0xF0u) != 0x20u) return 0;
    path3_advance();   // the EE had time to come and look: an open PATH3 has drained
    const std::uint32_t held = static_cast<std::uint32_t>(held_bytes() / 16);
    const std::uint32_t fqc = std::min(held, kGifFifoQwords);
    return g_stat | (g_mode & 5u) | (g_mask_vif1 ? 2u : 0u) | (held ? (1u << 6) : 0u) | (fqc << 24);
}

void write(std::uint32_t a, std::uint32_t v) {
    if ((a & 0xF0u) == 0x00u && (v & 1u)) reset();
    if ((a & 0xF0u) == 0x10u) {
        const bool was = path3_masked();
        g_mode = v & 5u;
        if (was && !path3_masked()) start_path3();
    }
}

}

namespace ps2::gif {

void state(ps2::state::Stream &s) {
    s.tag("gif");
    s.pod(g_paths);
    s.pod(g_stat);
    s.pod(g_mask_vif1);
    s.pod(g_mode);
    compact_held();   // the state holds only what waits, as it always did
    s.vec(g_path3_held);
    s.pod(g_tags);
    s.pod(g_scan);
}

}
