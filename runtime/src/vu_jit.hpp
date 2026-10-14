#pragma once

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

struct Layout {
    std::uint32_t flags = 0;
    std::uint32_t scratch = 0;
};

// Parts of the translation that can be switched off, to measure them or to rule them out. The page takes
// PS2_VU_JIT_OFF, a comma-separated list of the names in quotes (vu_jit_web.cpp).
struct Options {
    bool consts = true;     // "consts": recurring vector constants in locals set once at entry, not a v128.const at each use
    bool exact = true;      // "exact": SUB, MADD and MSUB through the VU adder as ADD and OPMSUB are, not with the host's rounding
    bool inlined = true;    // "inline": FTOI, ITOF, CLIP, OPMULA and OPMSUB as vector code, not calls lane by lane
};

enum Import : std::uint32_t {
    kClipValue, kClipPost, kSetClip, kStart, kMac, kStatus, kClip, kForceMac, kMul, kMsub, kToFixed, kFromFixed,
    kDiv, kSqrt, kRsqrt, kEfu, kKick, kTop, kItop, kRandNext, kInterpret, kImports
};

extern const char *const kImportNames[kImports];

bool translate(const std::uint8_t *code, std::uint32_t entry, const Layout &layout, std::vector<std::uint8_t> &out, const Options &options = {});

}
