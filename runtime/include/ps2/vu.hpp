#pragma once

#include "cpu.hpp"
#include "fpu.hpp"

#include <cstdint>

namespace ps2::vu {

inline bool lane(unsigned dest, int i) { return dest & (8u >> i); }

template <typename F>
inline void apply(Vu0Regs &v, unsigned dest, unsigned fd, F f) {
    Reg128 r = v.vf[fd];
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    if (fd) v.vf[fd] = r;
}

template <typename F>
inline void apply_acc(Vu0Regs &v, unsigned dest, F f) {
    Reg128 r = v.acc;
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    v.acc = r;
}

inline void abs(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i] & fp::kAbs; });
}
inline void move(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i]; });
}
inline void mr32(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    const Reg128 s = v.vf[fs];
    apply(v, dest, ft, [&](int i) { return s.w[(i + 1) & 3]; });
}
inline void ftoi(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::to_fixed(v.vf[fs].w[i], fraction); });
}
inline void itof(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::from_fixed(v.vf[fs].sw[i], fraction); });
}

inline void clip(Vu0Regs &v, unsigned fs, unsigned ft) {
    const std::int32_t w = fp::key(v.vf[ft].w[3] & fp::kAbs);
    std::uint32_t flags = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(v.vf[fs].w[i]);
        if (x > w) flags |= 1u << (i * 2);
        if (x < -w) flags |= 2u << (i * 2);
    }
    v.clip = ((v.clip << 6) | flags) & 0xFFFFFFu;
}

inline void div(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::div(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}
inline void sqrt(Vu0Regs &v, std::uint32_t t) { v.q = fp::sqrt(t); }
inline void rsqrt(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::rsqrt(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}

inline void mfir(Vu0Regs &v, unsigned dest, unsigned ft, unsigned is) {
    const std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.vi[is])));
    apply(v, dest, ft, [&](int) { return value; });
}

inline void set_vi(Vu0Regs &v, unsigned index, std::uint32_t value) {
    if (index) v.vi[index] = static_cast<std::uint16_t>(value);
}

inline std::uint32_t rand_next(std::uint32_t r) {
    const std::uint32_t bit = ((r >> 4) ^ (r >> 22)) & 1u;
    return ((r << 1) | bit) & 0x7FFFFFu;
}

}
