#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>

namespace ps2::vu {

void reset();
void run(int unit, std::uint32_t address);
std::uint32_t tpc(int unit);
void set_tops(std::uint32_t top, std::uint32_t itop);
void code_changed(int unit);
Vu0Regs &regs(int unit);

}
