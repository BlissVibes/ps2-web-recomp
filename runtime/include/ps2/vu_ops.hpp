#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "vu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#define PS2_VU_INLINE inline __attribute__((always_inline))

namespace ps2::vu {

void kick(int unit, std::uint32_t address);
std::uint32_t top();
std::uint32_t itop();
void interpret(int unit, std::uint32_t pc);

using ProgramFn = void (*)(Vu0Regs &r, std::uint8_t *mem);

struct Program {
    std::uint64_t key;
    ProgramFn function;
};

void register_programs(const Program *programs, std::uint32_t count);

inline std::uint32_t lower_major(std::uint32_t lo) { return lo >> 25; }
inline bool is_branch(std::uint32_t lo) {
    const std::uint32_t op = lower_major(lo);
    return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2F && op != 0x2A && op != 0x2B);
}
inline bool is_jump_register(std::uint32_t lo) { return lower_major(lo) == 0x24 || lower_major(lo) == 0x25; }
inline bool is_unconditional(std::uint32_t lo) { return lower_major(lo) == 0x20 || lower_major(lo) == 0x21; }
inline bool links(std::uint32_t lo) { return lower_major(lo) == 0x21 || lower_major(lo) == 0x25; }
inline std::uint32_t branch_target(std::uint32_t lo, std::uint32_t pc, std::uint32_t mask) {
    const std::int32_t imm11 = static_cast<std::int32_t>(lo << 21) >> 21;
    return (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & mask;
}

// Walks the instructions reachable from an entry; both the runtime and vurecomp key programs by this set.
inline std::vector<std::uint32_t> reachable(const std::uint8_t *code, std::uint32_t mask, std::uint32_t entry) {
    std::vector<std::uint8_t> seen((mask + 1) / 8, 0);
    std::vector<std::uint32_t> work{entry & mask}, out;
    while (!work.empty()) {
        std::uint32_t pc = work.back();
        work.pop_back();
        int stop_after = -1;
        for (int guard = 0; guard < 4096; ++guard) {
            if (seen[pc >> 3]) break;
            seen[pc >> 3] = 1;
            out.push_back(pc);
            if (stop_after == 0) break;
            if (stop_after > 0) --stop_after;
            std::uint32_t lo, hi;
            std::memcpy(&lo, code + pc, 4);
            std::memcpy(&hi, code + pc + 4, 4);
            if (hi & 0x40000000u) stop_after = 0;
            if (!(hi & 0x80000000u) && is_branch(lo)) {
                if (!is_jump_register(lo)) work.push_back(branch_target(lo, pc, mask));
                if (links(lo)) work.push_back((pc + 16u) & mask);
                if (is_unconditional(lo) || is_jump_register(lo)) stop_after = 0;
            }
            pc = (pc + 8u) & mask;
        }
    }
    std::vector<std::uint32_t> sorted(out);
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

inline std::uint64_t program_key(const std::uint8_t *code, const std::vector<std::uint32_t> &pcs, std::uint32_t entry) {
    std::uint64_t h = 0xCBF29CE484222325ull ^ entry;
    const auto mix = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    };
    for (std::uint32_t pc : pcs) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + pc, 4);
        std::memcpy(&hi, code + pc + 4, 4);
        mix(pc);
        mix(lo);
        mix(hi);
    }
    return h;
}

}

namespace ps2::vuops {

PS2_VU_INLINE float fin(std::uint32_t x) {
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) x &= 0x80000000u;
    else if (e == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

PS2_VU_INLINE std::uint32_t fout(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;
    return x;
}

PS2_VU_INLINE float norm(float f) { return fin(fout(f)); }
PS2_VU_INLINE std::uint32_t add(std::uint32_t a, std::uint32_t b) { return fout(fin(a) + fin(b)); }
PS2_VU_INLINE std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return fout(fin(a) - fin(b)); }
PS2_VU_INLINE std::uint32_t mul(std::uint32_t a, std::uint32_t b) { return fout(fin(a) * fin(b)); }
PS2_VU_INLINE std::uint32_t madd(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) + norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t msub(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) - norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t max(std::uint32_t a, std::uint32_t b) { return fp::max(a, b); }
PS2_VU_INLINE std::uint32_t min(std::uint32_t a, std::uint32_t b) { return fp::min(a, b); }

PS2_VU_INLINE void flags(Vu0Regs &r, const Reg128 &v, unsigned dest) {
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    r.mac = mac;
    std::uint32_t status = r.status & 0xFF0u;
    if (mac & 0x000Fu) status |= 0x1u;
    if (mac & 0x00F0u) status |= 0x2u;
    if (mac & 0x0F00u) status |= 0x4u;
    if (mac & 0xF000u) status |= 0x8u;
    status |= (status & 0xFu) << 6;
    r.status = (status & ~0x30u) | (r.status & 0x30u);
}

PS2_VU_INLINE void clip(Vu0Regs &r, const Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = fp::key(w & fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    r.clip = ((r.clip << 6) | f) & 0xFFFFFFu;
}

PS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) { return mem + (index & 1023u) * 16u; }
PS2_VU_INLINE void set_vi(Vu0Regs &r, unsigned i, std::uint32_t v) {
    if (i) r.vi[i] = static_cast<std::uint16_t>(v);
}

PS2_VU_INLINE std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

PS2_VU_INLINE float u2f(std::uint32_t v) {
    if (!(v & 0x7F800000u)) v &= 0x80000000u;
    if ((v & 0x7F800000u) == 0x7F800000u) v = (v & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

inline void efu(Vu0Regs &r, unsigned index, unsigned fs, unsigned fsf) {
    switch (index) {
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
    case 0x7C: r.p = f2u(std::sin(u2f(r.vf[fs].w[fsf]))); break;
    case 0x7D: r.p = f2u(std::atan(u2f(r.vf[fs].w[fsf]))); break;
    case 0x7E: r.p = f2u(std::exp(-u2f(r.vf[fs].w[fsf]))); break;
    default: break;
    }
}

}
