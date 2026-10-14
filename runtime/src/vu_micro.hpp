#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::vu {

void state(ps2::state::Stream &s);

void reset();
void run(int unit, std::uint32_t address);
std::uint32_t tpc(int unit);
void set_tops(std::uint32_t top, std::uint32_t itop);
void code_changed(int unit);
Vu0Regs &regs(int unit);
std::uint64_t interpreted_calls();
void stats();

// VU0 has two loops with the same outcome, bit for bit: the exact one, which decodes every instruction as
// it meets it and computes each lane with the integer model (ps2/fpu.hpp), and the fast one, which runs
// instructions decoded once per code image and computes with host floats (ps2/vu_exact.hpp). The
// environment picks at the first call:
//   PS2_VU0_CHECK=1                     both loops on every call and the whole state compared; the exact
//                                       loop's outcome is kept
//   PS2_VU0_CHECK=live                  the same, keeping the fast loop's outcome
//   PS2_VU0_EXACT=1 or PS2_VU_EXACT=1   the exact loop (PS2_VU_EXACT moves VU1 to its exact loop as well)
//   none of them                        the fast loop
enum class Vu0Mode { Fast, Exact, Check, CheckLive };
void set_vu0_mode(Vu0Mode mode);
Vu0Mode vu0_mode();

struct Vu0Counters {
    std::uint64_t calls, steps;      // VU0 calls, and the instructions they ran
    std::uint64_t slow, guarded;     // fast loop: instructions computed by the model after all; calls left to the exact loop
    std::uint64_t images, lookups;   // fast loop: code images decoded; searches for the image after a code change
    std::uint64_t compared, differ, stale, kicks_skipped;   // check modes
};
const Vu0Counters &vu0_counters();
// Check modes: called with the fast loop's outcome before it is compared, so a test can damage it.
void set_vu0_tamper(void (*tamper)(Vu0Regs &regs, std::uint8_t *mem));

}
