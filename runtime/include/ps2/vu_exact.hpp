#pragma once

#include "fpu.hpp"
#include "vu_ops.hpp"

#include <cstdint>
#include <cstring>

// The VU's adder and multiplier on four lanes of host floats, with the bits of the integer model
// (fp::add, fp::sub and fp::mul in fpu.hpp), not the host's rounding. The exact interpreter computes every
// lane with the model; these forms give the same bits several times cheaper.
//
// Every form takes register lanes as they are and gives the result with a `bad` mask: all ones in a lane
// the form cannot compute. In every other lane the result is the model's, bit for bit. The caller must
// compute a bad lane with the model. Bad lanes are the ones the host has no number for or rounds on its
// own terms: an operand or a result in the top binade (exponent field 255, a number on a VU, an infinity or
// a NaN on the host) and one product next to the smallest normal number. Ordinary numbers are never bad.
//
// Nothing here may be reordered or fused by the compiler: build with -ffp-contract=off and without
// -ffast-math. The host must round to nearest and keep its denormals (no flush-to-zero, no
// denormals-are-zero mode): the rounding error of a sum near the bottom of the range is one.
// tests/unit/test_vu_exact.cpp of the game repository compares every form with the model.

namespace ps2::vuexact {

using vuops::v4f;
using vuops::v4i;
using vuops::v4u;
typedef double v2d __attribute__((vector_size(16)));
typedef std::uint64_t v2q __attribute__((vector_size(16)));
typedef float v2f __attribute__((vector_size(8)));

// The sign bits of four lanes as a number, lane 0 in bit 0: which lanes of a mask are set.
PS2_VU_INLINE unsigned vbits(v4i m) {
#if defined(__SSE2__)
    return static_cast<unsigned>(__builtin_ia32_movmskps((v4f)m));
#elif defined(__wasm_simd128__)
    return static_cast<unsigned>(__builtin_wasm_bitmask_i32x4(m));
#else
    return (m[0] < 0 ? 1u : 0u) | (m[1] < 0 ? 2u : 0u) | (m[2] < 0 ? 4u : 0u) | (m[3] < 0 ? 8u : 0u);
#endif
}

// fp::flush on four lanes: an exponent field of zero is a zero that keeps its sign. It is also
// fp::mul(x, 1.0) for every x, so a product with 1.0 needs no multiply.
PS2_VU_INLINE v4u vflush(v4u x) {
    const v4u zero = (v4u)((x & 0x7F800000u) == 0u);
    return x & ~(zero & 0x7FFFFFFFu);
}

// The second operand of a product, widened: lanes 0 and 1, and lanes 2 and 3.
PS2_VU_INLINE v2d widen_low(v4f y) { return __builtin_convertvector(__builtin_shufflevector(y, y, 0, 1), v2d); }
PS2_VU_INLINE v2d widen_high(v4f y) { return __builtin_convertvector(__builtin_shufflevector(y, y, 2, 3), v2d); }
// One value for every lane (a broadcast lane, Q or I), flushed and widened once.
PS2_VU_INLINE v2d widen(std::uint32_t b) {
    b = fp::flush(b);
    float f;
    std::memcpy(&f, &b, 4);
    const double d = f;
    return v2d{d, d};
}

// fp::mul of flushed lanes a with the widened, flushed second operand. The multiplier keeps the top 24 bits
// of the 48-bit product of the two mantissas and drops the rest. A double holds that product exactly;
// clearing its low 29 bits leaves the same 24 bits, and what is left fits a float, so narrowing it does
// not round. A product too small for a float narrows to a denormal or to zero, which is flushed, as the
// model gives zero. bad: a result in the top binade (this covers an operand there, and a product past the
// top, where the model saturates) and the result 2^-126, because the one product a step under it, which the
// model makes a zero, is rounded up to it by the narrowing.
PS2_VU_INLINE v4u product(v4u a, v2d y0, v2d y1, v4u &bad) {
    const v4f x = (v4f)a;
    v2d p0 = widen_low(x) * y0;
    v2d p1 = widen_high(x) * y1;
    p0 = (v2d)((v2q)p0 & 0xFFFFFFFFE0000000ull);
    p1 = (v2d)((v2q)p1 & 0xFFFFFFFFE0000000ull);
    const v2f r0 = __builtin_convertvector(p0, v2f), r1 = __builtin_convertvector(p1, v2f);
    const v4u r = (v4u)__builtin_shufflevector(r0, r1, 0, 1, 2, 3);
    const v4u e = r & 0x7F800000u;
    bad = (v4u)(e == 0x7F800000u) | (v4u)((r & 0x7FFFFFFFu) == 0x00800000u);
    return r & ~((v4u)(e == 0u) & 0x7FFFFFFFu);
}

// fp::add of flushed lanes. The adder cuts the smaller operand off below half a unit in the last place of
// the larger, adds or subtracts, and truncates; the host rounds the exact sum to nearest. So the host sum
// is corrected by at most one unit in the last place, by the three rules written out at vuops::add_vu in
// vu_ops.hpp, with s the host sum and "away" meaning it was rounded away from zero:
//   - same signs: step back if away;
//   - opposite signs, sum below the larger operand's power of two: step forward unless away;
//   - opposite signs otherwise: step back if away on a tie.
// The operands are put larger magnitude first, so the rounding error of s comes exactly from three
// operations, and none of them can overflow while s is a number. (A two-sum that takes the operands in any
// order needs six operations, and one of those overflows beside the largest host float: 0x73C00000 +
// 0xFF7FFFFF came out one unit off that way. vuops::add_vu and vadd_vu have the steps used here.)
// bad: a host sum in the top binade, which covers an operand there. A sum too small for a float is exact
// on the host and is flushed, as the model gives zero.
PS2_VU_INLINE v4u sum(v4u a, v4u b, v4u &bad) {
    const v4u ab = a ^ b;
    const v4u sw = ab & (v4u)((v4i)(a & 0x7FFFFFFFu) < (v4i)(b & 0x7FFFFFFFu));
    const v4f x = (v4f)(a ^ sw), y = (v4f)(b ^ sw);
    const v4f s = x + y;
    const v4f err = y - (s - x);
    const v4f zero = {0.0f, 0.0f, 0.0f, 0.0f};
    const v4i inexact = (v4i)(err != zero);
    const v4i away = (v4i)((v4u)s ^ (v4u)err) < 0;
    const v4i opposite = (v4i)ab < 0;
    const v4i ms = (v4i)((v4u)s & 0x7FFFFFFFu);
    const v4i power = (v4i)((v4u)x & 0x7F800000u);
    // away is 0 or -1: below the power, or the power itself reached by rounding up.
    const v4i below = (ms + away) < power;
    const v4f twice = err + err;
    const v4i tie = (v4i)(((s + twice) - s) == twice);
    const v4i back = away & (~opposite | (~below & tie));
    const v4i forward = opposite & below & ~away;
    const v4u r = (v4u)((v4i)s + (inexact & (back - forward)));
    bad = (v4u)(((v4u)s & 0x7F800000u) == 0x7F800000u);
    return r & ~((v4u)((r & 0x7F800000u) == 0u) & 0x7FFFFFFFu);
}

PS2_VU_INLINE v4u mul(v4u a, v4u b, v4u &bad) {
    const v4f y = (v4f)vflush(b);
    return product(vflush(a), widen_low(y), widen_high(y), bad);
}
PS2_VU_INLINE v4u mul1(v4u a, std::uint32_t b, v4u &bad) {
    const v2d y = widen(b);
    return product(vflush(a), y, y, bad);
}
PS2_VU_INLINE v4u add(v4u a, v4u b, v4u &bad) { return sum(vflush(a), vflush(b), bad); }
PS2_VU_INLINE v4u sub(v4u a, v4u b, v4u &bad) { return sum(vflush(a), vflush(b) ^ 0x80000000u, bad); }

// The accumulating forms: the product, which comes flushed, goes into the adder, with its sign turned for
// MSUB. A lane is bad when either step is; a bad product may be an infinity or a NaN, and the sum of that
// lane is then bad by itself.
PS2_VU_INLINE v4u accumulate(v4u acc, v4u p, v4u bad_product, v4u &bad) {
    const v4u r = sum(vflush(acc), p, bad);
    bad |= bad_product;
    return r;
}
PS2_VU_INLINE v4u madd(v4u acc, v4u a, v4u b, v4u &bad) {
    v4u bad_product;
    const v4u p = mul(a, b, bad_product);
    return accumulate(acc, p, bad_product, bad);
}
PS2_VU_INLINE v4u msub(v4u acc, v4u a, v4u b, v4u &bad) {
    v4u bad_product;
    const v4u p = mul(a, b, bad_product);
    return accumulate(acc, p ^ 0x80000000u, bad_product, bad);
}
PS2_VU_INLINE v4u madd1(v4u acc, v4u a, std::uint32_t b, v4u &bad) {
    v4u bad_product;
    const v4u p = mul1(a, b, bad_product);
    return accumulate(acc, p, bad_product, bad);
}
PS2_VU_INLINE v4u msub1(v4u acc, v4u a, std::uint32_t b, v4u &bad) {
    v4u bad_product;
    const v4u p = mul1(a, b, bad_product);
    return accumulate(acc, p ^ 0x80000000u, bad_product, bad);
}

// The MAC flags of a result. lanes is the instruction's dest field times 0x1111: its lanes in each group of
// four bits, x in bit 3 down to w in bit 0. Zero (exponent field 0) in the low group, sign in the next,
// overflow (every magnitude bit set) in the top one; underflow is never set.
PS2_VU_INLINE std::uint32_t mac(v4u value, std::uint32_t lanes) {
    const v4u w = __builtin_shufflevector(value, value, 3, 2, 1, 0);
    const std::uint32_t z = vbits((v4i)((w & 0x7F800000u) == 0u));
    const std::uint32_t s = vbits((v4i)w);
    const std::uint32_t o = vbits((v4i)((w & 0x7FFFFFFFu) == 0x7FFFFFFFu));
    return (z | (s << 4) | (o << 12)) & lanes;
}

}
