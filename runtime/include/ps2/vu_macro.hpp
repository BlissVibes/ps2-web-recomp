#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "vu.hpp"
#include "vu_exact.hpp"

#include <cstdint>

namespace ps2::vu {

// The arithmetic of COP2 macro mode: four lanes of host floats with the bits of the integer model
// (ps2/vu_exact.hpp). When a lane in dest is one host floats cannot compute (an operand or a result in the
// top binade, or one product next to the smallest normal number) the whole instruction is computed with the
// model instead, lane by lane with fp::add, sub and mul as before. Nothing is written until one of the two has
// the result, so the model reads the registers as they were. (The FPU's add.s and mul.s stay on fp::add and
// fp::mul: for a single value the integer routines are cheaper than the exact host-float forms.)

// The lanes of a dest field as a mask: x from bit 3 down to w from bit 0.
inline vuops::v4u lanes(unsigned dest) {
    return (vuops::v4u)((vuops::v4u{dest, dest, dest, dest} & vuops::v4u{8u, 4u, 2u, 1u}) != 0u);
}

// The model: each lane of dest with fp::add, sub and mul, the way the recompiled code computed them before
// the four-lane forms. A call that falls back to it is not inlined: it is rare, and its code would only
// make every caller larger.
namespace model {

inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { apply(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void addbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) { apply(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], t); }); }
inline void sub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { apply(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void subbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) { apply(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], t); }); }
inline void mul(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { apply(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void mulbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) { apply(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], t); }); }
inline void madd(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { apply(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); }); }
inline void maddbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) { apply(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); }); }
inline void msub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) { apply(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); }); }
inline void msubbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) { apply(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); }); }
inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { apply_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void addabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) { apply_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], t); }); }
inline void suba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { apply_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void subabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) { apply_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], t); }); }
inline void mula(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { apply_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); }); }
inline void mulabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) { apply_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], t); }); }
inline void madda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { apply_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); }); }
inline void maddabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) { apply_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); }); }
inline void msuba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) { apply_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); }); }
inline void msubabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) { apply_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); }); }
inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    v.acc.w[0] = fp::mul(s.w[1], t.w[2]);
    v.acc.w[1] = fp::mul(s.w[2], t.w[0]);
    v.acc.w[2] = fp::mul(s.w[0], t.w[1]);
}
inline void opmsub(Vu0Regs &v, unsigned fd, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    Reg128 r = v.vf[fd];
    r.w[0] = fp::sub(v.acc.w[0], fp::mul(s.w[1], t.w[2]));
    r.w[1] = fp::sub(v.acc.w[1], fp::mul(s.w[2], t.w[0]));
    r.w[2] = fp::sub(v.acc.w[2], fp::mul(s.w[0], t.w[1]));
    if (fd) v.vf[fd] = r;
}

}

// The lanes of dest of an exact result into a register (no register at all for vf0), unless one of them is
// bad: then nothing is written and the caller computes the instruction with the model.
inline bool put(Reg128 &to, bool write, unsigned dest, vuops::v4u value, vuops::v4u bad) {
    const vuops::v4u mask = lanes(dest);
    if (vuexact::vbits((vuops::v4i)(bad & mask))) [[unlikely]] return false;
    if (write) vuops::vstore(to, vuops::vsel(vuops::vload(to), value, mask));
    return true;
}

// One lane, and known where the instruction is compiled in: the model computes that lane and nothing else,
// which is cheaper than four lanes of host floats. With more lanes, or with dest known only at run time (in
// the one out-of-line copy of an operation), the four lanes are cheaper.
inline bool one_known_lane(unsigned dest) { return __builtin_constant_p(dest) && (dest == 8u || dest == 4u || dest == 2u || dest == 1u); }

inline vuops::v4u vf(const Vu0Regs &v, unsigned r) { return vuops::vload(v.vf[r]); }

inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::add(v, dest, fd, fs, ft);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::add(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::add(v, dest, fd, fs, ft);
}
inline void addbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::addbc(v, dest, fd, fs, t);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::add(vf(v, fs), vuops::vsplat(t), bad), bad)) return;
    [[clang::noinline]] model::addbc(v, dest, fd, fs, t);
}
inline void sub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::sub(v, dest, fd, fs, ft);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::sub(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::sub(v, dest, fd, fs, ft);
}
inline void subbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::subbc(v, dest, fd, fs, t);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::sub(vf(v, fs), vuops::vsplat(t), bad), bad)) return;
    [[clang::noinline]] model::subbc(v, dest, fd, fs, t);
}
inline void mul(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::mul(v, dest, fd, fs, ft);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::mul(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::mul(v, dest, fd, fs, ft);
}
inline void mulbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::mulbc(v, dest, fd, fs, t);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::mul1(vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::mulbc(v, dest, fd, fs, t);
}
inline void madd(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::madd(v, dest, fd, fs, ft);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::madd(vuops::vload(v.acc), vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::madd(v, dest, fd, fs, ft);
}
inline void maddbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::maddbc(v, dest, fd, fs, t);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::madd1(vuops::vload(v.acc), vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::maddbc(v, dest, fd, fs, t);
}
inline void msub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::msub(v, dest, fd, fs, ft);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::msub(vuops::vload(v.acc), vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::msub(v, dest, fd, fs, ft);
}
inline void msubbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::msubbc(v, dest, fd, fs, t);
    vuops::v4u bad;
    if (put(v.vf[fd], fd, dest, vuexact::msub1(vuops::vload(v.acc), vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::msubbc(v, dest, fd, fs, t);
}
// MAX and MINI do no arithmetic (fp::max, fp::min): the four-lane compare has their bits in every lane.
inline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    put(v.vf[fd], fd, dest, vuops::vmax(vf(v, fs), vf(v, ft)), vuops::v4u{});
}
inline void maxbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    put(v.vf[fd], fd, dest, vuops::vmax(vf(v, fs), vuops::vsplat(t)), vuops::v4u{});
}
inline void mini(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    put(v.vf[fd], fd, dest, vuops::vmin(vf(v, fs), vf(v, ft)), vuops::v4u{});
}
inline void minibc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    put(v.vf[fd], fd, dest, vuops::vmin(vf(v, fs), vuops::vsplat(t)), vuops::v4u{});
}

inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::adda(v, dest, fs, ft);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::add(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::adda(v, dest, fs, ft);
}
inline void addabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::addabc(v, dest, fs, t);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::add(vf(v, fs), vuops::vsplat(t), bad), bad)) return;
    [[clang::noinline]] model::addabc(v, dest, fs, t);
}
inline void suba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::suba(v, dest, fs, ft);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::sub(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::suba(v, dest, fs, ft);
}
inline void subabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::subabc(v, dest, fs, t);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::sub(vf(v, fs), vuops::vsplat(t), bad), bad)) return;
    [[clang::noinline]] model::subabc(v, dest, fs, t);
}
inline void mula(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::mula(v, dest, fs, ft);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::mul(vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::mula(v, dest, fs, ft);
}
inline void mulabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::mulabc(v, dest, fs, t);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::mul1(vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::mulabc(v, dest, fs, t);
}
inline void madda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::madda(v, dest, fs, ft);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::madd(vuops::vload(v.acc), vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::madda(v, dest, fs, ft);
}
inline void maddabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::maddabc(v, dest, fs, t);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::madd1(vuops::vload(v.acc), vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::maddabc(v, dest, fs, t);
}
inline void msuba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    if (one_known_lane(dest)) return model::msuba(v, dest, fs, ft);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::msub(vuops::vload(v.acc), vf(v, fs), vf(v, ft), bad), bad)) return;
    [[clang::noinline]] model::msuba(v, dest, fs, ft);
}
inline void msubabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    if (one_known_lane(dest)) return model::msubabc(v, dest, fs, t);
    vuops::v4u bad;
    if (put(v.acc, true, dest, vuexact::msub1(vuops::vload(v.acc), vf(v, fs), t, bad), bad)) return;
    [[clang::noinline]] model::msubabc(v, dest, fs, t);
}

// OPMULA and OPMSUB: the cross product's terms, fs.yzx times ft.zxy, in x, y and z; w is left alone.
inline vuops::v4u yzx(vuops::v4u x) { return __builtin_shufflevector(x, x, 1, 2, 0, 3); }
inline vuops::v4u zxy(vuops::v4u x) { return __builtin_shufflevector(x, x, 2, 0, 1, 3); }

inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
    vuops::v4u bad;
    if (put(v.acc, true, 0xEu, vuexact::mul(yzx(vf(v, fs)), zxy(vf(v, ft)), bad), bad)) return;
    [[clang::noinline]] model::opmula(v, fs, ft);
}

inline void opmsub(Vu0Regs &v, unsigned fd, unsigned fs, unsigned ft) {
    vuops::v4u bad;
    if (put(v.vf[fd], fd, 0xEu, vuexact::msub(vuops::vload(v.acc), yzx(vf(v, fs)), zxy(vf(v, ft)), bad), bad)) return;
    [[clang::noinline]] model::opmsub(v, fd, fs, ft);
}

}
