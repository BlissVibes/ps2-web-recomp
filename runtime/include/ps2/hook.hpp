#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "memory.hpp"
#include "system.hpp"

#include <cstdint>
#include <string>

namespace ps2 {

// A hook replaces one guest function. It reads its arguments from c (a0-a3, t0-t3 = c.r[4..11],
// f12-f19 = c.f[12..19], further arguments on the stack at c.r[29]), writes its results to c
// (v0 = c.r[2], v1 = c.r[3], f0 = c.f[0]; 32-bit integers sign-extended with set32()), may use
// the scratch registers (v0, v1, a0-a3, t0-t9, f0-f19, facc, fcr31) freely, must leave s0-s7,
// gp, sp, fp, ra (c.r[16..23], 28, 29, 30, 31) and f20-f31 as it found them, and must not touch
// c.pc: enter() sets c.pc = ra afterwards, which is how the recompiled caller sees the return.
using HookFn = void (*)(Cpu &c);

struct Hook {
    std::uint32_t address;
    const char *name;
    HookFn fn;
};

void register_hooks(const Hook *hooks, std::uint32_t count);   // registered but disabled
bool enable_hook(const std::string &name_or_hex_address);
void enable_all_hooks();
void set_entry_trace(bool on);
void set_entry_snapshot(std::uint32_t address, std::uint32_t every, std::uint32_t max, const std::string &dir);
// Shadow mode: enabled hooks do not serve their calls; on every `every`th call of a hook the recompiled
// function and the hook both run on the live call and their outcomes are compared (0 = off).
void set_hook_shadow(std::uint32_t every);
void report_hooks();   // one line per enabled hook on stderr: how many calls it served, or was compared on

// Defined in runtime/src/memory.cpp and declared in the private header runtime/src/hw.hpp, which
// decompiled code must not include; declared here as well.
std::uint32_t physical(std::uint32_t address);

// EE memory as the host sees it. RAM addresses are physical(a) & (kRamSize - 1), so KUSEG
// 0x00xxxxxx, uncached 0x2xxxxxxx and accelerated 0x3xxxxxxx all land in g_ram; the 16 KB
// scratchpad is a separate array. Scalars: ld8/ld16/ld32/ld64/ld128 and st* from memory.hpp.
inline std::uint8_t *host_ram(std::uint32_t a) { return g_ram + (physical(a) & (kRamSize - 1)); }
inline std::uint8_t *host_spr(std::uint32_t a) { return g_spr + (a & (kScratchpadSize - 1)); }

}
