#include "vu_micro.hpp"

#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "vu_jit_web.hpp"
#include "hw.hpp"
#include "ps2/fpu.hpp"
#include "ps2/system.hpp"
#include "ps2/vu.hpp"
#include "ps2/vu_exact.hpp"
#include "ps2/vu_ops.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ps2::vu {

namespace {

struct Unit {
    Vu0Regs *regs;
    std::uint8_t *code;
    std::uint8_t *data;
    std::uint32_t code_mask;
    std::uint32_t data_qwords;
    std::uint32_t tpc;
    vuops::FlagPipe *pipe = nullptr;
    bool dry = false;   // XGKICK sends nothing: the call is run a second time, for real
};

Vu0Regs g_vu1;
std::uint32_t g_top = 0, g_itop = 0;
std::uint64_t g_reported = 0;
Vu0Counters g_vu0{};

Unit unit_of(int n) {
    if (n == 0) return Unit{&g_cpu.vu0, g_vu0_code, g_vu0_mem, 4095u, 256u, g_cpu.vu0.tpc};
    return Unit{&g_vu1, g_vu1_code, g_vu1_mem, 16383u, 1024u, g_vu1.tpc};
}

struct UpperOut {
    int target = -1;
    Reg128 value{};
    unsigned dest = 0;
    bool flags = false;
    bool has_clip = false;
    std::uint32_t clip = 0;
};

constexpr int kAcc = 32;

std::uint32_t mac_flags(const Reg128 &v, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    return mac;
}

void set_flags(Vu0Regs &r, const Reg128 &v, unsigned dest) {
    const std::uint32_t mac = mac_flags(v, dest);
    r.mac = mac;
    std::uint32_t status = r.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0x0F00u) status |= 0x4u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}

template <typename F>
UpperOut lanes(int target, unsigned dest, bool flags, F f) {
    UpperOut o;
    o.target = target;
    o.dest = dest;
    o.flags = flags;
    for (int i = 0; i < 4; ++i)
        if (dest & (8u >> i)) o.value.w[i] = f(i);
    return o;
}

UpperOut upper(Vu0Regs &r, std::uint32_t w) {
    const unsigned dest = (w >> 21) & 15u, ft = (w >> 16) & 31u, fs = (w >> 11) & 31u, fd = (w >> 6) & 31u, bc = w & 3u;
    const Reg128 s = r.vf[fs], t = r.vf[ft], acc = r.acc;
    const std::uint32_t b = t.w[bc];
    const unsigned funct = w & 63u;
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], b); });
        case 0x04: case 0x05: case 0x06: case 0x07: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], b); });
        case 0x08: case 0x09: case 0x0A: case 0x0B: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], b)); });
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], b)); });
        case 0x10: case 0x11: case 0x12: case 0x13: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], b); });
        case 0x14: case 0x15: case 0x16: case 0x17: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], b); });
        case 0x18: case 0x19: case 0x1A: case 0x1B: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], b); });
        case 0x1C: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], r.q); });
        case 0x1D: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], r.i); });
        case 0x1E: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], r.i); });
        case 0x1F: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], r.i); });
        case 0x20: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], r.q); });
        case 0x21: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.q)); });
        case 0x22: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], r.i); });
        case 0x23: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.i)); });
        case 0x24: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], r.q); });
        case 0x25: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.q)); });
        case 0x26: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], r.i); });
        case 0x27: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.i)); });
        case 0x28: return lanes(fd, dest, true, [&](int i) { return fp::add(s.w[i], t.w[i]); });
        case 0x29: return lanes(fd, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], t.w[i])); });
        case 0x2A: return lanes(fd, dest, true, [&](int i) { return fp::mul(s.w[i], t.w[i]); });
        case 0x2B: return lanes(fd, dest, false, [&](int i) { return fp::max(s.w[i], t.w[i]); });
        case 0x2C: return lanes(fd, dest, true, [&](int i) { return fp::sub(s.w[i], t.w[i]); });
        case 0x2D: return lanes(fd, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], t.w[i])); });
        case 0x2E: {
            UpperOut o;
            o.target = static_cast<int>(fd);
            o.dest = 0xEu;
            o.flags = true;
            o.value.w[0] = fp::sub(acc.w[0], fp::mul(s.w[1], t.w[2]));
            o.value.w[1] = fp::sub(acc.w[1], fp::mul(s.w[2], t.w[0]));
            o.value.w[2] = fp::sub(acc.w[2], fp::mul(s.w[0], t.w[1]));
            return o;
        }
        case 0x2F: return lanes(fd, dest, false, [&](int i) { return fp::min(s.w[i], t.w[i]); });
        default: return UpperOut{};
        }
    }
    const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
    switch (index) {
    case 0x00: case 0x01: case 0x02: case 0x03: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], b); });
    case 0x04: case 0x05: case 0x06: case 0x07: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], b); });
    case 0x08: case 0x09: case 0x0A: case 0x0B: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], b)); });
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], b)); });
    case 0x10: case 0x11: case 0x12: case 0x13: {
        static const int kFrac[4] = {0, 4, 12, 15};
        return lanes(ft, dest, false, [&](int i) { return fp::from_fixed(s.sw[i], kFrac[index & 3u]); });
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {
        static const int kFrac[4] = {0, 4, 12, 15};
        return lanes(ft, dest, false, [&](int i) { return fp::to_fixed(s.w[i], kFrac[index & 3u]); });
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], b); });
    case 0x1C: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], r.q); });
    case 0x1D: return lanes(ft, dest, false, [&](int i) { return s.w[i] & fp::kAbs; });
    case 0x1E: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], r.i); });
    case 0x1F: {
        UpperOut o;
        o.has_clip = true;
        o.clip = vuops::clip_value(r, s, t.w[3]);
        return o;
    }
    case 0x20: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], r.q); });
    case 0x21: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.q)); });
    case 0x22: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], r.i); });
    case 0x23: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], r.i)); });
    case 0x24: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], r.q); });
    case 0x25: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.q)); });
    case 0x26: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], r.i); });
    case 0x27: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], r.i)); });
    case 0x28: return lanes(kAcc, dest, true, [&](int i) { return fp::add(s.w[i], t.w[i]); });
    case 0x29: return lanes(kAcc, dest, true, [&](int i) { return fp::add(acc.w[i], fp::mul(s.w[i], t.w[i])); });
    case 0x2A: return lanes(kAcc, dest, true, [&](int i) { return fp::mul(s.w[i], t.w[i]); });
    case 0x2C: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(s.w[i], t.w[i]); });
    case 0x2D: return lanes(kAcc, dest, true, [&](int i) { return fp::sub(acc.w[i], fp::mul(s.w[i], t.w[i])); });
    case 0x2E: {
        UpperOut o;
        o.target = kAcc;
        o.dest = 0xEu;
        o.flags = true;
        o.value.w[0] = fp::mul(s.w[1], t.w[2]);
        o.value.w[1] = fp::mul(s.w[2], t.w[0]);
        o.value.w[2] = fp::mul(s.w[0], t.w[1]);
        return o;
    }
    default: return UpperOut{};
    }
}

enum UpperKind : std::uint8_t { kNop, kAdd, kSub, kMadd, kMsub, kMax, kMin, kMul, kAbs, kFtoi, kItof, kClip, kOpmula, kOpmsub };
enum UpperSource : std::uint8_t { kVec, kBc, kQ, kI };

struct Decoded {
    std::uint32_t lo = 0, hi = 0;
    bool valid = false;
    UpperKind kind = kNop;
    UpperSource source = kVec;
    std::uint8_t target = 0, dest = 0, fs = 0, ft = 0, bc = 0, frac = 0;
    bool flags = false;
};

Decoded g_decoded[2][2048];
bool g_flags_used[2] = {true, true};
bool g_scan[2] = {true, true};

Decoded decode_upper(std::uint32_t lo, std::uint32_t hi) {
    Decoded d;
    d.lo = lo;
    d.hi = hi;
    d.valid = true;
    d.dest = static_cast<std::uint8_t>((hi >> 21) & 15u);
    d.ft = static_cast<std::uint8_t>((hi >> 16) & 31u);
    d.fs = static_cast<std::uint8_t>((hi >> 11) & 31u);
    d.bc = static_cast<std::uint8_t>(hi & 3u);
    const std::uint8_t fd = static_cast<std::uint8_t>((hi >> 6) & 31u);
    const unsigned funct = hi & 63u;
    const auto set = [&](UpperKind k, UpperSource src, int target, bool flags) {
        d.kind = k;
        d.source = src;
        d.target = static_cast<std::uint8_t>(target);
        d.flags = flags;
    };
    if (funct < 0x3C) {
        switch (funct) {
        case 0x00: case 0x01: case 0x02: case 0x03: set(kAdd, kBc, fd, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: set(kSub, kBc, fd, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: set(kMadd, kBc, fd, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(kMsub, kBc, fd, true); break;
        case 0x10: case 0x11: case 0x12: case 0x13: set(kMax, kBc, fd, false); break;
        case 0x14: case 0x15: case 0x16: case 0x17: set(kMin, kBc, fd, false); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: set(kMul, kBc, fd, true); break;
        case 0x1C: set(kMul, kQ, fd, true); break;
        case 0x1D: set(kMax, kI, fd, false); break;
        case 0x1E: set(kMul, kI, fd, true); break;
        case 0x1F: set(kMin, kI, fd, false); break;
        case 0x20: set(kAdd, kQ, fd, true); break;
        case 0x21: set(kMadd, kQ, fd, true); break;
        case 0x22: set(kAdd, kI, fd, true); break;
        case 0x23: set(kMadd, kI, fd, true); break;
        case 0x24: set(kSub, kQ, fd, true); break;
        case 0x25: set(kMsub, kQ, fd, true); break;
        case 0x26: set(kSub, kI, fd, true); break;
        case 0x27: set(kMsub, kI, fd, true); break;
        case 0x28: set(kAdd, kVec, fd, true); break;
        case 0x29: set(kMadd, kVec, fd, true); break;
        case 0x2A: set(kMul, kVec, fd, true); break;
        case 0x2B: set(kMax, kVec, fd, false); break;
        case 0x2C: set(kSub, kVec, fd, true); break;
        case 0x2D: set(kMsub, kVec, fd, true); break;
        case 0x2E: set(kOpmsub, kVec, fd, true); d.dest = 0xE; break;
        case 0x2F: set(kMin, kVec, fd, false); break;
        default: break;
        }
        return d;
    }
    static const std::uint8_t kFrac[4] = {0, 4, 12, 15};
    const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
    switch (index) {
    case 0x00: case 0x01: case 0x02: case 0x03: set(kAdd, kBc, kAcc, true); break;
    case 0x04: case 0x05: case 0x06: case 0x07: set(kSub, kBc, kAcc, true); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: set(kMadd, kBc, kAcc, true); break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: set(kMsub, kBc, kAcc, true); break;
    case 0x10: case 0x11: case 0x12: case 0x13: set(kItof, kVec, d.ft, false); d.frac = kFrac[index & 3u]; break;
    case 0x14: case 0x15: case 0x16: case 0x17: set(kFtoi, kVec, d.ft, false); d.frac = kFrac[index & 3u]; break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: set(kMul, kBc, kAcc, true); break;
    case 0x1C: set(kMul, kQ, kAcc, true); break;
    case 0x1D: set(kAbs, kVec, d.ft, false); break;
    case 0x1E: set(kMul, kI, kAcc, true); break;
    case 0x1F: set(kClip, kVec, 0, false); break;
    case 0x20: set(kAdd, kQ, kAcc, true); break;
    case 0x21: set(kMadd, kQ, kAcc, true); break;
    case 0x22: set(kAdd, kI, kAcc, true); break;
    case 0x23: set(kMadd, kI, kAcc, true); break;
    case 0x24: set(kSub, kQ, kAcc, true); break;
    case 0x25: set(kMsub, kQ, kAcc, true); break;
    case 0x26: set(kSub, kI, kAcc, true); break;
    case 0x27: set(kMsub, kI, kAcc, true); break;
    case 0x28: set(kAdd, kVec, kAcc, true); break;
    case 0x29: set(kMadd, kVec, kAcc, true); break;
    case 0x2A: set(kMul, kVec, kAcc, true); break;
    case 0x2C: set(kSub, kVec, kAcc, true); break;
    case 0x2D: set(kMsub, kVec, kAcc, true); break;
    case 0x2E: set(kOpmula, kVec, kAcc, true); d.dest = 0xE; break;
    default: break;
    }
    return d;
}

float fin(std::uint32_t x) {
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) x &= 0x80000000u;
    else if (e == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

std::uint32_t fout(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;
    return x;
}

float norm(float f) { return fin(fout(f)); }

void fast_upper(Vu0Regs &r, const Decoded &d, UpperOut &o) {
    o.target = -1;
    o.has_clip = false;
    if (d.kind == kNop) return;
    const Reg128 &s = r.vf[d.fs], &t = r.vf[d.ft];
    if (d.kind == kClip) {
        o.has_clip = true;
        o.clip = vuops::clip_value(r, s, t.w[3]);
        return;
    }
    o.target = d.target;
    o.dest = d.dest;
    o.flags = d.flags;
    switch (d.kind) {
    case kAbs:
        for (int i = 0; i < 4; ++i) o.value.w[i] = s.w[i] & fp::kAbs;
        return;
    case kFtoi:
        for (int i = 0; i < 4; ++i) if (d.dest & (8u >> i)) o.value.w[i] = fp::to_fixed(s.w[i], d.frac);
        return;
    case kItof:
        for (int i = 0; i < 4; ++i) if (d.dest & (8u >> i)) o.value.w[i] = fp::from_fixed(s.sw[i], d.frac);
        return;
    case kOpmula:
        o.value.w[0] = fout(norm(fin(s.w[1]) * fin(t.w[2])));
        o.value.w[1] = fout(norm(fin(s.w[2]) * fin(t.w[0])));
        o.value.w[2] = fout(norm(fin(s.w[0]) * fin(t.w[1])));
        return;
    case kOpmsub:
        o.value.w[0] = fout(vuops::add_vu(fin(r.acc.w[0]), -norm(fin(s.w[1]) * fin(t.w[2]))));
        o.value.w[1] = fout(vuops::add_vu(fin(r.acc.w[1]), -norm(fin(s.w[2]) * fin(t.w[0]))));
        o.value.w[2] = fout(vuops::add_vu(fin(r.acc.w[2]), -norm(fin(s.w[0]) * fin(t.w[1]))));
        return;
    default: break;
    }
    std::uint32_t b[4];
    switch (d.source) {
    case kVec: for (int i = 0; i < 4; ++i) b[i] = t.w[i]; break;
    case kBc: for (int i = 0; i < 4; ++i) b[i] = t.w[d.bc]; break;
    case kQ: for (int i = 0; i < 4; ++i) b[i] = r.q; break;
    case kI: for (int i = 0; i < 4; ++i) b[i] = r.i; break;
    }
    switch (d.kind) {
    case kAdd: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(vuops::add_vu(fin(s.w[i]), fin(b[i]))); break;
    case kSub: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(vuops::add_vu(fin(s.w[i]), -fin(b[i]))); break;
    case kMul: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(fin(s.w[i]) * fin(b[i])); break;
    case kMadd: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(vuops::add_vu(fin(r.acc.w[i]), norm(fin(s.w[i]) * fin(b[i])))); break;
    case kMsub: for (int i = 0; i < 4; ++i) o.value.w[i] = fout(vuops::add_vu(fin(r.acc.w[i]), -norm(fin(s.w[i]) * fin(b[i])))); break;
    case kMax: for (int i = 0; i < 4; ++i) o.value.w[i] = fp::max(s.w[i], b[i]); break;
    case kMin: for (int i = 0; i < 4; ++i) o.value.w[i] = fp::min(s.w[i], b[i]); break;
    default: break;
    }
}

void scan_flags(int unit, const std::uint8_t *code, std::uint32_t size) {
    bool used = false;
    for (std::uint32_t pc = 0; pc < size && !used; pc += 8) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + pc, 4);
        std::memcpy(&hi, code + pc + 4, 4);
        if (hi & 0x80000000u) continue;
        switch (lo >> 25) {
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B: used = true; break;
        default: break;
        }
    }
    g_flags_used[unit] = used;
    g_scan[unit] = false;
}

void commit(Vu0Regs &r, const UpperOut &o) {
    if (o.target < 0) return;
    if (o.target > 0) {
        Reg128 &dst = o.target == kAcc ? r.acc : r.vf[o.target];
        for (int i = 0; i < 4; ++i)
            if (o.dest & (8u >> i)) dst.w[i] = o.value.w[i];
    }
    if (o.flags) set_flags(r, o.value, o.dest);
}

std::uint8_t *qword(Unit &u, std::uint32_t index) {
    return u.data + (index & (u.data_qwords - 1)) * 16;
}

void store_vi(Vu0Regs &r, unsigned index, std::uint32_t value) {
    if (index) r.vi[index] = static_cast<std::uint16_t>(value);
}

std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

float u2f(std::uint32_t v) {
    if (!(v & 0x7F800000u)) v &= 0x80000000u;
    if ((v & 0x7F800000u) == 0x7F800000u) v = (v & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void xgkick(Unit &u, std::uint32_t address) {
    std::uint8_t buffer[16 * 1024 + 16];
    std::uint32_t q = address & (u.data_qwords - 1), count = 0;
    bool done = false;
    while (!done && count < u.data_qwords) {
        Reg128 tag;
        std::memcpy(&tag, qword(u, q), 16);
        std::memcpy(buffer + count * 16, &tag, 16);
        ++q;
        ++count;
        const std::uint32_t nloop = static_cast<std::uint32_t>(tag.d[0]) & 0x7FFFu;
        const bool eop = (tag.d[0] >> 15) & 1u;
        const std::uint32_t flg = static_cast<std::uint32_t>(tag.d[0] >> 58) & 3u;
        std::uint32_t nreg = static_cast<std::uint32_t>(tag.d[0] >> 60) & 0xFu;
        if (!nreg) nreg = 16;
        std::uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (std::uint32_t i = 0; i < body && count < u.data_qwords; ++i, ++q, ++count)
            std::memcpy(buffer + count * 16, qword(u, q), 16);
        done = eop;
    }
    gif::transfer(1, buffer, count);
}

void lower(Unit &u, std::uint32_t w, std::uint32_t pc, std::uint32_t &npc, bool &branch) {
    Vu0Regs &r = *u.regs;
    const unsigned dest = (w >> 21) & 15u, it = (w >> 16) & 15u, is = (w >> 11) & 15u, id = (w >> 6) & 15u;
    const std::uint32_t ft = (w >> 16) & 31u, fs = (w >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(w << 21) >> 21;
    const auto clip_flags = [&]() { return u.pipe ? u.pipe->clip() : r.clip; };
    const auto status_flags = [&]() { return u.pipe ? u.pipe->status() : r.status; };
    const auto mac_flags_now = [&]() { return u.pipe ? u.pipe->mac() : r.mac; };
    const auto branch_to = [&](std::uint32_t target) {
        npc = target & u.code_mask;
        branch = true;
    };
    if ((w >> 25) == 0x40u) {
        const unsigned funct = w & 63u;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: store_vi(r, id, r.vi[is] + r.vi[it]); break;
            case 0x31: store_vi(r, id, r.vi[is] - r.vi[it]); break;
            case 0x32: store_vi(r, it, r.vi[is] + static_cast<std::uint32_t>(static_cast<std::int32_t>(((w >> 6) & 31u) << 27) >> 27)); break;
            case 0x34: store_vi(r, id, r.vi[is] & r.vi[it]); break;
            case 0x35: store_vi(r, id, r.vi[is] | r.vi[it]); break;
            default:
                if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower op 0x%08X at 0x%04X\n", w, pc);
                break;
            }
            return;
        }
        const unsigned index = ((w >> 6) & 31u) * 4u + (funct & 3u);
        const unsigned fsf = (w >> 21) & 3u, ftf = (w >> 23) & 3u;
        switch (index) {
        case 0x30: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.vf[fs].w[i]; break;
        case 0x31: if (ft) { const Reg128 s = r.vf[fs]; for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = s.w[(i + 1) & 3]; } break;
        case 0x34: {
            const std::uint8_t *q = qword(u, r.vi[is]);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
            store_vi(r, is, r.vi[is] + 1u);
            break;
        }
        case 0x35: {
            std::uint8_t *q = qword(u, r.vi[it]);
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
            store_vi(r, it, r.vi[it] + 1u);
            break;
        }
        case 0x36: {
            store_vi(r, is, r.vi[is] - 1u);
            const std::uint8_t *q = qword(u, r.vi[is]);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
            break;
        }
        case 0x37: {
            store_vi(r, it, r.vi[it] - 1u);
            std::uint8_t *q = qword(u, r.vi[it]);
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
            break;
        }
        case 0x38: vu::div(r, r.vf[fs].w[fsf], r.vf[ft].w[ftf]); break;
        case 0x39: vu::sqrt(r, r.vf[ft].w[ftf]); break;
        case 0x3A: vu::rsqrt(r, r.vf[fs].w[fsf], r.vf[ft].w[ftf]); break;
        case 0x3B: break;
        case 0x3C: store_vi(r, it, r.vf[fs].w[fsf]); break;
        case 0x3D: if (ft) { const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r.vi[is]))); for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = v; } break;
        case 0x3E: {
            const std::uint8_t *q = qword(u, r.vi[is]);
            for (int i = 0; i < 4; ++i)
                if (dest & (8u >> i)) {
                    std::uint16_t v;
                    std::memcpy(&v, q + i * 4, 2);
                    store_vi(r, it, v);
                    break;
                }
            break;
        }
        case 0x3F: {
            std::uint8_t *q = qword(u, r.vi[is]);
            const std::uint32_t v = r.vi[it];
            for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &v, 4);
            break;
        }
        case 0x40:
            r.r = vu::rand_next(r.r);
            if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.r | fp::kOne;
            break;
        case 0x41: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = (r.r & 0x7FFFFFu) | fp::kOne; break;
        case 0x42: r.r = r.vf[fs].w[fsf] & 0x7FFFFFu; break;
        case 0x43: r.r = (r.r ^ r.vf[fs].w[fsf]) & 0x7FFFFFu; break;
        case 0x64: if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) r.vf[ft].w[i] = r.p; break;
        case 0x68: store_vi(r, it, g_top); break;
        case 0x69: store_vi(r, it, g_itop); break;
        case 0x6C:
            if (u.dry) ++g_vu0.kicks_skipped;
            else xgkick(u, r.vi[is]);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: {
            const float x = u2f(r.vf[fs].w[0]), y = u2f(r.vf[fs].w[1]), z = u2f(r.vf[fs].w[2]);
            const float sum = x * x + y * y + z * z;
            float p = sum;
            if (index == 0x71) p = sum != 0.0f ? 1.0f / sum : 3.4e38f;
            if (index == 0x72) p = std::sqrt(sum);
            if (index == 0x73) p = sum != 0.0f ? 1.0f / std::sqrt(sum) : 3.4e38f;
            r.p = f2u(p);
            break;
        }
        case 0x74: r.p = f2u(std::atan2(u2f(r.vf[fs].w[1]), u2f(r.vf[fs].w[0]))); break;
        case 0x75: r.p = f2u(std::atan2(u2f(r.vf[fs].w[2]), u2f(r.vf[fs].w[0]))); break;
        case 0x76: r.p = f2u(u2f(r.vf[fs].w[0]) + u2f(r.vf[fs].w[1]) + u2f(r.vf[fs].w[2]) + u2f(r.vf[fs].w[3])); break;
        case 0x78: r.p = f2u(std::sqrt(std::fabs(u2f(r.vf[fs].w[fsf])))); break;
        case 0x79: { const float v = std::sqrt(std::fabs(u2f(r.vf[fs].w[fsf]))); r.p = f2u(v != 0.0f ? 1.0f / v : 3.4e38f); break; }
        case 0x7A: { const float v = u2f(r.vf[fs].w[fsf]); r.p = f2u(v != 0.0f ? 1.0f / v : 3.4e38f); break; }
        case 0x7B: break;
        case 0x7C: r.p = f2u(std::sin(u2f(r.vf[fs].w[fsf]))); break;
        case 0x7D: r.p = f2u(std::atan(u2f(r.vf[fs].w[fsf]))); break;
        case 0x7E: r.p = f2u(std::exp(-u2f(r.vf[fs].w[fsf]))); break;
        default:
            if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower special 0x%X (0x%08X) at 0x%04X\n", index, w, pc);
            break;
        }
        return;
    }
    const std::uint32_t target = pc + 8u + static_cast<std::uint32_t>(imm11) * 8u;
    switch (w >> 25) {
    case 0x00: {
        const std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        if (ft) for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(&r.vf[ft].w[i], q + i * 4, 4);
        break;
    }
    case 0x01: {
        std::uint8_t *q = qword(u, r.vi[it] + static_cast<std::uint32_t>(imm11));
        for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &r.vf[fs].w[i], 4);
        break;
    }
    case 0x04: {
        const std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        for (int i = 0; i < 4; ++i)
            if (dest & (8u >> i)) {
                std::uint16_t v;
                std::memcpy(&v, q + i * 4, 2);
                store_vi(r, it, v);
                break;
            }
        break;
    }
    case 0x05: {
        std::uint8_t *q = qword(u, r.vi[is] + static_cast<std::uint32_t>(imm11));
        const std::uint32_t v = r.vi[it];
        for (int i = 0; i < 4; ++i) if (dest & (8u >> i)) std::memcpy(q + i * 4, &v, 4);
        break;
    }
    case 0x08: store_vi(r, it, r.vi[is] + (((w >> 10) & 0x7800u) | (w & 0x7FFu))); break;
    case 0x09: store_vi(r, it, r.vi[is] - (((w >> 10) & 0x7800u) | (w & 0x7FFu))); break;
    case 0x10: store_vi(r, 1, (clip_flags() & 0xFFFFFFu) == (w & 0xFFFFFFu) ? 1u : 0u); break;
    case 0x11:
        r.clip = w & 0xFFFFFFu;
        if (u.pipe) u.pipe->set_clip(r.clip, u.pipe->t - 4u);
        break;
    case 0x12: store_vi(r, 1, (clip_flags() & w & 0xFFFFFFu) ? 1u : 0u); break;
    case 0x13: store_vi(r, 1, ((clip_flags() | w) & 0xFFFFFFu) == 0xFFFFFFu ? 1u : 0u); break;
    case 0x14: store_vi(r, it, (status_flags() & 0xFFFu) == ((((w >> 10) & 0x800u) | (w & 0x7FFu))) ? 1u : 0u); break;
    case 0x15:
        r.status = (status_flags() & 0x3Fu) | ((((w >> 10) & 0x800u) | (w & 0x7FFu)) & 0xFC0u);
        if (u.pipe) u.pipe->force_mac(r.mac, r.status);
        break;
    case 0x16: store_vi(r, it, status_flags() & (((w >> 10) & 0x800u) | (w & 0x7FFu))); break;
    case 0x17: store_vi(r, it, (status_flags() | (((w >> 10) & 0x800u) | (w & 0x7FFu))) & 0xFFFu); break;
    case 0x18: store_vi(r, it, (mac_flags_now() & 0xFFFFu) == r.vi[is] ? 1u : 0u); break;
    case 0x1A: store_vi(r, it, mac_flags_now() & r.vi[is]); break;
    case 0x1B: store_vi(r, it, (mac_flags_now() | r.vi[is]) & 0xFFFFu); break;
    case 0x1C: store_vi(r, it, clip_flags() & 0xFFFu); break;
    case 0x20: branch_to(target); break;
    case 0x21: store_vi(r, it, (pc + 16u) / 8u); branch_to(target); break;
    case 0x24: branch_to(static_cast<std::uint32_t>(r.vi[is]) * 8u); break;
    case 0x25: { const std::uint32_t t = static_cast<std::uint32_t>(r.vi[is]) * 8u; store_vi(r, it, (pc + 16u) / 8u); branch_to(t); break; }
    case 0x28: if (r.vi[is] == r.vi[it]) branch_to(target); break;
    case 0x29: if (r.vi[is] != r.vi[it]) branch_to(target); break;
    case 0x2C: if (static_cast<std::int16_t>(r.vi[is]) < 0) branch_to(target); break;
    case 0x2D: if (static_cast<std::int16_t>(r.vi[is]) > 0) branch_to(target); break;
    case 0x2E: if (static_cast<std::int16_t>(r.vi[is]) <= 0) branch_to(target); break;
    case 0x2F: if (static_cast<std::int16_t>(r.vi[is]) >= 0) branch_to(target); break;
    default:
        if (g_reported++ < 32) std::fprintf(stderr, "[vu] lower 0x%08X at 0x%04X\n", w, pc);
        break;
    }
}

}

void reset() {
    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        gs::queue::vu().post(gs::queue::kVuReset, 0, 0, nullptr, 0);
        return;
    }
    std::memset(&g_vu1, 0, sizeof g_vu1);
    g_vu1.vf[0].w[3] = fp::kOne;
    g_top = g_itop = 0;
}

Vu0Regs &regs(int unit) { return unit ? g_vu1 : g_cpu.vu0; }

std::uint32_t tpc(int unit) { return regs(unit).tpc; }

void set_tops(std::uint32_t top, std::uint32_t itop) {
    g_top = top;
    g_itop = itop;
}

void run_fast(int n, std::uint32_t address);

std::unordered_map<std::uint64_t, ProgramFn> g_programs;
std::uint32_t g_generation[2] = {1, 1};

struct Candidate {
    std::vector<std::uint32_t> runs;
    std::vector<std::uint8_t> code;
    ProgramFn function = nullptr;
    ProgramFn jit = nullptr;
};

struct EntryCache {
    std::uint32_t generation = 0;
    ProgramFn function = nullptr;
    ProgramFn jit = nullptr;
    std::vector<Candidate> candidates;
    std::uint32_t next = 0;
};

bool candidate_matches(const Candidate &c) {
    const std::uint8_t *code = c.code.data();
    for (std::size_t i = 0; i < c.runs.size(); i += 2) {
        if (std::memcmp(g_vu1_code + c.runs[i], code, c.runs[i + 1]) != 0) return false;
        code += c.runs[i + 1];
    }
    return true;
}

Candidate make_candidate(std::vector<std::uint32_t> pcs) {
    std::sort(pcs.begin(), pcs.end());
    Candidate c;
    for (std::uint32_t pc : pcs) {
        if (!c.runs.empty() && c.runs[c.runs.size() - 2] + c.runs.back() == pc) c.runs.back() += 8;
        else {
            c.runs.push_back(pc);
            c.runs.push_back(8);
        }
        c.code.insert(c.code.end(), g_vu1_code + pc, g_vu1_code + pc + 8);
    }
    return c;
}
std::unordered_map<std::uint64_t, ProgramFn> g_jit_programs;
std::vector<std::uint32_t> *g_kick_log = nullptr;
std::int64_t *g_capture = nullptr;

ProgramFn jit_program(std::uint64_t key, std::uint32_t address) {
    const auto it = g_jit_programs.find(key);
    if (it != g_jit_programs.end()) return it->second;
    const ProgramFn fn = jit::compile(g_vu1_code, address & 16383u);
    g_jit_programs[key] = fn;
    return fn;
}
EntryCache g_entries[2048];

void code_changed(int unit) {
    g_scan[unit & 1] = true;
    ++g_generation[unit & 1];
}

void register_programs(const Program *programs, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i)
        if (programs[i].function) g_programs[programs[i].key] = programs[i].function;
}

void kick(int unit, std::uint32_t address) {
    if (g_kick_log && unit == 1) g_kick_log->push_back(address);
    Unit u = unit_of(unit);
    xgkick(u, address);
}

std::uint32_t top() { return g_top; }
std::uint32_t itop() { return g_itop; }

ProgramFn lookup(std::uint32_t address);

std::atomic<std::uint64_t> g_interpreted_calls{0};

std::uint64_t interpreted_calls() { return g_interpreted_calls.load(std::memory_order_relaxed); }

void check_jit(ProgramFn compiled, ProgramFn jit) {
    static std::uint64_t calls = 0, escapes = 0, mismatches = 0;
    static std::uint8_t before_mem[16384], jit_mem[16384];
    const Vu0Regs before = g_vu1;
    std::memcpy(before_mem, g_vu1_mem, sizeof before_mem);
    std::vector<std::uint32_t> jit_kicks, kicks;
    jit::g_dry = true;
    jit::g_escape = -1;
    jit::g_kicks = &jit_kicks;
    jit(g_vu1, g_vu1_mem);
    jit::g_dry = false;
    const std::int64_t jit_escape = jit::g_escape;
    const Vu0Regs after = g_vu1;
    std::memcpy(jit_mem, g_vu1_mem, sizeof jit_mem);
    g_vu1 = before;
    std::memcpy(g_vu1_mem, before_mem, sizeof before_mem);
    std::int64_t escape = -1;
    g_kick_log = &kicks;
    g_capture = &escape;
    compiled(g_vu1, g_vu1_mem);
    g_capture = nullptr;
    g_kick_log = nullptr;
    ++calls;
    if (escape >= 0) ++escapes;
    if (escape != jit_escape || std::memcmp(&after, &g_vu1, sizeof after) != 0 || std::memcmp(jit_mem, g_vu1_mem, sizeof jit_mem) != 0 || kicks != jit_kicks) {
        if (++mismatches <= 10) {
            int vf = -1;
            for (int i = 0; i < 32 && vf < 0; ++i)
                if (std::memcmp(&after.vf[i], &g_vu1.vf[i], 16) != 0) vf = i;
            std::fprintf(stderr, "[jit] mismatch at tpc 0x%X escape %lld/%lld: vf%d acc %d vi %d q/p/i/r %d flags %d mem %d kicks %zu/%zu\n", before.tpc, static_cast<long long>(jit_escape), static_cast<long long>(escape), vf,
                         std::memcmp(&after.acc, &g_vu1.acc, 16) != 0, std::memcmp(after.vi, g_vu1.vi, sizeof after.vi) != 0,
                         after.q != g_vu1.q || after.p != g_vu1.p || after.i != g_vu1.i || after.r != g_vu1.r,
                         after.mac != g_vu1.mac || after.status != g_vu1.status || after.clip != g_vu1.clip,
                         std::memcmp(jit_mem, g_vu1_mem, sizeof jit_mem) != 0, jit_kicks.size(), kicks.size());
        }
    }
    if (calls % 2000 == 0)
        std::fprintf(stderr, "[jit] checked %llu calls, %llu mismatches, %llu through the interpreter\n", static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(escapes));
    if (escape >= 0) interpret(1, static_cast<std::uint32_t>(escape));
}

void run_entry(int unit, std::uint32_t pc);

bool g_chaining = false;
std::int64_t g_chained = -1;

void interpret(int unit, std::uint32_t pc) {
    if (unit == 1 && g_capture) {
        *g_capture = pc;
        return;
    }
    if (unit != 1) {
        run_entry(unit, pc);
        return;
    }
    if (g_chaining) {
        g_chained = pc;
        return;
    }
    g_chaining = true;
    for (;;) {
        run_entry(1, pc);
        if (g_chained < 0) break;
        pc = static_cast<std::uint32_t>(g_chained);
        g_chained = -1;
    }
    g_chaining = false;
}

void run_entry(int unit, std::uint32_t pc) {
    static const bool interpret_only = std::getenv("PS2_VU_INTERPRET") != nullptr;
    static const bool count = std::getenv("PS2_VU_COUNT") != nullptr;
    static std::uint64_t compiled = 0, interpreted = 0;
    static std::unordered_map<std::uint32_t, std::uint64_t> misses;
    if (unit == 1 && !interpret_only) {
        if (const ProgramFn fn = lookup(pc)) {
            ++compiled;
            const ProgramFn jit = g_entries[(pc & 16383u) >> 3].jit;
            static std::uint64_t sampled = 0;
            if (jit && jit != fn && ++sampled % 32 == 0) check_jit(fn, jit);
            else fn(g_vu1, g_vu1_mem);
            return;
        }
    }
    if (unit == 1) g_interpreted_calls.fetch_add(1, std::memory_order_relaxed);
    if (count && unit == 1) {
        ++misses[pc];
        if (++interpreted % 5000 == 0) {
            std::fprintf(stderr, "[vu1] compiled %llu interpreted %llu:", static_cast<unsigned long long>(compiled), static_cast<unsigned long long>(interpreted));
            for (const auto &[a, n] : misses) std::fprintf(stderr, " %04X:%llu", a, static_cast<unsigned long long>(n));
            std::fprintf(stderr, "\n");
        }
    }
    run_fast(unit, pc);
}

ProgramFn lookup(std::uint32_t address) {
    EntryCache &e = g_entries[(address & 16383u) >> 3];
    if (e.generation == g_generation[1]) return e.function;
    e.generation = g_generation[1];
    for (const Candidate &c : e.candidates)
        if (candidate_matches(c)) {
            e.function = c.function;
            e.jit = c.jit;
            return e.function;
        }
    const std::vector<std::uint32_t> pcs = reachable(g_vu1_code, 16383u, address);
    const std::uint64_t key = program_key(g_vu1_code, pcs, address & 16383u);
    const auto it = g_programs.find(key);
    e.function = it == g_programs.end() ? nullptr : it->second;
    e.jit = nullptr;
    const int jit_mode = jit::mode();
    if (jit_mode == 2 || (jit_mode >= 1 && !e.function)) e.function = jit_program(key, address);
    else if (jit_mode == 3) e.jit = jit_program(key, address);
    Candidate c = make_candidate(pcs);
    c.function = e.function;
    c.jit = e.jit;
    if (e.candidates.size() < 8) e.candidates.push_back(std::move(c));
    else e.candidates[e.next++ % 8] = std::move(c);
    static const char *record = std::getenv("PS2_VU_RECORD");
    if (!e.function && (record || g_options.on_vu_program)) {
        char name[64];
        std::snprintf(name, sizeof name, "%016llX_%04X.bin", static_cast<unsigned long long>(key), address & 16383u);
        if (g_options.on_vu_program) {
            static std::unordered_set<std::string> reported;
            if (reported.insert(name).second) g_options.on_vu_program(name, g_vu1_code, 16384);
            return e.function;
        }
        const std::filesystem::path path = std::filesystem::path(record) / name;
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(record);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(g_vu1_code), 16384);
        }
    }
    return e.function;
}

// DIV, SQRT and RSQRT deliver Q, and the EFU instructions deliver P, a fixed number of instructions
// after they start. Until then Q and P still hold the previous result, and programs read them in
// that window on purpose: hand-scheduled code starts the divide for the next vertex and, a few
// instructions later, multiplies by the Q of the current one. Results therefore must not appear
// early. A result becomes visible to the instruction `latency` places after the one that started
// it; WAITQ and WAITP, the end of the program, and the start of another operation of the same
// kind (which the hardware stalls for) deliver it at once. Stalls from register hazards between
// ordinary instructions are not modelled.
struct Delayed {
    std::uint32_t q = 0, p = 0, q_left = 0, p_left = 0;

    void tick(Vu0Regs &r) {
        if (q_left && --q_left == 0) r.q = q;
        if (p_left && --p_left == 0) r.p = p;
    }

    void finish(Vu0Regs &r) {
        if (q_left) r.q = q;
        if (p_left) r.p = p;
        q_left = p_left = 0;
    }

    // After a lower instruction ran and wrote its result straight into r.q or r.p.
    void issued(Vu0Regs &r, std::uint32_t lo, std::uint32_t q_before, std::uint32_t p_before) {
        if ((lo >> 25) != 0x40u || (lo & 0x3Cu) != 0x3Cu) return;
        std::uint32_t wait_q = 0, wait_p = 0;
        switch ((((lo >> 6) & 31u) << 2) | (lo & 3u)) {
        case 0x38: case 0x39: wait_q = 7; break;                   // DIV, SQRT
        case 0x3A: wait_q = 13; break;                             // RSQRT
        case 0x3B: if (q_left) { r.q = q; q_left = 0; } break;     // WAITQ
        case 0x70: wait_p = 11; break;                             // ESADD
        case 0x71: case 0x72: case 0x79: wait_p = 18; break;       // ERSADD, ELENG, ERSQRT
        case 0x73: wait_p = 24; break;                             // ERLENG
        case 0x74: case 0x75: case 0x7D: wait_p = 54; break;       // EATANxy, EATANxz, EATAN
        case 0x76: case 0x78: case 0x7A: wait_p = 12; break;       // ESUM, ESQRT, ERCPR
        case 0x7B: if (p_left) { r.p = p; p_left = 0; } break;     // WAITP
        case 0x7C: wait_p = 29; break;                             // ESIN
        case 0x7E: wait_p = 44; break;                             // EEXP
        default: break;
        }
        if (wait_q) {
            const std::uint32_t fresh = r.q;
            r.q = q_left ? q : q_before;
            q = fresh;
            q_left = wait_q;
        }
        if (wait_p) {
            const std::uint32_t fresh = r.p;
            r.p = p_left ? p : p_before;
            p = fresh;
            p_left = wait_p;
        }
    }
};

void run_fast(int n, std::uint32_t address) {
    Unit u = unit_of(n);
    Vu0Regs &r = *u.regs;
    if (g_scan[n]) scan_flags(n, u.code, u.code_mask + 1);
    const bool flags = g_flags_used[n];
    Decoded *cache = g_decoded[n];
    vuops::FlagPipe pipe;
    pipe.start(r);
    u.pipe = &pipe;
    std::uint32_t pc = address & u.code_mask, npc = (pc + 8u) & u.code_mask;
    bool ending = false;
    UpperOut out;
    Delayed delayed;
    static std::uint64_t total = 0, calls = 0;
    static const bool count = std::getenv("PS2_VU_COUNT") != nullptr;
    static const bool record_jumps = std::getenv("PS2_VU_RECORD") != nullptr || static_cast<bool>(g_options.on_vu_program);
    if (count && ++calls % 20000 == 0) std::fprintf(stderr, "[vu1] %llu calls, %llu instructions\n", (unsigned long long)calls, (unsigned long long)total);
    for (std::uint32_t steps = 0; steps < 1'000'000; ++steps, ++total) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, u.code + pc, 4);
        std::memcpy(&hi, u.code + pc + 4, 4);
        Decoded &d = cache[pc >> 3];
        if (!d.valid || d.lo != lo || d.hi != hi) {
            d = decode_upper(lo, hi);
            if (g_scan[n]) scan_flags(n, u.code, u.code_mask + 1);
        }
        const std::uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & u.code_mask;
        ++pipe.t;
        delayed.tick(r);
        fast_upper(r, d, out);
        if (hi & 0x80000000u) {
            r.i = lo;
        } else {
            bool branch = false;
            std::uint32_t target = npc;
            const std::uint32_t q_before = r.q, p_before = r.p;
            lower(u, lo, current, target, branch);
            delayed.issued(r, lo, q_before, p_before);
            if (branch) npc = target;
            if (branch && record_jumps && n == 1 && ((lo >> 25) == 0x24u || (lo >> 25) == 0x25u)) lookup(target);
        }
        if (out.target > 0) {
            Reg128 &dst = out.target == kAcc ? r.acc : r.vf[out.target];
            for (int i = 0; i < 4; ++i)
                if (out.dest & (8u >> i)) dst.w[i] = out.value.w[i];
        }
        if (out.target >= 0 && out.flags && flags) {
            set_flags(r, out.value, out.dest);
            pipe.set_mac(r.mac, r.status);
        }
        if (out.has_clip) {
            r.clip = out.clip;
            pipe.set_clip(out.clip, pipe.t);
        }
        r.vf[0].d[0] = 0;
        r.vf[0].w[2] = 0;
        r.vf[0].w[3] = fp::kOne;
        r.vi[0] = 0;
        if (ending) {
            delayed.finish(r);
            r.tpc = pc;
            return;
        }
        if (hi & 0x40000000u) ending = true;
    }
    std::fprintf(stderr, "[vu%d] program at 0x%04X did not end\n", n, address);
}

// The exact loop: every instruction is fetched and decoded as it is met, and upper() computes each lane with
// the integer model (ps2/fpu.hpp). Returns the number of instructions it ran.
std::uint32_t run_exact(int n, std::uint32_t address, bool dry = false) {
    Unit u = unit_of(n);
    u.dry = dry;
    Vu0Regs &r = *u.regs;
    vuops::FlagPipe pipe;
    pipe.start(r);
    u.pipe = &pipe;
    std::uint32_t pc = address & u.code_mask, npc = (pc + 8u) & u.code_mask;
    bool ending = false;
    Delayed delayed;
    for (std::uint32_t steps = 0; steps < 1'000'000; ++steps) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, u.code + pc, 4);
        std::memcpy(&hi, u.code + pc + 4, 4);
        const std::uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & u.code_mask;
        ++pipe.t;
        delayed.tick(r);
        UpperOut out = upper(r, hi);
        bool branch = false;
        if (hi & 0x80000000u) {
            r.i = lo;
        } else {
            std::uint32_t target = npc;
            const std::uint32_t q_before = r.q, p_before = r.p;
            lower(u, lo, current, target, branch);
            delayed.issued(r, lo, q_before, p_before);
            if (branch) npc = target;
        }
        commit(r, out);
        if (out.target >= 0 && out.flags) pipe.set_mac(r.mac, r.status);
        if (out.has_clip) {
            r.clip = out.clip;
            pipe.set_clip(out.clip, pipe.t);
        }
        r.vf[0].d[0] = 0;
        r.vf[0].w[2] = 0;
        r.vf[0].w[3] = fp::kOne;
        r.vi[0] = 0;
        if (ending) {
            delayed.finish(r);
            r.tpc = pc;
            return steps + 1;
        }
        if (hi & 0x40000000u) ending = true;
    }
    std::fprintf(stderr, "[vu%d] program at 0x%04X did not end\n", n, address);
    return 1'000'000;
}

namespace {

using vuops::v4i;
using vuops::v4u;
using vuops::vload;
using vuops::vsel;
using vuops::vsplat;
using vuops::vstore;

Vu0Mode g_vu0_mode = Vu0Mode::Fast;
bool g_vu0_mode_set = false;
void (*g_vu0_tamper)(Vu0Regs &, std::uint8_t *) = nullptr;

// What the fast loop does for the upper of an instruction pair, how it puts the result away once the lower
// has run, and which lowers it runs itself. V: the second operand is ft, lane by lane. S: it is one value
// for every lane (one lane of ft, Q or I). One: that value is the w of vf0, which is 1.0, so the product
// is the first operand. 1: the instruction has one lane, which the model computes sooner than four lanes
// of host floats are set up.
enum Vu0Upper : std::uint8_t {
    kUpNone, kUpAddV, kUpAddS, kUpSubV, kUpSubS, kUpMulV, kUpMulS, kUpMaddV, kUpMaddS, kUpMsubV, kUpMsubS,
    kUpMaxV, kUpMaxS, kUpMinV, kUpMinS, kUpAbs, kUpFtoi, kUpItof, kUpClip, kUpOpmula, kUpOpmsub,
    kUpMulOne, kUpMaddOne, kUpMsubOne, kUpAdd1, kUpSub1, kUpMul1, kUpMadd1, kUpMsub1,
};
enum Vu0Commit : std::uint8_t { kPutNothing, kPutLanesFlags, kPutLanes, kPutFlags, kPutLane, kPutClip };
enum Vu0Lower : std::uint8_t {
    kLoGeneric, kLoNone, kLoI, kLoIaddiu, kLoIsubiu, kLoIadd, kLoIand, kLoIor, kLoLq, kLoSq, kLoFmand, kLoFmor, kLoB, kLoIbeq, kLoIbne, kLoIbgtz, kLoIblez,
};

// One instruction pair as the fast loop runs it, decoded once per code image.
struct Vu0Op {
    std::uint32_t lo, hi;
    std::uint8_t upper, commit, lower;          // Vu0Upper, Vu0Commit, Vu0Lower
    std::uint8_t dest, target, fs, ft;          // the upper's lanes and registers; target kAcc is ACC
    std::uint8_t frac;                          // FTOI and ITOF: fraction bits
    std::uint8_t lane;                          // the lane of a one-lane form, 0 for x to 3 for w
    bool end;                                   // the E bit
    std::uint8_t it, is, id, ldest, lft, lfs;   // the lower's fields
    std::uint16_t second;                       // where an S form's second operand is, as an offset into Vu0Regs
    std::uint16_t branch;                       // where a relative branch goes
    std::uint32_t imm;                          // the lower's immediate: 11 bits with their sign (LQ, SQ) or 15 bits (IADDIU, ISUBIU)
    std::uint32_t mac_lanes;                    // the upper's lanes in each group of four MAC bits
};

Vu0Op decode_vu0(std::uint32_t lo, std::uint32_t hi, std::uint32_t pc) {
    Vu0Op op{};
    op.lo = lo;
    op.hi = hi;
    op.end = (hi & 0x40000000u) != 0;
    const Decoded d = decode_upper(lo, hi);
    op.dest = d.dest;
    op.target = d.target;
    op.fs = d.fs;
    op.ft = d.ft;
    op.frac = d.frac;
    op.mac_lanes = d.dest * 0x1111u;
    switch (d.source) {
    case kVec: break;
    case kBc: op.second = static_cast<std::uint16_t>(offsetof(Vu0Regs, vf) + d.ft * 16u + d.bc * 4u); break;
    case kQ: op.second = static_cast<std::uint16_t>(offsetof(Vu0Regs, q)); break;
    case kI: op.second = static_cast<std::uint16_t>(offsetof(Vu0Regs, i)); break;
    }
    const bool one = d.source != kVec;
    switch (d.kind) {
    case kNop: op.upper = kUpNone; break;
    case kAdd: op.upper = one ? kUpAddS : kUpAddV; break;
    case kSub: op.upper = one ? kUpSubS : kUpSubV; break;
    case kMul: op.upper = one ? kUpMulS : kUpMulV; break;
    case kMadd: op.upper = one ? kUpMaddS : kUpMaddV; break;
    case kMsub: op.upper = one ? kUpMsubS : kUpMsubV; break;
    case kMax: op.upper = one ? kUpMaxS : kUpMaxV; break;
    case kMin: op.upper = one ? kUpMinS : kUpMinV; break;
    case kAbs: op.upper = kUpAbs; break;
    case kFtoi: op.upper = kUpFtoi; break;
    case kItof: op.upper = kUpItof; break;
    case kClip: op.upper = kUpClip; break;
    case kOpmula: op.upper = kUpOpmula; break;
    case kOpmsub: op.upper = kUpOpmsub; break;
    }
    const bool arithmetic = d.kind == kAdd || d.kind == kSub || d.kind == kMul || d.kind == kMadd || d.kind == kMsub;
    if (arithmetic && std::popcount(static_cast<unsigned>(d.dest)) == 1) {
        op.lane = static_cast<std::uint8_t>(std::countl_zero(static_cast<std::uint8_t>(d.dest)) - 4);
        if (d.source == kVec) op.second = static_cast<std::uint16_t>(offsetof(Vu0Regs, vf) + d.ft * 16u + op.lane * 4u);
        op.upper = d.kind == kAdd ? kUpAdd1 : d.kind == kSub ? kUpSub1 : d.kind == kMul ? kUpMul1 : d.kind == kMadd ? kUpMadd1 : kUpMsub1;
    } else if (d.source == kBc && d.ft == 0 && d.bc == 3) {
        // vf0 is (0, 0, 0, 1.0) when the loop starts and nothing writes it, and fp::mul(s, 1.0) is s with
        // a denormal flushed, for every s: no multiply is needed.
        if (d.kind == kMul) op.upper = kUpMulOne;
        if (d.kind == kMadd) op.upper = kUpMaddOne;
        if (d.kind == kMsub) op.upper = kUpMsubOne;
    }
    // An upper that sets no flags and writes vf0 leaves nothing behind.
    if (op.upper != kUpClip && !d.flags && d.target == 0) op.upper = kUpNone;
    op.commit = op.upper == kUpNone ? kPutNothing : op.upper == kUpClip ? kPutClip : op.upper >= kUpAdd1 ? kPutLane : !d.flags ? kPutLanes : d.target ? kPutLanesFlags : kPutFlags;

    // The lowers the loop runs itself, transcribed from lower(): the common ones that touch neither Q nor
    // P. Every other one goes through lower().
    op.it = (lo >> 16) & 15u;
    op.is = (lo >> 11) & 15u;
    op.id = (lo >> 6) & 15u;
    op.ldest = (lo >> 21) & 15u;
    op.lft = (lo >> 16) & 31u;
    op.lfs = (lo >> 11) & 31u;
    const std::int32_t imm11 = static_cast<std::int32_t>(lo << 21) >> 21;
    op.imm = static_cast<std::uint32_t>(imm11);
    op.branch = static_cast<std::uint16_t>((pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & 4095u);
    op.lower = kLoGeneric;
    if (hi & 0x80000000u) {
        op.lower = kLoI;
    } else if ((lo >> 25) == 0x40u) {
        const unsigned funct = lo & 63u;
        if (funct == 0x30) op.lower = kLoIadd;
        else if (funct == 0x34) op.lower = kLoIand;
        else if (funct == 0x35) op.lower = kLoIor;
        // MOVE with nothing to move: what an instruction with only an upper has as its lower.
        else if (funct >= 0x3C && ((lo >> 6) & 31u) * 4u + (funct & 3u) == 0x30 && (!op.lft || !op.ldest || op.lft == op.lfs)) op.lower = kLoNone;
    } else {
        switch (lo >> 25) {
        case 0x00: op.lower = kLoLq; break;
        case 0x01: op.lower = kLoSq; break;
        case 0x08: op.lower = kLoIaddiu; op.imm = ((lo >> 10) & 0x7800u) | (lo & 0x7FFu); break;
        case 0x09: op.lower = kLoIsubiu; op.imm = ((lo >> 10) & 0x7800u) | (lo & 0x7FFu); break;
        case 0x1A: op.lower = kLoFmand; break;
        case 0x1B: op.lower = kLoFmor; break;
        case 0x20: op.lower = kLoB; break;
        case 0x28: op.lower = kLoIbeq; break;
        case 0x29: op.lower = kLoIbne; break;
        case 0x2D: op.lower = kLoIbgtz; break;
        case 0x2E: op.lower = kLoIblez; break;
        default: break;
        }
    }
    return op;
}

// The code VU0 holds, decoded. VIF0 swaps a few programs in and out many times a frame, so the decoded
// forms of the last 16 code images are kept, most recently used first. An image is looked for, by
// comparing the code, only after code_changed(0) said the code is another.
struct Vu0Image {
    std::uint8_t code[4096];
    Vu0Op ops[512];
};

std::vector<std::unique_ptr<Vu0Image>> g_vu0_images;
Vu0Image *g_vu0_image = nullptr;
std::uint32_t g_vu0_image_generation = 0;

__attribute__((noinline)) Vu0Image &find_image() {
    ++g_vu0.lookups;
    g_vu0_image_generation = g_generation[0];
    for (std::size_t i = 0; i < g_vu0_images.size(); ++i)
        if (std::memcmp(g_vu0_images[i]->code, g_vu0_code, sizeof g_vu0_code) == 0) {
            std::rotate(g_vu0_images.begin(), g_vu0_images.begin() + i, g_vu0_images.begin() + i + 1);
            return *(g_vu0_image = g_vu0_images[0].get());
        }
    // Not kept: a new image, or the one used longest ago, goes to the front and is decoded.
    if (g_vu0_images.size() < 16) g_vu0_images.push_back(std::make_unique<Vu0Image>());
    std::rotate(g_vu0_images.begin(), g_vu0_images.end() - 1, g_vu0_images.end());
    Vu0Image &image = *g_vu0_images[0];
    std::memcpy(image.code, g_vu0_code, sizeof image.code);
    for (std::uint32_t pc = 0; pc < sizeof image.code; pc += 8) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, image.code + pc, 4);
        std::memcpy(&hi, image.code + pc + 4, 4);
        image.ops[pc >> 3] = decode_vu0(lo, hi, pc);
    }
    ++g_vu0.images;
    return *(g_vu0_image = &image);
}

PS2_VU_INLINE Vu0Image &current_image() {
    if (g_vu0_image && g_vu0_image_generation == g_generation[0]) return *g_vu0_image;
    return find_image();
}

// Check modes: the image the fast loop holds as current is no longer the code in memory. Some writer of
// VU0 code did not call code_changed(0), and the fast loop ran what was there before.
bool image_stale() {
    return g_vu0_image && g_vu0_image_generation == g_generation[0] && std::memcmp(g_vu0_image->code, g_vu0_code, sizeof g_vu0_code) != 0;
}

alignas(16) const std::uint32_t kLaneMasks[16][4] = {
    {0, 0, 0, 0}, {0, 0, 0, ~0u}, {0, 0, ~0u, 0}, {0, 0, ~0u, ~0u}, {0, ~0u, 0, 0}, {0, ~0u, 0, ~0u}, {0, ~0u, ~0u, 0}, {0, ~0u, ~0u, ~0u},
    {~0u, 0, 0, 0}, {~0u, 0, 0, ~0u}, {~0u, 0, ~0u, 0}, {~0u, 0, ~0u, ~0u}, {~0u, ~0u, 0, 0}, {~0u, ~0u, 0, ~0u}, {~0u, ~0u, ~0u, 0}, {~0u, ~0u, ~0u, ~0u},
};

// All ones in the lanes of a dest field (x in bit 3 down to w in bit 0).
PS2_VU_INLINE v4u lane_mask(unsigned dest) {
    v4u m;
    std::memcpy(&m, kLaneMasks[dest], 16);
    return m;
}

PS2_VU_INLINE v4u load_qword(const std::uint8_t *q) {
    v4u v;
    std::memcpy(&v, q, 16);
    return v;
}

PS2_VU_INLINE std::uint32_t second_operand(const Vu0Regs &r, const Vu0Op &op) {
    std::uint32_t b;
    std::memcpy(&b, reinterpret_cast<const std::uint8_t *>(&r) + op.second, 4);
    return b;
}

// The exact loop's routine for one upper: for an instruction with a lane that host floats cannot compute.
__attribute__((noinline)) v4u model_upper(Vu0Regs &r, const Vu0Op &op) {
    ++g_vu0.slow;
    return vload(upper(r, op.hi).value);
}

// set_flags() from its MAC flags on: the status bits and their sticky copies.
PS2_VU_INLINE void put_flags(Vu0Regs &r, std::uint32_t mac) {
    r.mac = mac;
    std::uint32_t status = r.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0x0F00u) status |= 0x4u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}

// The fast loop: the exact loop's steps in the exact loop's order, over instructions decoded once per code
// image, with the arithmetic on four lanes of host floats (ps2/vu_exact.hpp gives the model's bits or says
// which lanes it cannot compute). Flags, Q and P go through the same FlagPipe and Delayed, and every lower
// through the same lower(). Returns the number of instructions it ran.
std::uint32_t run_vu0(std::uint32_t address, bool dry) {
    Unit u = unit_of(0);
    u.dry = dry;
    Vu0Regs &r = *u.regs;
    // No instruction writes vf0 or vi0, so the exact loop's setting them after every instruction matters
    // only when a call starts with them wrong: such a call goes to the exact loop.
    if (r.vf[0].d[0] != 0 || r.vf[0].w[2] != 0 || r.vf[0].w[3] != fp::kOne || r.vi[0] != 0) {
        ++g_vu0.guarded;
        return run_exact(0, address, dry);
    }
    const Vu0Op *ops = current_image().ops;
    vuops::FlagPipe pipe;
    pipe.start(r);
    u.pipe = &pipe;
    std::uint32_t pc = address & u.code_mask, npc = (pc + 8u) & u.code_mask;
    bool ending = false;
    Delayed delayed;
    for (std::uint32_t steps = 0; steps < 1'000'000; ++steps) {
        const Vu0Op &op = ops[pc >> 3];
        const std::uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & u.code_mask;
        ++pipe.t;
        delayed.tick(r);
        // The upper's result, from the registers as they are before the lower runs.
        const v4u lanes = lane_mask(op.dest);
        v4u value = {0, 0, 0, 0}, bad = {0, 0, 0, 0};
        std::uint32_t one = 0, clip = 0;
        switch (op.upper) {
        case kUpNone: break;
        case kUpAdd1: one = fp::add(r.vf[op.fs].w[op.lane], second_operand(r, op)); break;
        case kUpSub1: one = fp::sub(r.vf[op.fs].w[op.lane], second_operand(r, op)); break;
        case kUpMul1: one = fp::mul(r.vf[op.fs].w[op.lane], second_operand(r, op)); break;
        case kUpMadd1: one = fp::add(r.acc.w[op.lane], fp::mul(r.vf[op.fs].w[op.lane], second_operand(r, op))); break;
        case kUpMsub1: one = fp::sub(r.acc.w[op.lane], fp::mul(r.vf[op.fs].w[op.lane], second_operand(r, op))); break;
        case kUpMulOne: value = vuexact::vflush(vload(r.vf[op.fs])); break;
        case kUpMaddOne: value = vuexact::add(vload(r.acc), vload(r.vf[op.fs]), bad); break;
        case kUpMsubOne: value = vuexact::sub(vload(r.acc), vload(r.vf[op.fs]), bad); break;
        case kUpAddV: value = vuexact::add(vload(r.vf[op.fs]), vload(r.vf[op.ft]), bad); break;
        case kUpAddS: value = vuexact::add(vload(r.vf[op.fs]), vsplat(second_operand(r, op)), bad); break;
        case kUpSubV: value = vuexact::sub(vload(r.vf[op.fs]), vload(r.vf[op.ft]), bad); break;
        case kUpSubS: value = vuexact::sub(vload(r.vf[op.fs]), vsplat(second_operand(r, op)), bad); break;
        case kUpMulV: value = vuexact::mul(vload(r.vf[op.fs]), vload(r.vf[op.ft]), bad); break;
        case kUpMulS: value = vuexact::mul1(vload(r.vf[op.fs]), second_operand(r, op), bad); break;
        case kUpMaddV: value = vuexact::madd(vload(r.acc), vload(r.vf[op.fs]), vload(r.vf[op.ft]), bad); break;
        case kUpMaddS: value = vuexact::madd1(vload(r.acc), vload(r.vf[op.fs]), second_operand(r, op), bad); break;
        case kUpMsubV: value = vuexact::msub(vload(r.acc), vload(r.vf[op.fs]), vload(r.vf[op.ft]), bad); break;
        case kUpMsubS: value = vuexact::msub1(vload(r.acc), vload(r.vf[op.fs]), second_operand(r, op), bad); break;
        case kUpMaxV: value = vuops::vmax(vload(r.vf[op.fs]), vload(r.vf[op.ft])); break;
        case kUpMaxS: value = vuops::vmax(vload(r.vf[op.fs]), vsplat(second_operand(r, op))); break;
        case kUpMinV: value = vuops::vmin(vload(r.vf[op.fs]), vload(r.vf[op.ft])); break;
        case kUpMinS: value = vuops::vmin(vload(r.vf[op.fs]), vsplat(second_operand(r, op))); break;
        case kUpAbs: value = vuops::vabs(vload(r.vf[op.fs])); break;
        case kUpFtoi: {
            Reg128 o;
            for (int i = 0; i < 4; ++i) o.w[i] = fp::to_fixed(r.vf[op.fs].w[i], op.frac);
            value = vload(o);
            break;
        }
        case kUpItof: {
            Reg128 o;
            for (int i = 0; i < 4; ++i) o.w[i] = fp::from_fixed(r.vf[op.fs].sw[i], op.frac);
            value = vload(o);
            break;
        }
        case kUpClip: clip = vuops::clip_value(r, r.vf[op.fs], r.vf[op.ft].w[3]); break;
        case kUpOpmula: {
            const v4u s = vload(r.vf[op.fs]), t = vload(r.vf[op.ft]);
            value = vuexact::mul(__builtin_shufflevector(s, s, 1, 2, 0, 3), __builtin_shufflevector(t, t, 2, 0, 1, 3), bad);
            break;
        }
        case kUpOpmsub: {
            const v4u s = vload(r.vf[op.fs]), t = vload(r.vf[op.ft]);
            value = vuexact::msub(vload(r.acc), __builtin_shufflevector(s, s, 1, 2, 0, 3), __builtin_shufflevector(t, t, 2, 0, 1, 3), bad);
            break;
        }
        }
        if (vuexact::vbits((v4i)(bad & lanes))) value = model_upper(r, op);
        switch (op.lower) {
        case kLoGeneric: {
            bool branch = false;
            std::uint32_t target = npc;
            const std::uint32_t q_before = r.q, p_before = r.p;
            lower(u, op.lo, current, target, branch);
            delayed.issued(r, op.lo, q_before, p_before);
            if (branch) npc = target;
            break;
        }
        case kLoNone: break;
        case kLoI: r.i = op.lo; break;
        case kLoIaddiu: store_vi(r, op.it, r.vi[op.is] + op.imm); break;
        case kLoIsubiu: store_vi(r, op.it, r.vi[op.is] - op.imm); break;
        case kLoIadd: store_vi(r, op.id, r.vi[op.is] + r.vi[op.it]); break;
        case kLoIand: store_vi(r, op.id, r.vi[op.is] & r.vi[op.it]); break;
        case kLoIor: store_vi(r, op.id, r.vi[op.is] | r.vi[op.it]); break;
        case kLoLq: {
            const std::uint8_t *q = qword(u, r.vi[op.is] + op.imm);
            if (op.lft) vstore(r.vf[op.lft], vsel(vload(r.vf[op.lft]), load_qword(q), lane_mask(op.ldest)));
            break;
        }
        case kLoSq: {
            std::uint8_t *q = qword(u, r.vi[op.it] + op.imm);
            const v4u merged = vsel(load_qword(q), vload(r.vf[op.lfs]), lane_mask(op.ldest));
            std::memcpy(q, &merged, 16);
            break;
        }
        case kLoFmand: store_vi(r, op.it, pipe.mac() & r.vi[op.is]); break;
        case kLoFmor: store_vi(r, op.it, (pipe.mac() | r.vi[op.is]) & 0xFFFFu); break;
        case kLoB: npc = op.branch; break;
        case kLoIbeq: if (r.vi[op.is] == r.vi[op.it]) npc = op.branch; break;
        case kLoIbne: if (r.vi[op.is] != r.vi[op.it]) npc = op.branch; break;
        case kLoIbgtz: if (static_cast<std::int16_t>(r.vi[op.is]) > 0) npc = op.branch; break;
        case kLoIblez: if (static_cast<std::int16_t>(r.vi[op.is]) <= 0) npc = op.branch; break;
        }
        // commit() and the flag pushes of the exact loop, by what the upper leaves behind.
        switch (op.commit) {
        case kPutNothing: break;
        case kPutLanesFlags: {
            Reg128 &dst = op.target == kAcc ? r.acc : r.vf[op.target];
            vstore(dst, vsel(vload(dst), value, lanes));
            put_flags(r, vuexact::mac(value, op.mac_lanes));
            pipe.set_mac(r.mac, r.status);
            break;
        }
        case kPutLanes: {
            Reg128 &dst = r.vf[op.target];
            vstore(dst, vsel(vload(dst), value, lanes));
            break;
        }
        case kPutFlags:
            put_flags(r, vuexact::mac(value, op.mac_lanes));
            pipe.set_mac(r.mac, r.status);
            break;
        case kPutLane: {
            if (op.target) (op.target == kAcc ? r.acc : r.vf[op.target]).w[op.lane] = one;
            Reg128 result{};
            result.w[op.lane] = one;
            set_flags(r, result, op.dest);
            pipe.set_mac(r.mac, r.status);
            break;
        }
        case kPutClip:
            r.clip = clip;
            pipe.set_clip(clip, pipe.t);
            break;
        }
        if (ending) {
            delayed.finish(r);
            r.tpc = pc;
            return steps + 1;
        }
        ending = op.end;
    }
    std::fprintf(stderr, "[vu0] program at 0x%04X did not end\n", address);
    return 1'000'000;
}

// The fast loop's arithmetic gives the model's bits only on a host that rounds to nearest and keeps
// denormals (ps2/vu_exact.hpp). A few sums and products against the model show a thread that was set up
// otherwise. The operands are read through volatile, so the compiler cannot work the answers out itself.
bool host_floats_fit() {
    static volatile std::uint32_t operands[8] = {0x00800000u, 0x00BFFFFFu, 0x73C00000u, 0xFF7FFFFFu, 0x3FAAAAABu, 0x40490FDBu, 0x3F800001u, 0xB3800000u};
    Reg128 a, b;
    for (int i = 0; i < 4; ++i) {
        a.w[i] = operands[i * 2];
        b.w[i] = operands[i * 2 + 1];
    }
    v4u bad_sum, bad_difference, bad_product, bad_both;
    const v4u sum = vuexact::add(vload(a), vload(b), bad_sum), difference = vuexact::sub(vload(a), vload(b), bad_difference);
    const v4u product = vuexact::mul(vload(a), vload(b), bad_product), both = vuexact::madd(vload(b), vload(a), vload(b), bad_both);
    for (int i = 0; i < 4; ++i) {
        if (!bad_sum[i] && sum[i] != fp::add(a.w[i], b.w[i])) return false;
        if (!bad_difference[i] && difference[i] != fp::sub(a.w[i], b.w[i])) return false;
        if (!bad_product[i] && product[i] != fp::mul(a.w[i], b.w[i])) return false;
        if (!bad_both[i] && both[i] != fp::add(b.w[i], fp::mul(a.w[i], b.w[i]))) return false;
    }
    return true;
}

// What a program can leave behind in the registers: the struct up to tpc (it ends in padding).
constexpr std::size_t kVu0RegBytes = offsetof(Vu0Regs, tpc) + sizeof(std::uint32_t);

// The first register in which the two loops' outcomes differ, with both values.
void first_difference(const Vu0Regs &fast, const Vu0Regs &exact, char *text, std::size_t size) {
    const auto vector = [&](const char *name, const Reg128 &a, const Reg128 &b) {
        std::snprintf(text, size, "%s fast %08X %08X %08X %08X exact %08X %08X %08X %08X", name, a.w[0], a.w[1], a.w[2], a.w[3], b.w[0], b.w[1], b.w[2], b.w[3]);
    };
    for (int i = 0; i < 32; ++i)
        if (std::memcmp(&fast.vf[i], &exact.vf[i], 16) != 0) {
            char name[8];
            std::snprintf(name, sizeof name, "vf%d", i);
            return vector(name, fast.vf[i], exact.vf[i]);
        }
    if (std::memcmp(&fast.acc, &exact.acc, 16) != 0) return vector("acc", fast.acc, exact.acc);
    for (int i = 0; i < 16; ++i)
        if (fast.vi[i] != exact.vi[i]) {
            std::snprintf(text, size, "vi%d fast %04X exact %04X", i, fast.vi[i], exact.vi[i]);
            return;
        }
    const struct {
        const char *name;
        std::uint32_t fast, exact;
    } words[] = {{"q", fast.q, exact.q}, {"p", fast.p, exact.p}, {"i", fast.i, exact.i}, {"r", fast.r, exact.r}, {"mac", fast.mac, exact.mac},
                 {"status", fast.status, exact.status}, {"clip", fast.clip, exact.clip}, {"cmsar0", fast.cmsar0, exact.cmsar0}, {"tpc", fast.tpc, exact.tpc}};
    for (const auto &w : words)
        if (w.fast != w.exact) {
            std::snprintf(text, size, "%s fast %08X exact %08X", w.name, w.fast, w.exact);
            return;
        }
    std::snprintf(text, size, "registers equal");
}

// Both loops on one call, from the same state, and everything a program can leave behind compared: the
// registers, the data memory, the number of instructions, and the EE's cycle budget, which neither loop may
// touch. The fast loop goes first and sends no XGKICK; the exact loop then runs the call for real, and its
// outcome stays unless the mode keeps the fast loop's.
void check_vu0(std::uint32_t address) {
    static std::uint8_t mem_before[4096], mem_fast[4096];
    const Vu0Regs before = g_cpu.vu0;
    std::memcpy(mem_before, g_vu0_mem, sizeof mem_before);
    const std::int32_t budget = g_cpu.budget;
    const std::uint32_t fast_steps = run_vu0(address, true);
    const bool stale = image_stale();
    const std::int32_t fast_budget = g_cpu.budget;
    Vu0Regs fast = g_cpu.vu0;
    std::memcpy(mem_fast, g_vu0_mem, sizeof mem_fast);
    if (g_vu0_tamper) g_vu0_tamper(fast, mem_fast);
    g_cpu.vu0 = before;
    std::memcpy(g_vu0_mem, mem_before, sizeof mem_before);
    const std::uint32_t steps = run_exact(0, address);
    g_vu0.steps += steps;
    ++g_vu0.compared;
    const bool regs_differ = std::memcmp(&fast, &g_cpu.vu0, kVu0RegBytes) != 0;
    const bool mem_differs = std::memcmp(mem_fast, g_vu0_mem, sizeof mem_fast) != 0;
    const bool charged = fast_budget != budget || g_cpu.budget != budget;
    if (stale) ++g_vu0.stale;
    if (regs_differ || mem_differs || fast_steps != steps || charged || stale) {
        if (++g_vu0.differ <= 8) {
            char regs[160];
            first_difference(fast, g_cpu.vu0, regs, sizeof regs);
            std::size_t at = 0;
            while (at < sizeof mem_fast && mem_fast[at] == g_vu0_mem[at]) ++at;
            char memory[32] = "memory equal";
            if (mem_differs) std::snprintf(memory, sizeof memory, "memory differs at 0x%03zX", at);
            std::fprintf(stderr, "[vu0-check] call %llu entry 0x%04X, %u instructions (fast loop %u): %s; %s%s%s\n", static_cast<unsigned long long>(g_vu0.compared),
                         address & 4095u, steps, fast_steps, regs, memory, charged ? "; the cycle budget moved" : "", stale ? "; the decoded image is not the code in memory" : "");
        }
    }
    if (g_vu0.compared % 1'000'000 == 0)
        std::fprintf(stderr, "[vu0-check] %llu calls compared, %llu differ\n", static_cast<unsigned long long>(g_vu0.compared), static_cast<unsigned long long>(g_vu0.differ));
    if (g_vu0_mode == Vu0Mode::CheckLive) {
        g_cpu.vu0 = fast;
        std::memcpy(g_vu0_mem, mem_fast, sizeof mem_fast);
    }
}

Vu0Mode vu0_mode_from_environment() {
    if (const char *check = std::getenv("PS2_VU0_CHECK")) return std::strcmp(check, "live") == 0 ? Vu0Mode::CheckLive : Vu0Mode::Check;
    if (std::getenv("PS2_VU_EXACT") || std::getenv("PS2_VU0_EXACT")) return Vu0Mode::Exact;
    return Vu0Mode::Fast;
}

}

void set_vu0_mode(Vu0Mode mode) {
    if (mode != Vu0Mode::Exact && !host_floats_fit()) {
        std::fprintf(stderr, "[vu0] this thread's floats do not round to nearest with denormals kept: VU0 runs its exact loop\n");
        mode = Vu0Mode::Exact;
    }
    g_vu0_mode = mode;
    g_vu0_mode_set = true;
}

Vu0Mode vu0_mode() {
    if (!g_vu0_mode_set) set_vu0_mode(vu0_mode_from_environment());
    return g_vu0_mode;
}

const Vu0Counters &vu0_counters() { return g_vu0; }

void set_vu0_tamper(void (*tamper)(Vu0Regs &regs, std::uint8_t *mem)) { g_vu0_tamper = tamper; }

void stats() {
    if (!g_vu0.calls) return;
    static const char *const kNames[] = {"fast", "exact", "check", "check, fast outcome kept"};
    std::fprintf(stderr, "[vu0] mode %s: %llu calls, %llu instructions; fast loop: %llu through the model, %llu calls left to the exact loop, %llu code images, %llu image lookups\n",
                 kNames[static_cast<int>(g_vu0_mode)], static_cast<unsigned long long>(g_vu0.calls), static_cast<unsigned long long>(g_vu0.steps),
                 static_cast<unsigned long long>(g_vu0.slow), static_cast<unsigned long long>(g_vu0.guarded), static_cast<unsigned long long>(g_vu0.images),
                 static_cast<unsigned long long>(g_vu0.lookups));
    if (g_vu0_mode == Vu0Mode::Check || g_vu0_mode == Vu0Mode::CheckLive)
        std::fprintf(stderr, "[vu0-check] %llu calls compared, %llu differ (%llu with a stale image), %llu XGKICK skipped in the fast loop\n", static_cast<unsigned long long>(g_vu0.compared),
                     static_cast<unsigned long long>(g_vu0.differ), static_cast<unsigned long long>(g_vu0.stale), static_cast<unsigned long long>(g_vu0.kicks_skipped));
}

void run(int n, std::uint32_t address) {
    if (n == 1) {
        static const bool exact = std::getenv("PS2_VU_EXACT") != nullptr;
        if (exact) run_exact(1, address);
        else interpret(1, address);
        return;
    }
    ++g_vu0.calls;
    switch (vu0_mode()) {
    case Vu0Mode::Fast: g_vu0.steps += run_vu0(address, false); break;
    case Vu0Mode::Exact: g_vu0.steps += run_exact(0, address); break;
    default: check_vu0(address); break;
    }
}

}

namespace ps2 {

void vcallms(Cpu &, std::uint32_t address) { vu::run(0, address); }

}

__attribute__((weak)) void register_generated_vu() {}

namespace ps2::vu {

void state(ps2::state::Stream &s) {
    s.tag("vu");
    s.pod(g_vu1);
    s.pod(g_top);
    s.pod(g_itop);
    s.bytes(g_vu0_code, sizeof g_vu0_code);
    s.bytes(g_vu0_mem, sizeof g_vu0_mem);
    s.bytes(g_vu1_code, sizeof g_vu1_code);
    s.bytes(g_vu1_mem, sizeof g_vu1_mem);
    if (s.loading()) {
        code_changed(0);
        code_changed(1);
    }
}

}
